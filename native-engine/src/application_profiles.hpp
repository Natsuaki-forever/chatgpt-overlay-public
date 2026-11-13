#pragma once
#include "application_deletion.hpp"
#include <appmodel.h>

struct WindowIdentity {
    static constexpr wchar_t propertyName[]=L"ChatGPTOverlay.WindowIdentity.v1";
    HWND window=nullptr;
    DWORD pid=0;
    uint64_t started=0;
    uintptr_t propertyToken=0;
    std::wstring executable,windowClass,appUserModelId;
    static WindowIdentity read(HWND window){
        WindowIdentity result;result.window=window;result.pid=process_id(window);
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,result.pid);
        if(!process)return result;
        wchar_t path[32768]{};DWORD length=32768;FILETIME created{},ended{},kernel{},user{};
        if(QueryFullProcessImageNameW(process,0,path,&length)&&GetProcessTimes(process,&created,&ended,&kernel,&user)){
            result.executable.assign(path,length);
            result.started=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
            wchar_t appId[256]{};UINT32 appIdLength=256;
            if(GetApplicationUserModelId(process,&appIdLength,appId)==ERROR_SUCCESS)result.appUserModelId=appId;
        }
        CloseHandle(process);
        wchar_t name[256]{};GetClassNameW(window,name,256);result.windowClass=name;
        return result;
    }
    bool current()const{
        if(!IsWindow(window)||executable.empty()||!started||
            (propertyToken&&propertyToken!=reinterpret_cast<uintptr_t>(GetPropW(window,propertyName))))return false;
        auto now=read(window);
        return pid==now.pid&&started==now.started&&windowClass==now.windowClass&&
            _wcsicmp(executable.c_str(),now.executable.c_str())==0;
    }
    void mark(){
        if(!current())return;
        propertyToken=reinterpret_cast<uintptr_t>(GetPropW(window,propertyName));
        if(propertyToken)return;
        auto token=static_cast<uintptr_t>(std::stoull(new_id().substr(0,sizeof(uintptr_t)*2),nullptr,16))|1;
        if(SetPropW(window,propertyName,reinterpret_cast<HANDLE>(token)))propertyToken=token;
    }
};

inline bool application_window(HWND window,bool includeMinimized=false){
    if(!available(window,includeMinimized)||GetAncestor(window,GA_ROOT)!=window||GetWindow(window,GW_OWNER)||process_id(window)==GetCurrentProcessId())return false;
    if(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)return false;
    if(!IsIconic(window)){
        RECT client{};
        if(!GetClientRect(window,&client)||IsRectEmpty(&client))return false;
    }
    wchar_t name[256]{};GetClassNameW(window,name,256);
    return wcscmp(name,L"Progman")&&wcscmp(name,L"WorkerW")&&wcscmp(name,L"Shell_TrayWnd")&&
        wcscmp(name,L"Shell_SecondaryTrayWnd");
}

inline bool identifiable_application(const WindowIdentity& identity){
    if(!identity.started||identity.executable.empty()||identity.windowClass.empty())return false;
    return _wcsicmp(fs::path(identity.executable).filename().c_str(),L"ApplicationFrameHost.exe")!=0;
}

