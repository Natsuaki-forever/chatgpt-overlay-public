#pragma once

struct Theme {
    int mode=0; // System, light, dark.
    bool isolated=GetEnvironmentVariableW(L"OVERLAY_NATIVE_EDITOR_LAB_NO_PREFERENCES",nullptr,0)!=0;
    bool highContrast=false;
    COLORREF background{},input{},text{},muted{},border{},accent{},accentText{},button{},selected{},selectedBorder{},
        segment{},activeSegment{},hover{},disabled{},warning{},outline{},rail{},paper{},glass{},tint{},highlight{},danger{},focus{},action{},actionBorder{};
    HBRUSH page=nullptr,field=nullptr,buttonField=nullptr,railField=nullptr,paperField=nullptr;
    unsigned revision=0;
    bool dark=false;
    static constexpr const wchar_t* key=L"Software\\ChatGPTOverlay\\NativeEditorLab";

    ~Theme(){for(auto brush:{page,field,buttonField,railField,paperField})if(brush)DeleteObject(brush);}

    void load(){
        if(isolated)return;
        DWORD value=0,size=sizeof(value);
        if(RegGetValueW(HKEY_CURRENT_USER,key,L"Theme",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS&&value<=2){
            mode=int(value);return;
        }
        wchar_t previous[16]{};size=sizeof(previous);
        if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\ChatGPTOverlay\\Editor",L"theme",RRF_RT_REG_SZ,
            nullptr,previous,&size)==ERROR_SUCCESS){
            if(wcscmp(previous,L"light")==0)mode=1;
            else if(wcscmp(previous,L"dark")==0)mode=2;
        }
    }

    void save()const{
        if(isolated)return;
        DWORD value=DWORD(mode);
        RegSetKeyValueW(HKEY_CURRENT_USER,key,L"Theme",REG_DWORD,&value,sizeof(value));
    }

    void apply(HWND window){
        DWORD light=1,size=sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
        HIGHCONTRASTW contrast{};contrast.cbSize=sizeof(contrast);
        highContrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,contrast.cbSize,&contrast,0)&&
            (contrast.dwFlags&HCF_HIGHCONTRASTON);
        bool high=highContrast;
        dark=!high&&(mode==2||(mode==0&&!light));
        if(high){
            background=input=GetSysColor(COLOR_WINDOW);text=GetSysColor(COLOR_WINDOWTEXT);
            muted=GetSysColor(COLOR_GRAYTEXT);border=text;accent=GetSysColor(COLOR_HIGHLIGHT);
            accentText=GetSysColor(COLOR_HIGHLIGHTTEXT);
            button=segment=hover=rail=paper=glass=tint=input;
            selected=activeSegment=accent;selectedBorder=warning=outline=highlight=danger=focus=text;disabled=muted;action=accent;actionBorder=text;
        }else if(dark){
            background=RGB(39,35,56);input=RGB(51,45,71);text=RGB(243,239,249);
            muted=RGB(195,189,214);border=outline=RGB(116,110,141);accent=RGB(201,238,70);accentText=RGB(37,35,56);
            button=input;selected=RGB(66,56,84);selectedBorder=RGB(95,83,116);
            segment=input;activeSegment=selected;hover=RGB(68,58,94);
            disabled=RGB(149,140,166);warning=RGB(255,179,171);danger=RGB(255,146,158);focus=RGB(208,189,245);
            rail=RGB(45,38,64);paper=RGB(48,42,67);glass=RGB(56,47,76);tint=RGB(60,48,81);highlight=RGB(101,92,124);
            action=RGB(67,57,91);actionBorder=RGB(116,103,150);
        }else{
            background=RGB(238,236,246);input=RGB(247,244,252);text=RGB(37,35,56);
            muted=RGB(87,83,110);border=outline=RGB(188,183,207);accent=RGB(201,238,70);accentText=text;
            button=input;selected=RGB(245,241,251);selectedBorder=RGB(214,206,231);
            segment=input;activeSegment=RGB(233,227,245);hover=activeSegment;
            disabled=RGB(119,113,136);warning=RGB(157,48,42);danger=RGB(168,39,58);focus=RGB(116,99,160);
            action=RGB(232,225,244);actionBorder=RGB(182,169,208);
            rail=RGB(228,221,240);paper=RGB(245,242,250);glass=RGB(255,250,255);tint=RGB(216,207,238);highlight=RGB(255,255,255);
        }
        for(auto brush:{page,field,buttonField,railField,paperField})if(brush)DeleteObject(brush);
        page=CreateSolidBrush(background);field=CreateSolidBrush(input);buttonField=CreateSolidBrush(button);
        railField=CreateSolidBrush(rail);paperField=CreateSolidBrush(paper);++revision;
        BOOL immersive=dark;
        DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&immersive,sizeof(immersive));
        COLORREF caption=high?DWMWA_COLOR_DEFAULT:background;
        COLORREF captionText=high?DWMWA_COLOR_DEFAULT:text;
        DwmSetWindowAttribute(window,DWMWA_CAPTION_COLOR,&caption,sizeof(caption));
        DwmSetWindowAttribute(window,DWMWA_TEXT_COLOR,&captionText,sizeof(captionText));
        COLORREF systemOutline=high?DWMWA_COLOR_DEFAULT:DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(window,DWMWA_BORDER_COLOR,&systemOutline,sizeof(systemOutline));
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
    }

};
