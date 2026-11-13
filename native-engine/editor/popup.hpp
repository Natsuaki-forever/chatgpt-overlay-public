#pragma once

#include <functional>
#include <optional>
#include <utility>

class PanelPopup {
public:
    struct Row {
        std::wstring label;
        bool current=false, next=false, danger=false, disabled=false;
        int indent=0;
        bool heading=false, expanded=false;
        std::optional<chrome::Icon> icon;
        std::function<void()> onAction;
        bool footer=false,footerStart=false,footerEnd=false;
    };

    ~PanelPopup(){close(false);}

    void close(bool restoreFocus=true){
        auto handle=std::exchange(window,nullptr);
        if(!handle)return;
        DestroyWindow(handle);
        list=nullptr;
        if(restoreFocus&&IsWindowVisible(focusReturn)&&IsWindowEnabled(focusReturn))SetFocus(focusReturn);
        onChoose={};onBack={};
    }

    bool open()const{return window!=nullptr;}
    bool handles(HWND handle)const{return window&&(handle==window||IsChild(window,handle));}

    void show(HWND owner,HWND anchor,const RECT& bounds,int width,const Theme& colors,HFONT textFont,
        std::vector<Row> items,std::function<void(size_t)> choose,std::function<void()> back={},int selected=-1,HFONT headingFont=nullptr){
        auto instance=GetModuleHandleW(nullptr);
        theme=&colors;font=textFont;boldFont=headingFont;focusReturn=anchor;
        dpi=GetDpiForWindow(owner);padding=chrome::px(6,dpi);rowHeight=chrome::px(35,dpi);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(&bounds,MONITOR_DEFAULTTONEAREST),&monitor);
        const auto& work=monitor.rcWork;
        int gap=chrome::px(5,dpi);
        LONG desired=LONG(std::min<size_t>(items.size(),9))*rowHeight+padding*2;
        LONG below=work.bottom-bounds.bottom-gap,above=bounds.top-work.top-gap;
        bool flip=desired>below&&above>below;
        LONG available=std::max<LONG>(rowHeight+padding*2,flip?above:below);
        LONG height=std::min(desired,available);
        if(!window){
            rows.clear();
            WNDCLASSW type{};
            type.hInstance=instance;type.lpszClassName=L"OverlayEditorPopup";type.lpfnWndProc=proc;
            type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.style=CS_DROPSHADOW;
            RegisterClassW(&type);
            if(!CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED,type.lpszClassName,nullptr,WS_POPUP|WS_CLIPCHILDREN,
                0,0,0,0,owner,nullptr,instance,this))return;
        }
        if(!list){close();return;}
        size_t prefix=0;
        while(prefix<rows.size()&&prefix<items.size()&&rows[prefix].label==items[prefix].label)++prefix;
        size_t oldCount=rows.size();
        int top=int(SendMessageW(list,LB_GETTOPINDEX,0,0));
        RECT oldBounds{};GetWindowRect(window,&oldBounds);
        bool visible=IsWindowVisible(window)!=FALSE;
        rows=std::move(items);onChoose=std::move(choose);onBack=std::move(back);
        hoverRow=-1;
        SendMessageW(list,WM_SETREDRAW,FALSE,0);
        SendMessageW(list,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        SendMessageW(list,LB_SETITEMHEIGHT,0,rowHeight);
        for(size_t i=oldCount;i>prefix;--i)SendMessageW(list,LB_DELETESTRING,i-1,0);
        for(size_t i=prefix;i<rows.size();++i)
            SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(rows[i].label.c_str()));
        if(visible&&top>0&&size_t(top)<rows.size())SendMessageW(list,LB_SETTOPINDEX,top,0);
        SendMessageW(list,LB_SETCURSEL,selected,0);
        SendMessageW(list,WM_SETREDRAW,TRUE,0);
        LONG x=std::clamp(bounds.left,work.left,std::max<LONG>(work.left,work.right-width));
        LONG y=flip?bounds.top-gap-height:bounds.bottom+gap;
        bool resized=width!=oldBounds.right-oldBounds.left||height!=oldBounds.bottom-oldBounds.top;
        if(resized)SetWindowPos(list,nullptr,padding,padding,width-2*padding,height-2*padding,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        present({x,y,x+width,y+height});
        if(resized){
            auto radius=chrome::px(9,dpi);
            auto shape=CreateRoundRectRgn(0,0,width+1,height+1,radius*2,radius*2);
            if(shape&&!SetWindowRgn(window,shape,FALSE))DeleteObject(shape);
        }
        if(!visible){ShowWindow(window,SW_SHOW);SetFocus(list);}
    }

