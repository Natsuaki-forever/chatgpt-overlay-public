#pragma once
#include "edit_canvas.hpp"

inline std::vector<SurfaceGroup> plan_surfaces(const std::vector<std::unique_ptr<Surface>>& surfaces,int width,int height,float scale,bool separate=false){
    std::vector<RECT> rectangles(surfaces.size());
    for(size_t i=0;i<surfaces.size();i++){
        const auto& r=surfaces[i]->region;
        if(!r.drawable)continue;
        int x=int(std::round(r.x*scale)),y=int(std::round(r.y*scale));
        RECT box{x,y,std::min(width,x+int(std::round(r.width*scale))),std::min(height,y+int(std::round(r.height*scale)))};
        if(box.right>box.left&&box.bottom>box.top)rectangles[i]=box;
    }
    return plan_rect_groups(rectangles,separate);
}

struct GroupWindow {
    Renderer& renderer;
    Dib image;
    HWND hwnd=nullptr;
    size_t bytes=0;
    explicit GroupWindow(Renderer& value):renderer(value){}
    bool valid()const{return IsWindow(hwnd)&&GetWindowLongPtrW(hwnd,GWLP_USERDATA)==reinterpret_cast<LONG_PTR>(this);}
    void ensure(int w,int h){
        if(!valid()){
            hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,L"OverlayNativeSurface",L"Overlay group",WS_POPUP,
                0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(!hwnd)throw std::runtime_error("Cannot create group window");
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        }
        if(image.width==w&&image.height==h)return;
        renderer.surfaceBytes-=bytes;bytes=0;image.reset();
        size_t needed=size_t(w)*h*4;
        if(needed>Renderer::surfaceLimit||renderer.surfaceBytes>Renderer::surfaceLimit-needed)
            throw std::runtime_error("Grouped surfaces exceed the 64 MiB output budget");
        image.create(w,h);bytes=needed;renderer.surfaceBytes+=bytes;
    }
    ~GroupWindow(){renderer.surfaceBytes-=bytes;if(valid())DestroyWindow(hwnd);}
};

