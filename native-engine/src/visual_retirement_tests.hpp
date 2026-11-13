#pragma once
#include "visual_config_tests.hpp"
#include <winioctl.h>

inline bool visual_test_junction(const fs::path& link,const fs::path& target){
    fs::create_directory(link);auto destination=fs::absolute(target).wstring(),substitute=L"\\??\\"+destination;
    size_t names=(substitute.size()+1+destination.size()+1)*sizeof(wchar_t);
    std::vector<BYTE> data(16+names,0);auto dword=reinterpret_cast<DWORD*>(data.data());auto words=reinterpret_cast<WORD*>(data.data()+4);
    dword[0]=IO_REPARSE_TAG_MOUNT_POINT;words[0]=WORD(8+names);
    words[2]=0;words[3]=WORD(substitute.size()*2);words[4]=WORD((substitute.size()+1)*2);words[5]=WORD(destination.size()*2);
    memcpy(data.data()+16,substitute.c_str(),(substitute.size()+1)*2);
    memcpy(data.data()+16+(substitute.size()+1)*2,destination.c_str(),(destination.size()+1)*2);
    HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;DWORD returned=0;
    bool okay=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,data.data(),DWORD(data.size()),nullptr,0,&returned,nullptr)!=FALSE;
    CloseHandle(handle);return okay;
}