private:
    HWND window=nullptr,list=nullptr,focusReturn=nullptr;
    const Theme* theme=nullptr;
    HFONT font=nullptr,boldFont=nullptr;
    UINT dpi=96;
    int padding=6,rowHeight=35;
    int hoverRow=-1;
    std::vector<Row> rows;
    std::function<void(size_t)> onChoose;
    std::function<void()> onBack;
    static constexpr UINT presentMessage=WM_APP+91;

    void present(RECT bounds={}){
        if(!window||!list)return;
        if(IsRectEmpty(&bounds))GetWindowRect(window,&bounds);
        int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
        if(width<=0||height<=0)return;
        HDC screen=GetDC(nullptr);
        if(!screen)return;
        HDC dc=CreateCompatibleDC(screen);
        HBITMAP bitmap=dc?CreateCompatibleBitmap(screen,width,height):nullptr;
        if(bitmap){
            auto old=SelectObject(dc,bitmap);
            RECT surface{0,0,width,height};
            paint_surface(dc,surface);
            POINT origin{};MapWindowPoints(list,window,&origin,1);
            RECT client{};GetClientRect(list,&client);
            int saved=SaveDC(dc);
            if(saved){
                SetViewportOrgEx(dc,origin.x,origin.y,nullptr);
                IntersectClipRect(dc,0,0,client.right,client.bottom);
                int selected=int(SendMessageW(list,LB_GETCURSEL,0,0));
                int first=int(SendMessageW(list,LB_GETTOPINDEX,0,0));
                for(int index=std::max(0,first);size_t(index)<rows.size();++index){
                    RECT row{};
                    if(SendMessageW(list,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&row))==LB_ERR||
                        row.top>=client.bottom)break;
                    DRAWITEMSTRUCT item{};
                    item.itemID=UINT(index);item.itemState=index==selected?ODS_SELECTED:0;
                    item.hDC=dc;item.rcItem=row;
                    draw_row(item,true);
                }
                RestoreDC(dc,saved);
            }
            int shown=std::max(1,int(client.bottom)/rowHeight);
            if(rows.size()>size_t(shown)){
                int track=client.bottom;
                int thumb=std::max(chrome::px(16,dpi),int(track*shown/rows.size()));
                int top=int(SendMessageW(list,LB_GETTOPINDEX,0,0));
                int offset=int((track-thumb)*top/(rows.size()-shown));
                int x=origin.x+client.right+chrome::px(3,dpi);
                RECT mark{x,origin.y+offset,x+chrome::px(3,dpi),origin.y+offset+thumb};
                chrome::rounded(dc,mark,chrome::px(2,dpi),theme->border,theme->border);
            }
            POINT target{bounds.left,bounds.top},source{};
            SIZE size{width,height};
            UpdateLayeredWindow(window,screen,&target,&size,dc,&source,0,nullptr,ULW_OPAQUE);
            SelectObject(dc,old);DeleteObject(bitmap);
        }
        if(dc)DeleteDC(dc);
        ReleaseDC(nullptr,screen);
    }

    void select(LRESULT index){
        if(index<0||size_t(index)>=rows.size()||rows[index].disabled||!onChoose)return;
        auto choose=onChoose;
        choose(size_t(index));
    }

    void paint_row(const DRAWITEMSTRUCT& item)const{
        const auto& box=item.rcItem;
        int width=box.right-box.left,height=box.bottom-box.top;
        if(width<=0||height<=0)return;
        auto buffer=CreateCompatibleDC(item.hDC);
        auto bitmap=buffer?CreateCompatibleBitmap(item.hDC,box.right,box.bottom):nullptr;
        if(bitmap){
            auto old=SelectObject(buffer,bitmap);
            auto frame=item;
            frame.hDC=buffer;
            draw_row(frame);
            BitBlt(item.hDC,box.left,box.top,width,height,buffer,box.left,box.top,SRCCOPY);
            SelectObject(buffer,old);
            DeleteObject(bitmap);
        }else draw_row(item);
        if(buffer)DeleteDC(buffer);
    }

    void draw_row(const DRAWITEMSTRUCT& item,bool composed=false)const{
        auto dc=item.hDC;auto box=item.rcItem;
        if(!composed){
            int saved=SaveDC(dc);
            if(saved){
                IntersectClipRect(dc,box.left,box.top,box.right,box.bottom);
                paint_surface(dc,list);
                RestoreDC(dc,saved);
            }else FillRect(dc,&box,theme->field);
        }
        if(item.itemID>=rows.size())return;
        const auto& row=rows[item.itemID];
        auto scale=[this](int value){return chrome::px(value,dpi);};
        bool selected=(hoverRow>=0?size_t(hoverRow)==item.itemID:(item.itemState&ODS_SELECTED)!=0)&&!row.disabled;
        if(row.footer){
            RECT band=box;
            band.left+=scale(2);band.right-=scale(2);
            if(row.footerStart)band.top+=scale(6);
            if(row.footerEnd)band.bottom-=scale(3);
            if(!theme->highContrast){
                auto fill=theme->action;
                auto brush=CreateSolidBrush(RGB(
                    (GetRValue(fill)+GetRValue(theme->paper))/2,
                    (GetGValue(fill)+GetGValue(theme->paper))/2,
                    (GetBValue(fill)+GetBValue(theme->paper))/2));
                FillRect(dc,&band,brush);DeleteObject(brush);
            }
            box.top=band.top;box.bottom=band.bottom;
        }
        if(selected||row.current){
            auto fill=selected?theme->hover:theme->selected;
            if(theme->highContrast)fill=theme->accent;
            chrome::rounded(dc,box,scale(5),fill,fill);
        }
        RECT text=box; text.left+=scale(9+row.indent);text.right-=scale((row.current||row.next?26:8)+(row.onAction?30:0));
        auto ink=row.disabled?theme->disabled:row.danger?theme->danger:theme->text;
        if(theme->highContrast&&(selected||row.current))ink=theme->accentText;
        auto old=SelectObject(dc,row.heading&&boldFont?boldFont:font);
        if(row.icon){
            RECT icon{text.left,(box.top+box.bottom-scale(18))/2,text.left+scale(18),(box.top+box.bottom+scale(18))/2};
            auto fill=row.current?theme->accent:theme->hover;
            chrome::rounded(dc,icon,scale(4),fill,fill);
            chrome::icon(dc,icon,*row.icon,row.current?theme->accentText:
                *row.icon==chrome::Icon::Archive?theme->focus:theme->muted,dpi);
            text.left+=scale(24);
        }
        chrome::label(dc,text,row.label.c_str(),ink,DT_LEFT);
        RECT badge{box.right-scale(27),box.top+scale(8),box.right-scale(8),box.bottom-scale(8)};
        if(row.current&&row.onAction)OffsetRect(&badge,-scale(32),0);
        if(row.current){
            if(!row.icon)chrome::rounded(dc,badge,scale(4),theme->accent,theme->accent);
            chrome::label(dc,badge,L"✓",row.icon?ink:theme->accentText);
        }else if(row.next&&row.heading)chrome::icon(dc,badge,chrome::Icon::Chevron,ink,dpi,false,row.expanded);
        else if(row.next)chrome::label(dc,badge,L"›",ink);
        if(row.onAction){
            RECT action{box.right-scale(33),box.top,box.right,box.bottom};
            if(row.heading)chrome::icon(dc,badge,chrome::Icon::Chevron,ink,dpi,false,row.expanded);
            else{
                if(row.expanded)chrome::rounded(dc,action,scale(5),theme->hover,theme->hover);
                chrome::icon(dc,action,chrome::Icon::More,ink,dpi);
            }
        }
        SelectObject(dc,old);
    }

    void paint_surface(HDC dc,HWND child=nullptr)const{
        RECT box{};GetClientRect(window,&box);
        if(child)MapWindowPoints(window,child,reinterpret_cast<POINT*>(&box),2);
        paint_surface(dc,box);
    }
    void paint_surface(HDC dc,const RECT& box)const{
        FillRect(dc,&box,theme->field);
        if(theme->highContrast)return;
        int radius=chrome::px(9,dpi);
        chrome::backdrop_glow(dc,box,radius,theme->tint);
        chrome::glass_surface(dc,box,radius,theme->paper,theme->border,theme->highlight,theme->dark?174:152);
    }

    void hover(int index){
        if(index==hoverRow)return;
        hoverRow=index;
        present();
    }

    static LRESULT CALLBACK list_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR context){
        auto& popup=*reinterpret_cast<PanelPopup*>(context);
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,list_proc,id);
        if(msg==WM_GETDLGCODE)return DLGC_WANTALLKEYS|DLGC_WANTARROWS;
        if(msg==WM_KEYDOWN){
            popup.hover(-1);
            if(wp==VK_ESCAPE||wp==VK_TAB){popup.close();return 0;}
            if(wp==VK_LEFT&&popup.onBack){auto back=popup.onBack;back();return 0;}
            if(wp==VK_RIGHT||wp==VK_F10){
                auto index=SendMessageW(hwnd,LB_GETCURSEL,0,0);
                if(index>=0&&size_t(index)<popup.rows.size()&&popup.rows[index].onAction){
                    auto action=popup.rows[index].onAction;action();return 0;
                }
            }
            if(wp==VK_RETURN||wp==VK_RIGHT||wp==VK_SPACE){
                popup.select(SendMessageW(hwnd,LB_GETCURSEL,0,0));return 0;
            }
        }
        if(msg==WM_MOUSELEAVE){popup.hover(-1);return 0;}
        if(msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP){
            auto hit=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,lp);
            if(!HIWORD(hit)&&size_t(LOWORD(hit))<popup.rows.size()){
                RECT row{};POINT point{SHORT(LOWORD(lp)),SHORT(HIWORD(lp))};
                SendMessageW(hwnd,LB_GETITEMRECT,LOWORD(hit),reinterpret_cast<LPARAM>(&row));
                if(PtInRect(&row,point)){
                    if(msg==WM_MOUSEMOVE){
                        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0};
                        TrackMouseEvent(&tracking);
                        popup.hover(LOWORD(hit));
                        return 0;
                    }else{
                        auto action=popup.rows[LOWORD(hit)].onAction;
                        bool trailing=action&&point.x>=row.right-chrome::px(33,popup.dpi);
                        DefSubclassProc(hwnd,msg,wp,lp);
                        if(trailing)action();else popup.select(LOWORD(hit));
                        return 0;
                    }
                }
            }
            if(msg==WM_MOUSEMOVE){popup.hover(-1);return 0;}
        }
        auto result=DefSubclassProc(hwnd,msg,wp,lp);
        if(msg==WM_KEYDOWN||msg==WM_MOUSEWHEEL||msg==WM_VSCROLL||
            msg==LB_SETCURSEL||msg==LB_SETTOPINDEX)
            if(popup.window)PostMessageW(popup.window,presentMessage,0,0);
        return result;
    }

    static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        auto self=reinterpret_cast<PanelPopup*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE){
            self=static_cast<PanelPopup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window=hwnd;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self)return DefWindowProcW(hwnd,msg,wp,lp);
        switch(msg){
        case presentMessage:self->present();return 0;
        case WM_CREATE:
            self->list=CreateWindowExW(0,L"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|
                LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,0,0,0,0,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(self->list)SetWindowSubclass(self->list,list_proc,1,reinterpret_cast<DWORD_PTR>(self));
            return 0;
        case WM_SIZE:
            if(self->list)SetWindowPos(self->list,nullptr,self->padding,self->padding,
                LOWORD(lp)-2*self->padding,HIWORD(lp)-2*self->padding,
                SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
            return 0;
        case WM_ACTIVATE:
            if(LOWORD(wp)==WA_INACTIVE)self->close(false);
            break;
        case WM_NCDESTROY:
            if(self->window==hwnd){self->window=nullptr;self->list=nullptr;}
            break;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{
            PAINTSTRUCT paint{};auto dc=BeginPaint(hwnd,&paint);
            self->paint_surface(dc);
            EndPaint(hwnd,&paint);return 0;
        }
        case WM_DRAWITEM:
            self->paint_row(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));return TRUE;
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=self->rowHeight;return TRUE;
        case WM_CTLCOLORLISTBOX:
            SetBkColor(reinterpret_cast<HDC>(wp),self->theme->input);
            return reinterpret_cast<LRESULT>(self->theme->field);
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};
