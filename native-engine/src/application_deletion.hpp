#pragma once
#include "pipe.hpp"

// All files are opened for deletion before any file is removed. Directory
// handles prevent a configuration path being replaced with a junction.
struct ApplicationDeletion {
    std::vector<std::unique_ptr<WinHandle>> ancestors,files,readers;
    std::set<fs::path> lockedDirectories;
    fs::path assetDirectory;
    std::unique_ptr<WinHandle> assetLock;
    size_t scannedBytes=0,scannedEntries=0;

    std::unique_ptr<WinHandle> open(const fs::path& path,DWORD access,bool directory){
        auto handle=std::make_unique<WinHandle>(CreateFileW(path.c_str(),access,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr));
        if(handle->value==INVALID_HANDLE_VALUE)throw std::runtime_error("application_file_in_use");
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle->value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
            bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory)
            throw std::runtime_error("application_path_unsafe");
        return handle;
    }
    void lock_ancestors(const fs::path& directory){
        std::vector<fs::path> paths;
        for(auto path=directory;!path.empty();){
            paths.push_back(path);auto parent=path.parent_path();if(parent==path)break;path=parent;
        }
        for(auto it=paths.rbegin();it!=paths.rend();++it)if(lockedDirectories.insert(*it).second)
            ancestors.push_back(open(*it,FILE_READ_ATTRIBUTES,true));
    }
    bool exists(const fs::path& path){
        DWORD attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES)return true;
        auto error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return false;
        throw std::runtime_error("application_path_unreadable");
    }
    void add_file(const fs::path& path){
        if(exists(path))files.push_back(open(path,DELETE|FILE_READ_ATTRIBUTES,false));
    }
    static bool same_path(const fs::path& first,const fs::path& second){
        return _wcsicmp(first.c_str(),second.c_str())==0;
    }
    static bool contains_path(const std::set<fs::path>& paths,const fs::path& path){
        return std::any_of(paths.begin(),paths.end(),[&](const auto& entry){return same_path(entry,path);});
    }
    std::string read(const fs::path& path){
        auto handle=open(path,GENERIC_READ,false);LARGE_INTEGER size{};
        if(!GetFileSizeEx(handle->value,&size)||size.QuadPart<0||size.QuadPart>16777216||
            scannedBytes+size_t(size.QuadPart)>67108864)throw std::runtime_error("application_reference_scan_too_large");
        std::string bytes(size_t(size.QuadPart),'\0');DWORD count=0;
        if(!ReadFile(handle->value,bytes.data(),DWORD(bytes.size()),&count,nullptr)||count!=bytes.size())
            throw std::runtime_error("application_reference_unreadable");
        scannedBytes+=bytes.size();readers.push_back(std::move(handle));
        auto parsed=json::parse(bytes,nullptr,false);
        return parsed.is_discarded()?bytes:parsed.dump();
    }
    void scan_references(const fs::path& path,const fs::path& applications,const fs::path& assets,
                         const fs::path& registry,const std::set<fs::path>& owned,std::string& references){
        for(const auto& entry:fs::directory_iterator(path)){
            if(++scannedEntries>4096)throw std::runtime_error("application_reference_scan_too_large");
            auto file=entry.path();
            if(same_path(file,applications)||same_path(file,assets)||same_path(file,registry)||contains_path(owned,file))continue;
            if(entry.is_directory()){
                auto handle=open(file,FILE_READ_ATTRIBUTES,true);
                scan_references(file,applications,assets,registry,owned,references);
                ancestors.push_back(std::move(handle));
            }else if(_wcsicmp(file.extension().c_str(),L".json")==0)references+=read(file);
        }
    }
    void add_profile(const fs::path& config,const fs::path& applications,const fs::path& registry){
        auto root=config.parent_path(),assets=root/L"visual-templates";
        std::set<fs::path> owned{config,visual_retirement_ledger(config)};
        auto prefix=config.filename().wstring()+L".pre-visual-v1.";
        for(const auto& entry:fs::directory_iterator(root)){
            auto name=entry.path().filename().wstring();
            if(name.size()==prefix.size()+64+5&&_wcsnicmp(name.c_str(),prefix.c_str(),prefix.size())==0&&
                _wcsicmp(name.c_str()+name.size()-5,L".json")==0&&visual_hex(utf8(name.substr(prefix.size(),64)),64))
                owned.insert(entry.path());
        }
        std::string references;
        scan_references(root,applications,assets,registry,owned,references);
        if(exists(assets)){
            assetDirectory=assets;
            assetLock=open(assets,FILE_READ_ATTRIBUTES,true);
            for(const auto& entry:fs::directory_iterator(assets)){
                if(!contains_path(owned,entry.path())&&_wcsicmp(entry.path().extension().c_str(),L".json")==0)
                    references+=read(entry.path());
            }
            auto lower_ascii=[](char ch){return ch>='A'&&ch<='Z'?char(ch-'A'+'a'):ch;};
            std::transform(references.begin(),references.end(),references.begin(),lower_ascii);
            for(const auto& entry:fs::directory_iterator(assets)){
                auto name=utf8(entry.path().stem().wstring());
                std::transform(name.begin(),name.end(),name.begin(),lower_ascii);
                if(_wcsicmp(entry.path().extension().c_str(),L".vt1")==0&&visual_hex(name,32)&&
                    references.find(name)==std::string::npos)add_file(entry.path());
            }
        }
        for(const auto& file:owned)add_file(file);
    }
    void execute(){
        FILE_DISPOSITION_INFO disposition{TRUE};
        for(auto& file:files)if(!SetFileInformationByHandle(file->value,FileDispositionInfo,&disposition,sizeof(disposition)))
            throw std::runtime_error("application_delete_failed");
        files.clear();
        if(assetLock){
            readers.clear();
            assetLock.reset();
            if(!RemoveDirectoryW(assetDirectory.c_str())&&GetLastError()!=ERROR_DIR_NOT_EMPTY)
                throw std::runtime_error("application_directory_delete_failed");
        }
    }
};