inline void visual_retirement_tests(const fs::path& output){
    fs::create_directories(output);ConfigState state(output/L"config.json");
    auto id=state.create(json::object(),state.revision);state.set_viewport(900,600);state.save();
    std::vector<BYTE> pixels(128,55);
    state.install_visual(id,{100,100,180,80},96,visual_test_relation(),16,8,pixels,state.revision);
    auto binding=state.document["regions"][0]["visual_binding"];
    auto key=binding["template_id"].get<std::string>();
    auto asset=output/L"visual-templates"/(wide(key)+L".vt1"),ledger=visual_retirement_ledger(state.path);
    auto refs=visual_asset_references(state.document);auto empty=json::object();
    // An anchor-mode region still holds its saved calibration reference.
    auto anchored=state.document;anchored["regions"][0]["position_mode"]="anchor";
    require(visual_asset_references(anchored)==refs,"anchor switch retired a saved calibration");
    auto before=state.document;
    HANDLE lock=CreateFileW(state.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"cannot lock retirement save fixture");
    state.document["regions"]=json::array();state.revision++;bool failed=false;
    try{state.save();}catch(...){failed=true;}CloseHandle(lock);
    require(failed&&!fs::exists(ledger)&&state.persistedVisualAssets==refs,"failed save retired an asset");
    state.document=before;state.revision--;state.save();
    // Successful replacement retires precisely the old asset, not the new one.
    state.install_visual(id,{100,100,180,80},96,visual_test_relation(),16,8,pixels,state.revision);
    json retired;{std::ifstream input(ledger);input>>retired;}
    require(retired["entries"].contains(key)&&fs::exists(asset),"replacement did not defer old asset");
    auto replacement=state.document["regions"][0]["visual_binding"]["template_id"].get<std::string>();
    require(!retired["entries"].contains(replacement),"replacement retired the current asset");
    // Use a deterministic clock only in the maintenance function, never by
    // changing the system clock or ageing any real user file.
    auto writeLedger=[&](const json& value){std::ofstream(ledger)<<value.dump();};
    retired["entries"][key]["since"]=1000;writeLedger(retired);
    auto collect=[&](int64_t now){visual_retire_assets(state.path,empty,visual_asset_references(state.document),new_id(),now);};
    state.update(id,{{"text","style-save-does-not-rescan"}},state.revision);state.save();
    require(fs::exists(asset),"frequent style save bypassed daily collection throttle");
    collect(1000+7*86400-1);require(fs::exists(asset),"asset removed before seven days");
    fs::create_directories(output/L"backups"/L"nested");auto backup=output/L"backups"/L"nested"/L"saved.JSON";
    {std::ofstream stream(backup);stream<<before.dump();}
    collect(1000+7*86400);require(fs::exists(asset),"nested backup reference was ignored");
    // Fixture files only; no recursive delete and no path outside this test root.
    fs::remove(backup);
    auto textBackup=output/L"saved-workspace";{std::ofstream stream(textBackup);stream<<before.dump();}
    collect(1000+7*86400);require(fs::exists(asset),"extensionless backup reference was ignored");fs::remove(textBackup);
    auto opaque=output/L"opaque.txt";{std::ofstream stream(opaque,std::ios::binary);stream.put('\0');}
    collect(1000+7*86400);require(fs::exists(asset),"opaque backup did not defer collection");fs::remove(opaque);
    auto junction=output/L"linked-backup";
    require(visual_test_junction(junction,output/L"backups"),"cannot create isolated junction fixture");
    collect(1000+7*86400);require(fs::exists(asset),"junction did not defer collection");
    require(RemoveDirectoryW(junction.c_str())!=FALSE,"cannot remove isolated junction itself");
    auto archive=output/L"backup.ZIP";{std::ofstream stream(archive);stream<<"unknown backup";}
    collect(1000+7*86400);require(fs::exists(asset),"archive did not defer collection");fs::remove(archive);
    auto unknown=asset.parent_path()/(wide(new_id())+L".vt1");fs::copy_file(asset,unknown);
    auto originalSize=fs::file_size(asset);{std::ofstream stream(asset,std::ios::binary|std::ios::app);stream<<"changed";}
    collect(1000+7*86400);require(fs::exists(asset),"changed asset was removed");
    fs::resize_file(asset,originalSize);
    {std::ofstream stream(ledger);stream<<"broken ledger";}
    collect(1000+7*86400);require(fs::exists(asset),"corrupt ledger allowed collection");writeLedger(retired);
    auto oversize=output/L"oversize.json";{std::ofstream stream(oversize,std::ios::binary);stream.seekp(2097152);stream.put('x');}
    collect(1000+7*86400);require(fs::exists(asset),"oversize reference scan allowed collection");fs::remove(oversize);
    lock=CreateFileW(asset.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"cannot lock template fixture");collect(1000+7*86400);
    require(fs::exists(asset),"in-use asset was collected");CloseHandle(lock);
    // A locked backup must defer deletion instead of assuming it has no refs.
    {std::ofstream stream(backup);stream<<"{}";}
    lock=CreateFileW(backup.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"cannot lock backup fixture");collect(1000+7*86400);
    require(fs::exists(asset),"unreadable backup allowed collection");CloseHandle(lock);fs::remove(backup);
    // Restore a reference and verify its old retirement timestamp is revoked.
    visual_retire_assets(state.path,empty,refs,new_id(),1000+7*86400);
    {std::ifstream input(ledger);input>>retired;}
    require(!retired["entries"].contains(key)&&fs::exists(asset),"restored reference retained retirement timestamp");
    auto otherApplications=state.path.parent_path()/L"applications";
    auto otherConfig=otherApplications/L"other-profile"/L"config.json";
    auto otherAsset=otherConfig.parent_path()/L"visual-templates"/asset.filename();
    fs::create_directories(otherAsset.parent_path());fs::copy_file(asset,otherAsset);
    {std::ofstream sibling(otherConfig);sibling<<before.dump();}
    visual_retire_assets(state.path,refs,empty,new_id(),2000);
    visual_retire_assets(state.path,empty,visual_asset_references(state.document),new_id(),2000+7*86400,true,otherApplications);
    require(!fs::exists(asset)&&fs::exists(unknown),"expired managed asset not collected or unknown asset removed");
    require(visual_asset_valid(otherConfig,binding),"legacy profile collection changed another application's template");
    require(visual_asset_valid(state.path,state.document["regions"][0]["visual_binding"]),"current asset damaged by maintenance");
    state.erase(id,state.revision);state.save();
    {std::ifstream input(ledger);input>>retired;}
    require(retired["entries"].contains(replacement),"successful region deletion did not defer its template");
    std::ofstream(output/L"result.json")<<json{{"passed",true},{"successful_save_only",true},{"seven_day_delay",true},
        {"anchor_and_restored_reference_preserved",true},{"nested_backup_and_archive_preserved",true},
        {"changed_asset_corrupt_ledger_and_uncertain_scan_preserved",true},{"unknown_and_current_assets_preserved",true},
        {"junction_and_in_use_asset_preserved",true},{"daily_scan_throttle",true},{"other_application_assets_isolated",true}}.dump(2);
}