struct ApplicationProfiles {
    fs::path legacyConfig,path;
    json document;
    std::wstring testWindowPrefix;
    explicit ApplicationProfiles(const fs::path& config,const std::wstring& testPrefix={}):legacyConfig(fs::absolute(config).lexically_normal()),
        path(application_directory().wstring()+L".json"),testWindowPrefix(testPrefix){
        document={{"version",1},{"active","chatgpt"},{"applications",json::array({
            {{"id","chatgpt"},{"name","ChatGPT"},{"executable",""}}})}};
        if(!fs::exists(path))return;
        std::ifstream input(path);if(!input)throw std::runtime_error("Cannot read application profiles");
        input>>document;
        if(!document.is_object()||document.value("version",0)!=1||!document.at("applications").is_array())
            throw std::runtime_error("Invalid application profiles");
        std::set<std::string> ids;
        for(const auto& app:document["applications"]){
            auto id=app.at("id").get<std::string>();
            if((id!="chatgpt"&&!visual_hex(app["id"],32))||!ids.insert(id).second||
                !app.at("name").is_string()||!app.at("executable").is_string()||(app.contains("aumid")&&!app["aumid"].is_string())||
                (app.contains("listed")&&!app["listed"].is_boolean())||
                (app.contains("deleting")&&!app["deleting"].is_boolean()))
                throw std::runtime_error("Invalid application profile");
            if(id!="chatgpt"&&!fs::path(wide(app["executable"].get<std::string>())).is_absolute())
                throw std::runtime_error("Missing application identity");
            if(app.contains("windows")){
                if(!app["windows"].is_array())throw std::runtime_error("Invalid window profiles");
                std::set<std::string> windowIds;
                for(const auto& window:app["windows"]){
                    if(!visual_hex(window.at("id"),32)||!window.at("title").is_string()||
                        (window.contains("deleting")&&!window["deleting"].is_boolean())||
                        !windowIds.insert(window.at("id").get<std::string>()).second)
                        throw std::runtime_error("Invalid window profile");
                }
            }
        }
        if(listed_count()>128||(!active().empty()&&(!ids.count(active())||!listed(app(active())))))
            throw std::runtime_error("Invalid active application");
    }
    std::string active()const{return document.at("active").get<std::string>();}
    WindowIdentity restore_window(const std::string& id,const json& savedWindow,std::string& profile)const{
        const auto& application=app(id);
        if(!savedWindow.is_object()||!savedWindow.contains("profile")||!savedWindow["profile"].is_string()||
            !savedWindow.contains("hwnd")||!savedWindow["hwnd"].is_number_unsigned()||
            !savedWindow.contains("token")||!savedWindow["token"].is_number_unsigned())return {};
        auto window=reinterpret_cast<HWND>(savedWindow["hwnd"].get<uintptr_t>());
        auto token=savedWindow["token"].get<uintptr_t>();
        if(!token||token!=reinterpret_cast<uintptr_t>(GetPropW(window,WindowIdentity::propertyName))||
            !accepts(window,true))return {};
        auto identity=WindowIdentity::read(window);
        identity.propertyToken=token;
        if(!identity.current()||!matches(application,identity))return {};
        auto saved=savedWindow["profile"].get<std::string>();
        if(saved!="legacy"){
            auto windows=application.value("windows",json::array());
            if(std::none_of(windows.begin(),windows.end(),[&](const auto& entry){
                return entry.at("id")==saved&&!entry.value("deleting",false);
            }))return {};
        }
        if(!fs::exists(scoped_config_path(id))&&!fs::exists(window_config_path(id,saved)))return {};
        profile=std::move(saved);
        return identity;
    }
    void remember_window(const std::string& id,const WindowIdentity& identity,const std::string& profile,bool active=true){
        for(auto& entry:document.at("applications"))if(entry.at("id")==id){
            auto handle=reinterpret_cast<uintptr_t>(identity.window);
            json saved={{"hwnd",handle},
                {"token",identity.propertyToken},{"profile",profile}};
            if(active)entry["last_window"]=saved;
            auto& bindings=entry["window_bindings"];
            if(!bindings.is_object())bindings=json::object();
            for(auto it=bindings.begin();it!=bindings.end();){
                const auto& binding=it.value();
                if(!binding.is_object()||!binding.contains("hwnd")||!binding["hwnd"].is_number_unsigned()||
                    !binding.contains("token")||!binding["token"].is_number_unsigned()){
                    it=bindings.erase(it);continue;
                }
                auto hwnd=binding["hwnd"].get<uintptr_t>();
                auto token=binding["token"].get<uintptr_t>();
                if(hwnd==handle||!token||token!=reinterpret_cast<uintptr_t>(GetPropW(reinterpret_cast<HWND>(hwnd),WindowIdentity::propertyName)))
                    it=bindings.erase(it);
                else ++it;
            }
            if(identity.propertyToken)bindings[profile]=std::move(saved);
            return;
        }
    }
    static bool listed(const json& entry){return entry.value("listed",true)&&!entry.value("deleting",false);}
    size_t listed_count()const{
        return std::count_if(document.at("applications").begin(),document.at("applications").end(),
            [](const auto& entry){return listed(entry);});
    }
    fs::path application_directory()const{
        auto name=legacyConfig.filename().wstring();
        return legacyConfig.parent_path()/(_wcsicmp(name.c_str(),L"config.json")==0?L"applications":name+L".applications");
    }
    const json& app(const std::string& id)const{
        for(const auto& entry:document["applications"])if(entry["id"]==id)return entry;
        throw std::runtime_error("Application profile not found");
    }
    fs::path config_path(const std::string& id)const{
        app(id);
        return id=="chatgpt"?legacyConfig:application_directory()/wide(id)/L"config.json";
    }
    fs::path window_directory(const std::string& id)const{
        return application_directory()/wide(id)/L"windows";
    }
    fs::path scoped_config_path(const std::string& id)const{
        return application_directory()/wide(id)/L"scoped"/L"config.json";
    }
    fs::path ensure_scoped_config(const std::string& id)const{
        auto path=scoped_config_path(id);
        if(fs::exists(path)){
            {
                std::ifstream input(path);
                auto saved=json::parse(input,nullptr,false);
                if(saved.is_object()&&saved.value("legacy_application_scope_default",false))return path;
            }
            ConfigState state(path);
            std::set<std::string> originalIds;
            auto original=config_path(id);
            if(fs::exists(original)){
                std::ifstream input(original);
                auto saved=json::parse(input,nullptr,false);
                if(saved.is_object()&&saved.contains("regions")&&saved["regions"].is_array())
                    for(const auto& region:saved["regions"])
                        if(region.is_object()&&region.contains("id")&&region["id"].is_string())
                            originalIds.insert(region["id"].get<std::string>());
            }
            for(auto& region:state.document["regions"])
                if(region.value("window_profile",std::string())=="legacy"&&
                    originalIds.count(region.at("id").get<std::string>()))region.erase("window_profile");
            state.document["legacy_application_scope_default"]=true;
            state.revision++;
            state.save();
            return path;
        }
        const auto& application=app(id);
        json migrated={{"version",1},{"regions",json::array()},{"legacy_application_scope_default",true}};
        std::set<std::string> ids;
        auto import=[&](const fs::path& source,const std::string& scope){
            if(!fs::exists(source))return;
            ConfigState state(source);
            if(state.document["version"]==2)migrated["version"]=2;
            if(scope.empty())migrated["visual_check_interval_seconds"]=state.visual_check_interval();
            for(auto region:state.document["regions"]){
                auto id=region.at("id").get<std::string>();
                if(!ids.insert(id).second){id=new_id();ids.insert(id);region["id"]=id;}
                if(!scope.empty())region["window_profile"]=scope;
                else region.erase("window_profile");
                auto& binding=region["visual_binding"];
                if(!binding.is_null()){
                    auto from=source.parent_path()/L"visual-templates"/(wide(binding["template_id"].get<std::string>())+L".vt1");
                    auto templateId=new_id();
                    if(visual_asset_valid(source,binding)){
                        auto to=path.parent_path()/L"visual-templates"/(wide(templateId)+L".vt1");
                        fs::create_directories(to.parent_path());
                        fs::copy_file(from,to);
                        binding["template_id"]=templateId;
                        if(!visual_asset_valid(path,binding))throw std::runtime_error("template_copy_failed");
                    }else binding["template_id"]=templateId;
                }
                migrated["regions"].push_back(std::move(region));
            }
        };
        import(config_path(id),"");
        for(const auto& window:application.value("windows",json::array())){
            if(!window.value("deleting",false)){
                auto profile=window.at("id").get<std::string>();
                import(window_config_path(id,profile),profile);
            }
        }
        auto bytes=migrated.dump(2);
        fs::create_directories(path.parent_path());
        visual_atomic_write(path,std::vector<BYTE>(bytes.begin(),bytes.end()),new_id());
        return path;
    }
    fs::path window_config_path(const std::string& id,const std::string& window)const{
        if(window=="legacy")return config_path(id);
        window_profile(id,window);
        return window_directory(id)/wide(window)/L"config.json";
    }
    const json& window_profile(const std::string& id,const std::string& window)const{
        const auto& application=app(id);
        if(application.contains("windows"))
            for(const auto& entry:application.at("windows"))if(entry.at("id")==window)return entry;
        throw std::runtime_error("Window configuration not found");
    }
    std::string create_window_profile(const std::string& id,HWND window){
        auto profile=new_id();
        wchar_t title[181]{};
        GetWindowTextW(window,title,181);
        for(auto& entry:document["applications"])if(entry["id"]==id){
            if(!entry.contains("windows"))entry["windows"]=json::array();
            entry["windows"].push_back({{"id",profile},{"title",utf8(title)}});
            return profile;
        }
        throw std::runtime_error("Application profile not found");
    }
    bool matches(const json& entry,const WindowIdentity& identity)const{
        if(!identifiable_application(identity))return false;
        auto aumid=entry.value("aumid",std::string());
        if(!aumid.empty())return _wcsicmp(wide(aumid).c_str(),identity.appUserModelId.c_str())==0;
        auto executable=entry.at("executable").get<std::string>();
        if(executable.empty()&&entry["id"]=="chatgpt")return _wcsicmp(fs::path(identity.executable).filename().c_str(),L"ChatGPT.exe")==0;
        return _wcsicmp(wide(executable).c_str(),identity.executable.c_str())==0;
    }
    std::string remember(const WindowIdentity& identity){
        for(auto& entry:document["applications"])if(matches(entry,identity)){
            if(entry.value("deleting",false))throw std::runtime_error("请先在应用菜单中完成删除，再重新点选。");
            if(!listed(entry)&&listed_count()>=128)throw std::runtime_error("Application profile limit reached");
            entry["listed"]=true;
            if(entry["executable"]==""){
                entry["executable"]=utf8(identity.executable);
                if(!identity.appUserModelId.empty())entry["aumid"]=utf8(identity.appUserModelId);
            }
            return entry["id"];
        }
        if(listed_count()>=128)throw std::runtime_error("Application profile limit reached");
        bool legacy=_wcsicmp(fs::path(identity.executable).filename().c_str(),L"ChatGPT.exe")==0&&
            std::none_of(document["applications"].begin(),document["applications"].end(),
                [](const auto& app){return app["id"]=="chatgpt";});
        auto id=legacy?std::string("chatgpt"):new_id();
        document["applications"].push_back({{"id",id},{"name",utf8(fs::path(identity.executable).stem().wstring())},
            {"executable",utf8(identity.executable)}});
        if(!identity.appUserModelId.empty())document["applications"].back()["aumid"]=utf8(identity.appUserModelId);
        return id;
    }
    void remove_current(){
        auto id=active();
        if(id.empty())throw std::runtime_error("No active application");
        for(auto& entry:document["applications"])if(entry["id"]==id)entry["listed"]=false;
        document["active"]="";
    }
    void begin_deletion(const std::string& id){
        app(id);
        for(auto& entry:document["applications"])if(entry["id"]==id){
            entry["listed"]=false;
            entry["deleting"]=true;
        }
        if(active()==id)document["active"]="";
    }
    void delete_window_files(const std::string& id,const std::string& window){
        auto config=window_config_path(id,window);
        ApplicationDeletion deletion;
        if(!deletion.exists(config.parent_path()))return;
        deletion.lock_ancestors(config.parent_path());
        deletion.add_profile(config,{},path);
        deletion.execute();
    }
    void delete_window_profile(const std::string& id,const std::string& window,const std::function<void()>& removeRegions={}){
        bool retry=window_profile(id,window).value("deleting",false);
        auto next=*this;
        auto& applications=next.document.at("applications");
        auto application=std::find_if(applications.begin(),applications.end(),[&](const auto& entry){return entry.at("id")==id;});
        auto& windows=application->at("windows");
        auto profile=std::find_if(windows.begin(),windows.end(),[&](const auto& entry){return entry.at("id")==window;});
        if(!retry){
            (*profile)["deleting"]=true;
            next.save();
            document=next.document;
        }
        delete_window_files(id,window);
        if(removeRegions)removeRegions();
        windows.erase(profile);
        auto last=application->find("last_window");
        if(last!=application->end()&&last->is_object()&&last->contains("profile")&&last->at("profile")==window)
            application->erase(last);
        if(application->contains("window_bindings")&&application->at("window_bindings").is_object())
            application->at("window_bindings").erase(window);
        next.save();
        document=std::move(next.document);
    }
    void finish_deletion(const std::string& id){
        if(!app(id).value("deleting",false))throw std::runtime_error("application_deletion_not_started");
        for(const auto& window:app(id).value("windows",json::array()))delete_window_files(id,window.at("id"));
        auto scoped=scoped_config_path(id);
        {
            ApplicationDeletion deletion;
            if(deletion.exists(scoped.parent_path())){
                deletion.lock_ancestors(scoped.parent_path());
                deletion.add_profile(scoped,{},path);
                deletion.execute();
            }
        }
        auto config=config_path(id);
        {
            ApplicationDeletion deletion;
            if(deletion.exists(config.parent_path())){
                deletion.lock_ancestors(config.parent_path());
                deletion.add_profile(config,id=="chatgpt"?application_directory():window_directory(id),path);
                deletion.execute();
            }
        }
        auto next=*this;
        auto& entries=next.document["applications"];
        entries.erase(std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return entry["id"]==id;}));
        next.save();
        document=std::move(next.document);
    }
    bool accepts(HWND window,bool includeMinimized=false)const{
        if(!application_window(window,includeMinimized))return false;
        if(testWindowPrefix.empty())return true;
        wchar_t title[256]{};GetWindowTextW(window,title,256);
        return std::wstring(title).compare(0,testWindowPrefix.size(),testWindowPrefix)==0;
    }
    std::vector<WindowIdentity> candidates(const std::string& id,bool includeMinimized=false)const{
        if(id.empty()||!listed(app(id)))return {};
        struct Search {const ApplicationProfiles* profiles;const json* app;bool includeMinimized;std::vector<WindowIdentity> windows;};
        Search search{this,&app(id),includeMinimized,{}};
        EnumWindows([](HWND window,LPARAM value)->BOOL{
            auto& search=*reinterpret_cast<Search*>(value);
            if(search.profiles->accepts(window,search.includeMinimized)){
                auto identity=WindowIdentity::read(window);
                if(search.profiles->matches(*search.app,identity))search.windows.push_back(std::move(identity));
            }
            return TRUE;
        },reinterpret_cast<LPARAM>(&search));
        return search.windows;
    }
    void save(){
        auto bytes=document.dump(2);
        visual_atomic_write(path,std::vector<BYTE>(bytes.begin(),bytes.end()),new_id(),true);
    }
};

