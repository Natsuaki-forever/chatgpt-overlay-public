#pragma once
#include "core.hpp"
#include "application_profiles.hpp"
#include <shlobj.h>

inline fs::path application_file(){
    wchar_t text[32768];DWORD size=GetModuleFileNameW(nullptr,text,32768);
    if(!size||size>=32768)throw std::runtime_error("Cannot locate application directory");
    return fs::path(std::wstring(text,size));
}
inline fs::path application_data(){
    wchar_t text[MAX_PATH];
    check(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,text));
    return fs::path(text)/L"ChatGPTOverlay";
}
inline void record_error(const std::string& text) noexcept{
    try{
        auto directory=application_data();fs::create_directories(directory);
        std::ofstream(directory/L"native-error.log",std::ios::app)<<text<<'\n';
    }catch(...){}
}

struct NativeShell {
    static constexpr UINT callback=WM_APP+60;
    static constexpr int hotkeyId=0x4347;
    enum Command:UINT {Edit=100,Rediscover,OpenConfig,Startup,Help,Quit,PickApplication,TargetFirst=1000,ApplicationFirst=2000};
    HWND host=nullptr;
    HICON icon=nullptr;
    bool added=false,hotkey=false,test=false;
    UINT taskbarCreated=0;
    std::vector<HWND> targets;
    std::vector<WindowIdentity> applicationWindows;
    std::vector<std::string> applicationIds;
    static constexpr const wchar_t* runKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    NOTIFYICONDATAW data()const{
        NOTIFYICONDATAW value{};value.cbSize=sizeof(value);value.hWnd=host;value.uID=1;return value;
    }
    static HICON make_icon(){
        BITMAPV5HEADER header{};header.bV5Size=sizeof(header);header.bV5Width=32;header.bV5Height=-32;
        header.bV5Planes=1;header.bV5BitCount=32;header.bV5Compression=BI_BITFIELDS;
        header.bV5RedMask=0x00ff0000;header.bV5GreenMask=0x0000ff00;header.bV5BlueMask=0xff;header.bV5AlphaMask=0xff000000;
        void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(nullptr,reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap)return nullptr;
        for(int y=0;y<32;y++)for(int x=0;x<32;x++){
            double distance=std::hypot(x-15.5,y-15.5);
            static_cast<DWORD*>(pixels)[y*32+x]=(distance>=8&&distance<=13)?0xff8bdcc0:0;
        }
        HBITMAP mask=CreateBitmap(32,32,1,1,nullptr);
        ICONINFO info{};info.fIcon=TRUE;info.hbmColor=bitmap;info.hbmMask=mask;
        HICON result=mask?CreateIconIndirect(&info):nullptr;
        if(mask)DeleteObject(mask);DeleteObject(bitmap);return result;
    }
    bool add(){
        auto value=data();value.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;
        value.uCallbackMessage=callback;value.hIcon=icon;
        wcscpy_s(value.szTip,L"ChatGPT Overlay · Ctrl+Shift+E");
        added=Shell_NotifyIconW(NIM_ADD,&value)!=FALSE;
        if(added){value.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&value);}
        return added;
    }
    void start(HWND window,bool testing){
        host=window;test=testing;icon=make_icon();
        if(!icon)throw std::runtime_error("Cannot create tray icon");
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");add();
        hotkey=RegisterHotKey(host,hotkeyId,MOD_CONTROL|MOD_SHIFT|MOD_NOREPEAT,test?VK_F24:'E')!=FALSE;
        if(!added)SetTimer(host,9,2000,nullptr);
        if(!hotkey)notice(L"快捷键已被其他程序占用，请从托盘进入编辑。",true);
    }
    void calibration_status(const std::wstring& text){
        if(!added)return;
        auto value=data();value.uFlags=NIF_TIP|NIF_SHOWTIP;
        wcsncpy_s(value.szTip,text.empty()?L"ChatGPT Overlay · Ctrl+Shift+E":text.c_str(),_TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY,&value);
    }
    void notice(const std::wstring& text,bool error=false){
        if(!added){record_error(utf8(text));return;}
        auto value=data();value.uFlags=NIF_INFO;value.dwInfoFlags=error?NIIF_WARNING:NIIF_INFO;
        wcsncpy_s(value.szInfoTitle,L"ChatGPT Overlay",_TRUNCATE);
        wcsncpy_s(value.szInfo,text.c_str(),_TRUNCATE);Shell_NotifyIconW(NIM_MODIFY,&value);
    }
    bool startup_enabled()const{
        HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,runKey,0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;
        LONG result=RegQueryValueExW(key,L"ChatGPTOverlay",nullptr,nullptr,nullptr,nullptr);RegCloseKey(key);return result==ERROR_SUCCESS;
    }
    void toggle_startup(){
        if(test)throw std::runtime_error("Startup changes are disabled in diagnostic mode");
        bool enabled=startup_enabled();HKEY key=nullptr;
        LONG result=RegCreateKeyExW(HKEY_CURRENT_USER,runKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
        if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot open startup settings");
        std::wstring command=L"\""+application_file().wstring()+L"\"";
        result=enabled?RegDeleteValueW(key,L"ChatGPTOverlay"):
            RegSetValueExW(key,L"ChatGPTOverlay",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),DWORD((command.size()+1)*sizeof(wchar_t)));
        RegCloseKey(key);if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot update startup settings");
    }
    UINT menu(HWND selected,bool editing,const ApplicationProfiles* profiles=nullptr){
        HMENU menu=CreatePopupMenu();if(!menu)throw std::runtime_error("Cannot create tray menu");
        targets.clear();
        if(!profiles)EnumWindows([](HWND window,LPARAM param)->BOOL{
            if(available(window)&&!GetWindow(window,GW_OWNER)&&is_chatgpt(window))
                reinterpret_cast<NativeShell*>(param)->targets.push_back(window);
            return TRUE;
        },reinterpret_cast<LPARAM>(this));
        AppendMenuW(menu,MF_STRING,Edit,L"打开／恢复编辑器\tCtrl+Shift+E");
        if(profiles){
            AppendMenuW(menu,MF_STRING,PickApplication,L"点选并保存应用窗口…");
            HMENU saved=CreatePopupMenu();applicationIds.clear();
            auto active=profiles->active();
            for(const auto& app:profiles->document["applications"]){
                if(!ApplicationProfiles::listed(app))continue;
                auto id=app.at("id").get<std::string>();
                auto label=wide(app.at("name").get<std::string>());
                if(id!="chatgpt")label+=L" · "+wide(app.at("executable").get<std::string>());
                AppendMenuW(saved,MF_STRING|(id==active?MF_CHECKED:0),ApplicationFirst+applicationIds.size(),label.c_str());
                applicationIds.push_back(std::move(id));
            }
            AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(saved),L"已保存的应用");
            applicationWindows=profiles->candidates(active);
            if(!applicationWindows.empty()){
                HMENU choices=CreatePopupMenu();
                if(applicationWindows.size()>ApplicationFirst-TargetFirst)applicationWindows.resize(ApplicationFirst-TargetFirst);
                for(size_t i=0;i<applicationWindows.size();i++){
                    auto& candidate=applicationWindows[i];wchar_t title[256]{};GetWindowTextW(candidate.window,title,256);
                    std::wstring label=std::to_wstring(i+1)+L". "+(title[0]?title:L"无标题")+L" (PID "+std::to_wstring(candidate.pid)+L")";
                    AppendMenuW(choices,MF_STRING|(candidate.window==selected?MF_CHECKED:0),TargetFirst+i,label.c_str());
                }
                AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(choices),L"绑定当前应用的窗口");
            }
        }else AppendMenuW(menu,MF_STRING,Rediscover,L"重新识别 ChatGPT");
        if(!profiles&&targets.size()>1){
            HMENU choices=CreatePopupMenu();
            for(size_t i=0;i<targets.size();i++){
                wchar_t title[256]{};GetWindowTextW(targets[i],title,256);
                std::wstring label=std::to_wstring(i+1)+L". "+(title[0]?title:L"ChatGPT")+L" (PID "+std::to_wstring(process_id(targets[i]))+L")";
                AppendMenuW(choices,MF_STRING|(targets[i]==selected?MF_CHECKED:0),TargetFirst+i,label.c_str());
            }
            AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(choices),L"选择 ChatGPT 窗口");
        }
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,OpenConfig,L"打开配置目录");
        AppendMenuW(menu,MF_STRING|(startup_enabled()?MF_CHECKED:0)|(test?MF_GRAYED:0),Startup,L"开机自启");
        AppendMenuW(menu,MF_STRING,Help,L"使用说明");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,Quit,L"退出");
        POINT cursor{};GetCursorPos(&cursor);SetForegroundWindow(host);
        UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,cursor.x,cursor.y,0,host,nullptr);
        DestroyMenu(menu);PostMessageW(host,WM_NULL,0,0);
        auto value=data();Shell_NotifyIconW(NIM_SETFOCUS,&value);return command;
    }
    ~NativeShell(){
        if(hotkey)UnregisterHotKey(host,hotkeyId);
        if(added){auto value=data();Shell_NotifyIconW(NIM_DELETE,&value);}
        if(icon)DestroyIcon(icon);
    }
};
