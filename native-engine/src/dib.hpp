#pragma once
#include "core.hpp"

struct Dib {
    HDC dc=nullptr;
    HBITMAP bitmap=nullptr,previous=nullptr;
    BYTE* bits=nullptr;
    int width=0,height=0;
    Dib()=default;
    Dib(const Dib&)=delete;
    Dib& operator=(const Dib&)=delete;
    ~Dib(){reset();}
    void reset(){
        if(dc){SelectObject(dc,previous);DeleteDC(dc);dc=nullptr;}
        if(bitmap){DeleteObject(bitmap);bitmap=nullptr;}
        bits=nullptr;width=height=0;
    }
    void create(int w,int h){
        reset();
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
        info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        dc=CreateCompatibleDC(nullptr);
        if(dc)bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&bits),nullptr,0);
        if(!dc||!bitmap){reset();throw std::runtime_error("Cannot allocate compositor bitmap");}
        previous=HBITMAP(SelectObject(dc,bitmap));width=w;height=h;
    }
};