struct ApplicationPicker {
    static constexpr UINT notification=WM_APP+62;
    HWND window=nullptr,host=nullptr;
    WindowIdentity selected;
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l){
        auto self=reinterpret_cast<ApplicationPicker*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){
            self=static_cast<ApplicationPicker*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(self){
            if(message==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_CROSS));return TRUE;}
            if(message==WM_LBUTTONUP){
                POINT point{SHORT(LOWORD(l)),SHORT(HIWORD(l))};ClientToScreen(window,&point);ShowWindow(window,SW_HIDE);
                HWND candidate=GetAncestor(WindowFromPoint(point),GA_ROOT);
                if(application_window(candidate))self->selected=WindowIdentity::read(candidate);
                PostMessageW(self->host,notification,0,0);return 0;
            }
            if(message==WM_KEYDOWN&&w==VK_ESCAPE){ShowWindow(window,SW_HIDE);PostMessageW(self->host,notification,0,0);return 0;}
            if(message==WM_CLOSE){ShowWindow(window,SW_HIDE);PostMessageW(self->host,notification,0,0);return 0;}
            if(message==WM_PAINT){PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);FillRect(dc,&paint.rcPaint,HBRUSH(GetStockObject(BLACK_BRUSH)));EndPaint(window,&paint);return 0;}
        }
        return DefWindowProcW(window,message,w,l);
    }
    explicit ApplicationPicker(HWND owner):host(owner){
        WNDCLASSW type{};type.lpfnWndProc=proc;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"OverlayApplicationPicker";
        type.hCursor=LoadCursorW(nullptr,IDC_CROSS);RegisterClassW(&type);
        window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,type.lpszClassName,L"点选应用窗口 · Esc 取消",WS_POPUP,
            GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN),owner,nullptr,type.hInstance,this);
        if(!window)throw std::runtime_error("Cannot create application picker");
        if(!SetLayeredWindowAttributes(window,0,20,LWA_ALPHA)){
            DestroyWindow(window);throw std::runtime_error("Cannot display application picker");
        }
        ShowWindow(window,SW_SHOW);SetForegroundWindow(window);SetFocus(window);
    }
    ~ApplicationPicker(){if(window)DestroyWindow(window);}
};
