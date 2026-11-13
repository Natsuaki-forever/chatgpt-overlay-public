#pragma once
#include <optional>
#include "render.hpp"
#include "config.hpp"
#include "surface_plan.hpp"

inline DWORD pixel_scale(DWORD pixel,unsigned alpha){
    DWORD rb=(pixel&0x00ff00ff)*alpha+0x00800080;
    DWORD ag=((pixel>>8)&0x00ff00ff)*alpha+0x00800080;
    return (((rb+((rb>>8)&0x00ff00ff))>>8)&0x00ff00ff)|
        ((((ag+((ag>>8)&0x00ff00ff))>>8)&0x00ff00ff)<<8);
}
inline DWORD pixel_over(DWORD source,DWORD destination){return source+pixel_scale(destination,255-(source>>24));}

struct EditCanvas {
    enum Mode {None,Create,Move,Resize,Reference};
    HWND hwnd=nullptr;
    struct Drawing {
        HWND window=nullptr;
        Dib image;
        bool valid()const{return IsWindow(window)&&GetWindowLongPtrW(window,GWLP_USERDATA)==reinterpret_cast<LONG_PTR>(this);}
        void hide(){if(valid())ShowWindow(window,SW_HIDE);image.reset();}
        ~Drawing(){if(valid())DestroyWindow(window);}
        void ensure(HWND owner,int w,int h){
            if(!valid()){
                window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,
                    L"OverlayNativeSurface",L"Overlay editor preview",WS_POPUP,0,0,1,1,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
                if(!window)throw std::runtime_error("Cannot create edit preview");
                SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
            }
            if(image.width!=w||image.height!=h)image.create(w,h);
        }
    };
    std::vector<std::unique_ptr<Drawing>> drawings;
    std::vector<RECT> plannedRectangles;
    std::vector<SurfaceGroup> drawingPlan;
    size_t activeDrawings=0;
    RECT inputBox{};
    BYTE opacity=0;
    Mode mode=None;
    float startX=0,startY=0,currentX=0,currentY=0,scale=1;
    float originalX=0,originalY=0;
    json original;
    std::string dragged;
    bool picking=false;
    bool referencePicking=false;
    json preview;
    struct CoverDrag {
        std::string id;
        VisualRect reference, original;
        json relation;
        bool placing=true, held=false;
        double startX=0,startY=0;
        VisualRect cover;
    };
    std::optional<CoverDrag> coverDrag;
    Region positioned(ConfigState& state,int index){
        auto r=state.positioned(index);
        if(coverDrag&&r.source["id"]==coverDrag->id){
            const auto& box=coverDrag->cover;
            r.x=float(box.x);r.y=float(box.y);r.width=float(box.width);r.height=float(box.height);
            r.drawable=true;
        }else if(preview.is_object()&&preview["id"]==r.source["id"]){
            const auto& box=preview["cover"];r.x=box["x"];r.y=box["y"];r.width=box["width"];r.height=box["height"];
            r.drawable=r.width>0&&r.height>0;r.visual=true;
        }
        return r;
    }
    bool valid()const{return IsWindow(hwnd)&&GetWindowLongPtrW(hwnd,GWLP_USERDATA)==reinterpret_cast<LONG_PTR>(this);}
    void ensure_window(){
        if(valid())return;
        inputBox={};opacity=0;
        hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW,L"OverlayNativeSurface",L"Overlay editor canvas",WS_POPUP,
            0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hwnd)throw std::runtime_error("Cannot create edit canvas");
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
    }
    ~EditCanvas(){close();}
    void hide(){
        for(auto& drawing:drawings)drawing->hide();
        activeDrawings=0;
        if(valid())ShowWindow(hwnd,SW_HIDE);
    }
    void close(){mode=None;hide();if(valid()){if(GetCapture()==hwnd)ReleaseCapture();DestroyWindow(hwnd);}hwnd=nullptr;}
    HWND top_window()const{return activeDrawings&&drawings.front()->valid()?drawings.front()->window:hwnd;}
    size_t pixels()const{
        size_t count=0;
        for(size_t i=0;i<activeDrawings;i++){
            const auto& image=drawings[i]->image;
            count+=size_t(image.width)*image.height;
        }
        return count;
    }
    bool dragging()const{return mode!=None;}
    void begin(ConfigState& state,float x,float y,bool createOnly=false){
        if(referencePicking){startX=currentX=x;startY=currentY=y;mode=Reference;return;}
        startX=currentX=x;startY=currentY=y;mode=Create;dragged.clear();state.selected.clear();
        if(createOnly){state.stateSequence++;return;}
        const auto& entries=state.document["regions"];
        for(int i=int(entries.size())-1;i>=0;i--){
            Region region=state.positioned(i);
            if(!region.drawable||region.visual)continue;
            if(x>=region.x-6&&x<=region.x+region.width+6&&y>=region.y-6&&y<=region.y+region.height+6){
                dragged=entries[i]["id"];state.selected=dragged;original=entries[i];
                originalX=region.x;originalY=region.y;
                mode=std::abs(x-region.x-region.width)+std::abs(y-region.y-region.height)<18?Resize:Move;
                break;
            }
        }
        state.stateSequence++;
    }
    void motion(ConfigState& state,float x,float y,float clientWidth,float clientHeight){
        if(mode==None)return;
        currentX=std::clamp(x,0.f,clientWidth);currentY=std::clamp(y,0.f,clientHeight);
        if(mode==Create||mode==Reference)return;
        if(state.index(dragged)<0){mode=None;return;}
        float dx=currentX-startX,dy=currentY-startY;
        Region old(original);old.x=originalX;old.y=originalY;json patch;
        if(mode==Move)patch={{"x",std::clamp(old.x+dx,0.f,std::max(0.f,clientWidth-old.width))},
            {"y",std::clamp(old.y+dy,0.f,std::max(0.f,clientHeight-old.height))}};
        else patch={{"width",std::max(8.f,std::min(clientWidth-old.x,old.width+dx))},
            {"height",std::max(8.f,std::min(clientHeight-old.y,old.height+dy))}};
        state.update_positioned(dragged,patch,state.revision);
    }
    void end(ConfigState& state){
        if(mode==Create){
            float w=std::abs(currentX-startX),h=std::abs(currentY-startY);
            if(w>=8&&h>=8)state.create({{"x",std::min(startX,currentX)},{"y",std::min(startY,currentY)},
                {"width",w},{"height",h}},state.revision);
        }
        mode=None;dragged.clear();state.stateSequence++;
    }
    void cancel(ConfigState& state){
        if((mode==Move||mode==Resize)&&state.index(dragged)>=0){
            json restore;
            for(auto key:{"x","y","width","height","anchor_x","anchor_y","reference_width","reference_height"})restore[key]=original[key];
            state.update(dragged,restore,state.revision);
        }
        mode=None;dragged.clear();state.stateSequence++;
    }
    static void border(Dib& image,int x,int y,int width,int height,DWORD color){
        auto horizontal=[&](int left,int top,int length){
            if(top<0||top>=image.height)return;
            auto row=reinterpret_cast<DWORD*>(image.bits)+size_t(top)*image.width;
            for(int at=std::max(0,left);at<std::min(image.width,left+length);at++)row[at]=color;
        };
        horizontal(x,y,width);horizontal(x,y+height-1,width);
        for(int at=std::max(0,y);at<std::min(image.height,y+height);at++){
            auto row=reinterpret_cast<DWORD*>(image.bits)+size_t(at)*image.width;
            if(x>=0&&x<image.width)row[x]=color;
            if(x+width-1>=0&&x+width-1<image.width)row[x+width-1]=color;
        }
    }
    void frame(ConfigState& state,std::vector<std::unique_ptr<Surface>>& surfaces,HWND target,double seconds){
        if(!available(target)){hide();return;}
        ensure_window();RECT box=client_box(target);int w=box.right-box.left,h=box.bottom-box.top;
        UINT dpi=GetDpiForWindow(target);if(!dpi||w<=0||h<=0){hide();return;}scale=dpi/96.f;
        // Keep the full client area clickable, including the blank space between drawings.
        if(GetWindow(hwnd,GW_OWNER)!=target)SetWindowLongPtrW(hwnd,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(target));
        const BYTE nextOpacity=(picking||referencePicking)?1:20;
        if(opacity!=nextOpacity){
            if(!SetLayeredWindowAttributes(hwnd,0,nextOpacity,LWA_ALPHA))throw std::runtime_error("Cannot shade edit input");
            opacity=nextOpacity;
        }
        if(!EqualRect(&box,&inputBox)){
            if(!SetWindowPos(hwnd,nullptr,box.left,box.top,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER))
                throw std::runtime_error("Cannot position edit input");
            inputBox=box;
        }

        struct PaintItem {RECT box;DWORD border;bool handle;Surface* surface;};
        std::vector<PaintItem> items;
        std::vector<RECT> rectangles;
        const RECT viewport{0,0,w,h};
        auto stroke=[&](int x,int y,int width,int height,DWORD color,bool handle=false,Surface* surface=nullptr){
            RECT rect{x,y,x+width,y+height},clipped{};
            if(IntersectRect(&clipped,&rect,&viewport)){
                items.push_back({rect,color,handle,surface});
                rectangles.push_back(clipped);
            }
        };
        auto drag_stroke=[&](DWORD color){
            stroke(int(std::min(startX,currentX)*scale),int(std::min(startY,currentY)*scale),
                std::max(1,int(std::abs(currentX-startX)*scale)),std::max(1,int(std::abs(currentY-startY)*scale)),color);
        };
        if(!picking&&!referencePicking){
            for(auto& surface:surfaces){
                int index=state.index(surface->region.source.at("id"));
                if(index<0)continue;
                surface->region=positioned(state,index);
                const auto& r=surface->region;
                if(!r.drawable)continue;
                int x=int(std::round(r.x*scale)),y=int(std::round(r.y*scale));
                int width=int(std::round(r.width*scale)),height=int(std::round(r.height*scale));
                bool selected=r.source["id"]==state.selected;
                stroke(x,y,width,height,selected?0xff8bdcc0:0xffa0aaa8,selected&&!r.visual,surface.get());
            }
            if(mode==Create)drag_stroke(0xff8bdcc0);
        }
        if(mode==Reference)drag_stroke(0xff59c7ff);
        if(preview.is_object()&&!preview["reference"].is_null()&&!referencePicking){
            const auto& r=preview["reference"];double x=r[0],y=r[1],width=r[2],height=r[3];
            const auto& search=preview.contains("search_reference")?preview["search_reference"]:r;
            stroke(int((search[0].get<double>()-32)*scale),int((search[1].get<double>()-32)*scale),
                int((search[2].get<double>()+64)*scale),int((search[3].get<double>()+64)*scale),0xff7c828a);
            stroke(int(x*scale),int(y*scale),int(width*scale),int(height*scale),0xff59c7ff);
        }

        // Animated text changes pixels without changing the grouping geometry.
        if(!std::equal(rectangles.begin(),rectangles.end(),plannedRectangles.begin(),plannedRectangles.end(),
            [](const RECT& a,const RECT& b){return EqualRect(&a,&b)!=FALSE;})){
            drawingPlan=plan_rect_groups(rectangles);
            plannedRectangles=rectangles;
        }
        const auto& plan=drawingPlan;
        size_t bytes=0;
        for(const auto& group:plan){
            const auto& b=group.bounds;
            bytes+=size_t(b.right-b.left)*(b.bottom-b.top)*4;
        }
        if(bytes>128*1024*1024)throw std::runtime_error("Edit groups exceed the 128 MiB bitmap limit");
        while(drawings.size()<plan.size())drawings.push_back(std::make_unique<Drawing>());
        // The compositor retains the last uploaded frame while buffers are replaced.
        for(size_t i=0;i<drawings.size();i++){
            auto& image=drawings[i]->image;
            if(i>=plan.size()||image.width!=plan[i].bounds.right-plan[i].bounds.left||
                image.height!=plan[i].bounds.bottom-plan[i].bounds.top)image.reset();
        }
        for(size_t i=0;i<plan.size();i++){
            const auto& group=plan[i];
            const auto& bounds=group.bounds;
            int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
            auto& drawing=*drawings[i];
            drawing.ensure(hwnd,width,height);
            auto pixels=reinterpret_cast<DWORD*>(drawing.image.bits);
            std::fill(pixels,pixels+size_t(width)*height,0);
            for(auto index:group.indices){
                auto surface=items[index].surface;
                if(!surface)continue;
                const auto& rect=rectangles[index];
                int rw=rect.right-rect.left,rh=rect.bottom-rect.top;
                try{surface->resize(rw,rh,float(dpi));surface->render(box.left+rect.left,box.top+rect.top,seconds,false);}
                catch(...){surface->release();throw;}
                auto source=static_cast<const DWORD*>(surface->bits);
                for(int y=0;y<rh;y++)for(int x=0;x<rw;x++){
                    size_t offset=size_t(rect.top-bounds.top+y)*width+rect.left-bounds.left+x;
                    pixels[offset]=pixel_over(source[size_t(y)*rw+x],pixels[offset]);
                }
            }
            for(auto index:group.indices){
                const auto& line=items[index];
                int x=line.box.left-bounds.left,y=line.box.top-bounds.top;
                int rw=line.box.right-line.box.left,rh=line.box.bottom-line.box.top;
                border(drawing.image,x,y,rw,rh,line.border);
                if(line.handle){int side=std::max(5,int(6*scale));
                    for(int py=std::max(0,y+rh-side);py<std::min(height,y+rh);py++)
                        for(int px=std::max(0,x+rw-side);px<std::min(width,x+rw);px++)pixels[size_t(py)*width+px]=0xff8bdcc0;
                }
            }
            POINT origin{box.left+bounds.left,box.top+bounds.top},source{};SIZE size{width,height};
            BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            if(!UpdateLayeredWindow(drawing.window,nullptr,&origin,&size,drawing.image.dc,&source,0,&blend,ULW_ALPHA))
                throw std::runtime_error("Cannot update grouped edit preview");
        }
        for(size_t i=plan.size();i<activeDrawings;i++)drawings[i]->hide();
        activeDrawings=plan.size();
        if(!IsWindowVisible(hwnd))ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        for(size_t i=0;i<activeDrawings;i++)if(!IsWindowVisible(drawings[i]->window))
            ShowWindow(drawings[i]->window,SW_SHOWNOACTIVATE);
        HWND anchor=GetWindow(target,GW_HWNDPREV);
        auto ours=[&](HWND window){
            if(window==hwnd)return true;
            return std::any_of(drawings.begin(),drawings.end(),[&](const auto& drawing){
                return drawing->valid()&&drawing->window==window;
            });
        };
        while(anchor&&ours(anchor))anchor=GetWindow(anchor,GW_HWNDPREV);
        if(anchor&&(GetWindowLongPtrW(anchor,GWL_EXSTYLE)&WS_EX_TOPMOST))anchor=nullptr;
        constexpr UINT flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER;
        HWND previous=anchor;
        bool arranged=true;
        for(size_t i=0;i<activeDrawings;i++){
            HWND drawing=drawings[i]->window;
            if(GetWindow(drawing,GW_HWNDPREV)!=previous)arranged=false;
            previous=drawing;
        }
        if(!arranged||GetWindow(hwnd,GW_HWNDPREV)!=previous){
            auto stack=[&](HWND window){
                if(!SetWindowPos(window,anchor,0,0,0,0,flags))
                    throw std::runtime_error("Cannot position edit layers in the window stack");
            };
            stack(hwnd);
            for(size_t i=0;i<activeDrawings;i++){
                HWND drawing=drawings[i]->window;
                stack(drawing);
                anchor=drawing;
            }
        }
    }
};