struct NormalSurfaces {
    Renderer& renderer;
    std::vector<SurfaceGroup> plan;
    std::vector<std::unique_ptr<GroupWindow>> windows;
    int viewportWidth=0,viewportHeight=0;
    float viewportScale=0;
    bool separate=false; // Single-visual MVP keeps qualified HWNDs stable.
    explicit NormalSurfaces(Renderer& value):renderer(value){}
    void clear(){windows.clear();plan.clear();}
    void hide(){for(auto& window:windows)if(window&&window->valid())ShowWindow(window->hwnd,SW_HIDE);}
    HWND window(size_t i,const std::vector<std::unique_ptr<Surface>>& surfaces)const{
        return plan[i].indices.size()==1?surfaces[plan[i].indices[0]]->hwnd:windows[i]->hwnd;
    }
    bool animated(size_t i,const std::vector<std::unique_ptr<Surface>>& surfaces)const{
        for(auto index:plan[i].indices)if(surfaces[index]->region.animated())return true;return false;
    }
    bool active(const std::vector<std::unique_ptr<Surface>>& surfaces)const{
        for(size_t i=0;i<plan.size();i++)if(IsWindowVisible(window(i,surfaces))&&animated(i,surfaces))return true;return false;
    }
    void compose(size_t i,std::vector<std::unique_ptr<Surface>>& surfaces,RECT target,float scale,double time,bool tick){
        auto& group=plan[i];bool single=group.indices.size()==1;
        for(auto index:group.indices){
            auto& surface=*surfaces[index];auto& r=surface.region;
            int x=int(std::round(r.x*scale)),y=int(std::round(r.y*scale));
            int w=std::min(int(target.right-target.left)-x,int(std::round(r.width*scale)));
            int h=std::min(int(target.bottom-target.top)-y,int(std::round(r.height*scale)));
            bool changed=surface.width!=w||surface.height!=h||surface.dpi!=scale*96;
            surface.resize(w,h,scale*96);
            if(single||changed||!tick||surface.region.animated())surface.render(target.left+x,target.top+y,time,single);
        }
        if(single)return;
        auto& out=*windows[i];auto destination=reinterpret_cast<DWORD*>(out.image.bits);
        std::fill(destination,destination+size_t(out.image.width)*out.image.height,0);
        for(auto index:group.indices){
            const auto& source=*surfaces[index];
            int x=int(std::round(source.region.x*scale))-group.bounds.left,y=int(std::round(source.region.y*scale))-group.bounds.top;
            auto pixels=static_cast<const DWORD*>(source.bits);
            for(int row=0;row<source.height;row++)for(int col=0;col<source.width;col++){
                size_t at=size_t(y+row)*out.image.width+x+col;destination[at]=pixel_over(pixels[size_t(row)*source.width+col],destination[at]);
            }
        }
        POINT origin{target.left+group.bounds.left,target.top+group.bounds.top},source{};SIZE size{out.image.width,out.image.height};
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(!UpdateLayeredWindow(out.hwnd,nullptr,&origin,&size,out.image.dc,&source,0,&blend,ULW_ALPHA))throw std::runtime_error("Cannot upload grouped surface");
    }
    void sync(std::vector<std::unique_ptr<Surface>>& surfaces,HWND target,double time,ConfigState* state=nullptr){
        RECT box=client_box(target);float scale=GetDpiForWindow(target)/96.f;if(scale<=0)return;
        if(state)state->set_target_geometry(target);
        viewportWidth=box.right-box.left;viewportHeight=box.bottom-box.top;viewportScale=scale;
        for(auto& surface:surfaces){
            if(state){int i=state->index(surface->region.source.at("id"));if(i>=0)surface->region=state->positioned(i);else surface->region.drawable=false;}
            else surface->region.locate(viewportWidth/scale,viewportHeight/scale);
        }
        auto next=plan_surfaces(surfaces,box.right-box.left,box.bottom-box.top,scale,separate);
        bool rebuilt=next!=plan;
        if(rebuilt){clear();for(size_t i=0;i<surfaces.size();i++){
                bool retained=separate&&std::any_of(next.begin(),next.end(),[&](const auto& g){return g.indices.size()==1&&g.indices[0]==i;});
                if(!retained)surfaces[i]->hide();
            }plan=std::move(next);
            for(auto& group:plan)windows.push_back(group.indices.size()>1?std::make_unique<GroupWindow>(renderer):nullptr);
        }
        for(size_t i=0;i<plan.size();i++){
            if(plan[i].indices.size()==1)surfaces[plan[i].indices[0]]->ensure_window();
            else{
                for(auto index:plan[i].indices)surfaces[index]->close_window();
                auto& r=plan[i].bounds;windows[i]->ensure(r.right-r.left,r.bottom-r.top);
            }
        }
        HWND anchor=GetWindow(target,GW_HWNDPREV);
        auto ours=[&](HWND candidate){for(size_t i=0;i<plan.size();i++)if(window(i,surfaces)==candidate)return true;return false;};
        while(anchor&&ours(anchor))anchor=GetWindow(anchor,GW_HWNDPREV);
        if(anchor&&(GetWindowLongPtrW(anchor,GWL_EXSTYLE)&WS_EX_TOPMOST))anchor=nullptr;
        for(size_t i=plan.size();i-->0;){
            HWND hwnd=window(i,surfaces);auto& bounds=plan[i].bounds;
            bool dirty=rebuilt||!IsWindowVisible(hwnd);
            for(auto index:plan[i].indices){auto& s=*surfaces[index];if(!s.width||s.dpi!=scale*96)dirty=true;}
            RECT current=client_box(hwnd);
            if(dirty||current.left!=box.left+bounds.left||current.top!=box.top+bounds.top)compose(i,surfaces,box,scale,time,false);
            if(GetWindow(hwnd,GW_OWNER)!=target)SetWindowLongPtrW(hwnd,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(target));
            if(!IsWindowVisible(hwnd))ShowWindow(hwnd,SW_SHOWNOACTIVATE);
            if(GetWindow(hwnd,GW_HWNDPREV)!=anchor)SetWindowPos(hwnd,anchor,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            anchor=hwnd;
        }
    }
    void animate(std::vector<std::unique_ptr<Surface>>& surfaces,HWND target,double time,ConfigState* state=nullptr){
        if(state&&!state->visualPositions.empty()){
            auto sequence=state->stateSequence;state->set_target_geometry(target);state->expire_visual();
            if(sequence!=state->stateSequence){sync(surfaces,target,time,state);return;}
        }
        RECT box=client_box(target);float scale=GetDpiForWindow(target)/96.f;
        if(box.right-box.left!=viewportWidth||box.bottom-box.top!=viewportHeight||scale!=viewportScale){
            sync(surfaces,target,time,state);return;
        }
        for(size_t i=0;i<plan.size();i++)if(IsWindowVisible(window(i,surfaces))&&animated(i,surfaces))compose(i,surfaces,box,scale,time,true);
    }
};
