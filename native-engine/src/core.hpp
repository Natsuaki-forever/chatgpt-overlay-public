#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <shellapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "../third_party/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

inline void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Windows renderer error: " + std::to_string(hr)); }
template<class T> struct Com {
    T* value = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : value(other.value) { other.value = nullptr; }
    Com& operator=(Com&& other) noexcept { if (value) value->Release(); value=other.value; other.value=nullptr; return *this; }
    ~Com() { if (value) value->Release(); }
    T** put() { if (value) value->Release(); value=nullptr; return &value; }
    T* operator->() const { return value; }
};
inline std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8");
    std::wstring out(n, L' ');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), n);
    return out;
}
inline std::string utf8(const std::wstring& text) {
    int n=WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, ' ');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
inline D2D1_COLOR_F color(const std::string& hex, float alpha=1) {
    if (hex.size()!=7 || hex[0]!='#' || hex.find_first_not_of("0123456789abcdefABCDEF",1)!=std::string::npos) throw std::runtime_error("Invalid color");
    unsigned long rgb=std::stoul(hex.substr(1), nullptr, 16);
    return D2D1::ColorF((rgb>>16 & 255)/255.f, (rgb>>8 & 255)/255.f, (rgb & 255)/255.f, alpha);
}
inline double number(const json& obj, const char* key, double fallback, double low, double high) {
    double value=obj.value(key, fallback);
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite region value");
    return std::clamp(value, low, high);
}
inline RECT client_box(HWND hwnd) {
    RECT r{}; POINT origin{};
    GetClientRect(hwnd, &r); ClientToScreen(hwnd, &origin);
    return {origin.x, origin.y, origin.x+r.right, origin.y+r.bottom};
}
inline bool available(HWND hwnd,bool includeMinimized=false) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || (!includeMinimized&&IsIconic(hwnd))) return false;
    DWORD cloaked=0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked;
}
inline DWORD process_id(HWND hwnd) { DWORD pid=0; GetWindowThreadProcessId(hwnd, &pid); return pid; }
inline bool is_chatgpt(HWND hwnd) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id(hwnd));
    if (!process) return false;
    wchar_t path[32768]; DWORD size=32768;
    bool ok=QueryFullProcessImageNameW(process, 0, path, &size)!=0;
    CloseHandle(process);
    return ok && _wcsicmp(fs::path(path).filename().c_str(), L"ChatGPT.exe")==0;
}
inline HWND find_target() {
    HWND foreground=GetForegroundWindow();
    if (available(foreground) && is_chatgpt(foreground)) return foreground;
    HWND result=nullptr;
    EnumWindows([](HWND hwnd, LPARAM data)->BOOL {
        if (available(hwnd) && !GetWindow(hwnd, GW_OWNER) && is_chatgpt(hwnd)) {
            *reinterpret_cast<HWND*>(data)=hwnd; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

struct Region {
    json source;
    std::wstring text, family;
    float x,y,width,height,size,padding,spacing;
    float storedX,storedY,referenceWidth,referenceHeight;
    bool visual=false,drawable=true;
    std::string anchorX,anchorY;
    float opacity;
    bool bold,scroll,glow,outline,feather,pulse;
    float speed,gap,glowRadius,glowStrength,glowPeriod,outlineWidth,featherWidth;
    bool right;
    std::string align;
    D2D1_COLOR_F foreground, background, glowColor, outlineColor;
    explicit Region(const json& j): source(j) {
        visual=j.value("position_mode",std::string("anchor"))=="visual";drawable=!visual;
        text=wide(j.value("text", std::string("我的工作台")));
        family=wide(j.value("font", std::string("Segoe UI")));
        x=float(number(j,"x",20,0,100000)); y=float(number(j,"y",20,0,100000));
        width=float(number(j,"width",160,8,100000)); height=float(number(j,"height",40,8,100000));
        storedX=x;storedY=y;
        referenceWidth=float(number(j,"reference_width",0,0,100000));
        referenceHeight=float(number(j,"reference_height",0,0,100000));
        anchorX=j.value("anchor_x",std::string("left"));anchorY=j.value("anchor_y",std::string("top"));
        size=float(number(j,"size",14,6,200)); padding=float(number(j,"padding",8,0,100));
        spacing=float(number(j,"letter_spacing",0,-10,50)); opacity=float(number(j,"opacity",255,0,255))/255;
        bold=j.value("bold",false); scroll=j.value("scroll",false); glow=j.value("glow",false); outline=j.value("outline",false);
        feather=j.value("scroll_feather",false); pulse=j.value("glow_pulse",true);
        speed=float(number(j,"scroll_speed",40,5,300)); gap=float(number(j,"scroll_gap",40,8,300));
        glowRadius=float(number(j,"glow_radius",10,1,24)); glowStrength=float(number(j,"glow_strength",75,0,100))/100;
        glowPeriod=float(number(j,"glow_period",2,.5,10)); outlineWidth=float(number(j,"outline_width",1.5,.5,12));
        featherWidth=float(number(j,"scroll_feather_width",24,1,200));
        right=j.value("scroll_direction",std::string("left"))=="right";
        align=j.value("align",std::string("left"));
        foreground=color(j.value("color",std::string("#ececec")));
        background=color(j.value("background",std::string("#212121")), opacity);
        glowColor=color(j.value("glow_color",std::string("#2ab89d")));
        outlineColor=color(j.value("outline_color",std::string("#000000")));
    }
    void locate(float clientWidth,float clientHeight){
        if(visual){drawable=false;return;}
        drawable=true;
        float horizontal=anchorX=="right"?1.f:anchorX=="center"?.5f:0.f;
        float vertical=anchorY=="bottom"?1.f:anchorY=="center"?.5f:0.f;
        x=std::max(0.f,storedX+(referenceWidth>0&&clientWidth>0?clientWidth-referenceWidth:0)*horizontal);
        y=std::max(0.f,storedY+(referenceHeight>0&&clientHeight>0?clientHeight-referenceHeight:0)*vertical);
    }
    bool animated() const { return !text.empty() && (scroll || (glow && pulse && glowStrength)); }
};
