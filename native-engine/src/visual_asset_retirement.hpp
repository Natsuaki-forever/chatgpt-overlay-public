#pragma once
#include "visual_config.hpp"

inline json visual_asset_references(const json& document){
    json refs=json::object();
    for(const auto& r:document.at("regions"))if(r.contains("visual_binding")&&!r["visual_binding"].is_null()){
        const auto& binding=r["visual_binding"];refs[binding.at("template_id").get<std::string>()]=binding;
    }
    return refs;
}
inline fs::path visual_retirement_ledger(const fs::path& config){
    return config.parent_path()/L"visual-templates"/(L".retired-"+config.filename().wstring()+L".json");
}
// Only successful saves retire known assets. Maintenance is best effort; an
// uncertain reference, file identity or backup always prevents deletion.
inline void visual_retire_assets(const fs::path& config,const json& previous,const json& current,
                                 const std::string& nonce,int64_t now,bool allowCollection=true,
                                 const fs::path& otherApplicationDirectory={})noexcept{
    try{
        auto root=config.parent_path().lexically_normal(),directory=root/L"visual-templates";
        if(!fs::exists(directory))return;
        auto safe=[](const fs::path& p){DWORD a=GetFileAttributesW(p.c_str());return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);};
        // Hold every path component from the volume root down. Denying write
        // and delete sharing prevents a directory being renamed or converted
        // into a junction between validation and opening a candidate file.
        struct DirectoryLocks {std::vector<HANDLE> handles;~DirectoryLocks(){for(auto h:handles)CloseHandle(h);}} locks;
        std::vector<fs::path> ancestors;
        for(auto p=directory;!p.empty();){ancestors.push_back(p);auto parent=p.parent_path();if(parent==p)break;p=parent;}
        for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
            HANDLE h=CreateFileW(it->c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(h==INVALID_HANDLE_VALUE)return;locks.handles.push_back(h);
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(h,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||
                (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
        }
        auto ledger=visual_retirement_ledger(config);json retired=json::object();
        if(fs::exists(ledger)){
            if(!safe(ledger)||fs::file_size(ledger)>262144)return;
            json data;std::ifstream input(ledger);input>>data;
            if(data.at("format")!="overlay-retired-assets-1"||!data.at("entries").is_object())return;
            retired=data["entries"];if(retired.size()>512)return;
            for(auto it=retired.begin();it!=retired.end();++it){
                validate_visual_binding(it.value().at("binding"));
                if(it.key()!=it.value()["binding"]["template_id"]||!it.value().at("since").is_number_integer())return;
            }
        }
        auto original=retired;
        for(auto it=previous.begin();it!=previous.end();++it)if(!current.contains(it.key())&&!retired.contains(it.key()))
            retired[it.key()]={{"binding",it.value()},{"since",now}};
        for(auto it=current.begin();it!=current.end();++it)retired.erase(it.key());
        if(retired.size()>512)return;
        std::vector<std::string> aged;
        for(auto it=retired.begin();it!=retired.end();++it){
            auto since=it.value()["since"].get<int64_t>();
            if(allowCollection&&since>=0&&now>=since&&now-since>=7*86400)aged.push_back(it.key());
        }
        // Recheck local text files and configuration backups immediately before
        // collection. A bounded scan never follows a junction or symbolic link.
        bool certain=true;size_t entries=0,bytes=0;std::string references;
        if(!aged.empty())for(fs::recursive_directory_iterator it(root),end;it!=end;++it){
            if(++entries>2048||!safe(it->path())){certain=false;break;}
            if(it->is_directory()){
                if(it->path()==otherApplicationDirectory)it.disable_recursion_pending();
                continue;
            }
            if(it->path()==ledger)continue;
            auto name=it->path().filename().wstring();std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return wchar_t(towlower(c));});
            auto extension=fs::path(name).extension().wstring();
            if(it->path().parent_path()==directory&&extension==L".vt1")continue;
            if(extension==L".zip"||extension==L".7z"||extension==L".rar"||extension==L".gz"||
                extension==L".xz"||extension==L".tar"||extension==L".cab"||extension==L".bz2"||extension==L".zst"){certain=false;break;}
            auto size=it->file_size();if(size>2097152||bytes+size>8388608){certain=false;break;}bytes+=size;
            std::ifstream input(it->path(),std::ios::binary);std::string content(size,'\0');
            if(!input.read(content.data(),size)||content.find('\0')!=std::string::npos||
                (!content.empty()&&!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,content.data(),int(content.size()),nullptr,0))){certain=false;break;}
            references+=content;
        }
        if(certain)for(const auto& key:aged){
            if(references.find(key)!=std::string::npos)continue;
            auto binding=retired[key]["binding"];if(!visual_asset_valid(config,binding))continue;
            auto file=directory/(wide(key)+L".vt1");
            struct OpenFile {HANDLE value=INVALID_HANDLE_VALUE;~OpenFile(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} handle;
            handle.value=CreateFileW(file.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(handle.value==INVALID_HANDLE_VALUE)continue;
            BY_HANDLE_FILE_INFORMATION info{};bool valid=GetFileInformationByHandle(handle.value,&info)&&
                !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&info.nFileSizeHigh==0&&
                info.nFileSizeLow>=13&&info.nFileSizeLow<=131084;
            std::vector<BYTE> data(valid?info.nFileSizeLow:0);DWORD read=0;
            valid=valid&&ReadFile(handle.value,data.data(),DWORD(data.size()),&read,nullptr)&&read==data.size()&&
                visual_sha256(data)==binding["template_sha256"];
            FILE_DISPOSITION_INFO disposition{TRUE};
            if(valid&&SetFileInformationByHandle(handle.value,FileDispositionInfo,&disposition,sizeof(disposition)))retired.erase(key);
        }
        if(retired==original)return;
        auto text=json{{"format","overlay-retired-assets-1"},{"entries",retired}}.dump();
        if(text.size()>262144)return;
        visual_atomic_write(ledger,std::vector<BYTE>(text.begin(),text.end()),nonce,true);
    }catch(...){/* Maintenance cannot undo a successful user save. */}
}
