#pragma once
#include "core.hpp"
#include "dib.hpp"

struct Apartment { Apartment(){check(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED));} ~Apartment(){CoUninitialize();} };
struct Renderer {
    Apartment apartment;
    Com<ID2D1Factory> d2d;
    Com<IDWriteFactory> write;
    Com<IWICImagingFactory> wic;
    Com<ID2D1StrokeStyle> stroke;
    size_t cachedBytes=0;
    static constexpr size_t cacheLimit=8*1024*1024;
    size_t surfaceBytes=0;
    static constexpr size_t surfaceLimit=64*1024*1024;
    bool cpuComposite=true;
    Renderer() {
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put()));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.put())));
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())));
        auto props=D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND);
        check(d2d->CreateStrokeStyle(props,nullptr,0,stroke.put()));
    }
    void bitmap(UINT w,UINT h,float dpi,Com<IWICBitmap>& bitmap,Com<ID2D1RenderTarget>& rt) {
        check(wic->CreateBitmap(w,h,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));
        auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi);
        check(d2d->CreateWicBitmapRenderTarget(bitmap.value,props,rt.put()));
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
};

struct GlyphPainter : IDWriteTextRenderer {
    Renderer& renderer;
    ID2D1RenderTarget* rt;
    const Region& region;
    bool halo;
    ULONG references=1;
    unsigned missingGlyphs=0;
    GlyphPainter(Renderer& renderer,ID2D1RenderTarget* rt,const Region& region,bool halo):renderer(renderer),rt(rt),region(region),halo(halo){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDWriteTextRenderer)||iid==__uuidof(IDWritePixelSnapping)){*out=static_cast<IDWriteTextRenderer*>(this);AddRef();return S_OK;}
        *out=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{return --references;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled) override{*disabled=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* matrix) override{*matrix={1,0,0,1,0,0};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override{float y;rt->GetDpi(value,&y);*value/=96;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,
                                            const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*) override {
        for(UINT32 i=0;i<run->glyphCount;i++)if(!run->glyphIndices[i])missingGlyphs++;
        Com<ID2D1PathGeometry> path;Com<ID2D1GeometrySink> sink;
        HRESULT hr=renderer.d2d->CreatePathGeometry(path.put());if(FAILED(hr))return hr;
        hr=path->Open(sink.put());if(FAILED(hr))return hr;
        hr=run->fontFace->GetGlyphRunOutline(run->fontEmSize,run->glyphIndices,run->glyphAdvances,run->glyphOffsets,run->glyphCount,run->isSideways,run->bidiLevel%2,sink.value);
        if(FAILED(hr))return hr;
        hr=sink->Close();if(FAILED(hr))return hr;
        D2D1_MATRIX_3X2_F previous;rt->GetTransform(&previous);
        rt->SetTransform(D2D1::Matrix3x2F::Translation(x,y)*previous);
        Com<ID2D1SolidColorBrush> brush;
        hr=rt->CreateSolidColorBrush(region.foreground,brush.put());if(FAILED(hr))return hr;
        if(halo){
            for(int step=8;step>0;--step){auto tint=region.glowColor;tint.a=.035f+(1-step/8.f)*.1f;
                brush->SetColor(tint);
                rt->DrawGeometry(path.value,brush.value,2*(region.glowRadius*step/8.f+(region.outline?region.outlineWidth:0)),renderer.stroke.value);
            }
        }else{
            if(region.outline){brush->SetColor(region.outlineColor);rt->DrawGeometry(path.value,brush.value,2*region.outlineWidth,renderer.stroke.value);}
            brush->SetColor(region.foreground);rt->FillGeometry(path.value,brush.value);
        }
        rt->SetTransform(previous);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override{return E_NOTIMPL;}
};

struct Surface {
    HWND hwnd=nullptr;
    Renderer& renderer;
    Region region;
    Com<IWICBitmap> bitmap;
    Com<ID2D1RenderTarget> rt;
    Com<IDWriteTextLayout> layout;
    Com<ID2D1Bitmap> core,halo;
    Com<ID2D1Layer> fadeLayer;
    Com<ID2D1LinearGradientBrush> fadeBrush;
    HBITMAP dib=nullptr,oldBitmap=nullptr;
    HDC dc=nullptr;
    void* bits=nullptr;
    int width=0,height=0;
    float dpi=0,advance=0,margin=0,spriteWidth=0;
    unsigned missingGlyphs=0;
    unsigned long long frames=0;
    size_t cacheBytes=0;
    size_t outputBytes=0;
    Dib cpuCore,cpuHalo;
    std::vector<BYTE> featherMask;
    double drawMilliseconds=0,blendMilliseconds=0,uploadMilliseconds=0;
    explicit Surface(Renderer& renderer,const Region& region,bool createWindow=true):renderer(renderer),region(region){
        if(createWindow)ensure_window();
    }
    bool valid_window()const{return IsWindow(hwnd)&&GetWindowLongPtrW(hwnd,GWLP_USERDATA)==reinterpret_cast<LONG_PTR>(this);}
    void hide(){if(valid_window())ShowWindow(hwnd,SW_HIDE);}
    void close_window(){if(valid_window())DestroyWindow(hwnd);hwnd=nullptr;}
    void ensure_window(){
        if(valid_window())return;
        hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,L"OverlayNativeSurface",L"",WS_POPUP,
                             0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hwnd)throw std::runtime_error("Cannot create surface");
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
    }
    ~Surface(){release();if(valid_window())DestroyWindow(hwnd);}
    void release(){
        renderer.cachedBytes-=cacheBytes;cacheBytes=0;
        renderer.surfaceBytes-=outputBytes;outputBytes=0;
        cpuCore.reset();cpuHalo.reset();featherMask.clear();
        if(dc){SelectObject(dc,oldBitmap);DeleteDC(dc);dc=nullptr;}
        if(dib){DeleteObject(dib);dib=nullptr;}
        bits=nullptr;core=Com<ID2D1Bitmap>();halo=Com<ID2D1Bitmap>();
        fadeLayer=Com<ID2D1Layer>();fadeBrush=Com<ID2D1LinearGradientBrush>();
        layout=Com<IDWriteTextLayout>();rt=Com<ID2D1RenderTarget>();bitmap=Com<IWICBitmap>();width=height=0;
    }
    void sprite(bool glow,Com<ID2D1Bitmap>& out,Dib& cpu){
        Com<IWICBitmap> image;Com<ID2D1RenderTarget> paint;
        renderer.bitmap(UINT(std::ceil(spriteWidth*dpi/96)),UINT(std::ceil(region.height*dpi/96)),dpi,image,paint);
        paint->BeginDraw();paint->Clear(D2D1::ColorF(0,0));
        GlyphPainter glyphs(renderer,paint.value,region,glow);
        check(layout->Draw(nullptr,&glyphs,margin+(region.scroll?0:region.padding),0));
        if(!glow)missingGlyphs=glyphs.missingGlyphs;
        check(paint->EndDraw());
        auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi);
        if(renderer.cpuComposite){
            UINT w,h;check(image->GetSize(&w,&h));cpu.create(int(w),int(h));
            check(image->CopyPixels(nullptr,w*4,w*h*4,cpu.bits));
        }else check(rt->CreateBitmapFromWicBitmap(image.value,&props,out.put()));
    }
    void resize(int w,int h,float newDpi){
        if(width==w&&height==h&&dpi==newDpi)return;
        release();
        if(w<=0||h<=0)throw std::runtime_error("Invalid surface dimensions");
        size_t required=size_t(w)*size_t(h)*4;
        if(required>Renderer::surfaceLimit||renderer.surfaceBytes>Renderer::surfaceLimit-required)
            throw std::runtime_error("Overlay surfaces exceed the 64 MiB output budget");
        outputBytes=required;renderer.surfaceBytes+=required;
        width=w;height=h;dpi=newDpi;
        renderer.bitmap(w,h,dpi,bitmap,rt);
        Com<IDWriteTextFormat> format;
        check(renderer.write->CreateTextFormat(region.family.c_str(),nullptr,region.bold?DWRITE_FONT_WEIGHT_BOLD:DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,region.size,L"zh-cn",format.put()));
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetTextAlignment(region.scroll?DWRITE_TEXT_ALIGNMENT_LEADING:region.align=="center"?DWRITE_TEXT_ALIGNMENT_CENTER:region.align=="right"?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetWordWrapping(region.scroll?DWRITE_WORD_WRAPPING_NO_WRAP:DWRITE_WORD_WRAPPING_WRAP);
        std::wstring text=region.text;if(region.scroll)for(auto& ch:text)if(ch==L'\n'||ch==L'\r')ch=L' ';
        check(renderer.write->CreateTextLayout(text.data(),UINT32(text.size()),format.value,
            region.scroll?1000000.f:std::max(1.f,region.width-2*region.padding),region.height,layout.put()));
        Com<IDWriteTextLayout1> advanced;
        if(SUCCEEDED(layout->QueryInterface(IID_PPV_ARGS(advanced.put()))))check(advanced->SetCharacterSpacing(0,region.spacing,0,{0,UINT32(text.size())}));
        DWRITE_TEXT_METRICS metrics;check(layout->GetMetrics(&metrics));advance=metrics.widthIncludingTrailingWhitespace;
        margin=(region.glow?region.glowRadius:0)+(region.outline?region.outlineWidth:0)+2;
        spriteWidth=(region.scroll?advance:region.width)+2*margin;
        double pixels=std::ceil(spriteWidth*dpi/96)*std::ceil(region.height*dpi/96);
        size_t bytes=size_t(pixels)*4*(region.glow?2:1);
        if(pixels<=1000000 && spriteWidth*dpi/96<=16384 && region.height*dpi/96<=16384 &&
            bytes<=Renderer::cacheLimit && renderer.cachedBytes<=Renderer::cacheLimit-bytes){
            sprite(false,core,cpuCore);if(region.glow)sprite(true,halo,cpuHalo);
            cacheBytes=bytes;renderer.cachedBytes+=bytes;
        }
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        HDC newDC=CreateCompatibleDC(nullptr);void* newBits=nullptr;
        HBITMAP newDib=newDC?CreateDIBSection(newDC,&info,DIB_RGB_COLORS,&newBits,nullptr,0):nullptr;
        if(!newDC||!newDib){if(newDib)DeleteObject(newDib);if(newDC)DeleteDC(newDC);throw std::runtime_error("Cannot allocate surface bitmap");}
        dc=newDC;dib=newDib;bits=newBits;oldBitmap=HBITMAP(SelectObject(dc,dib));
        if(cpuCore.dc){
            featherMask.assign(w,255);
            if(region.scroll&&region.feather)for(int x=0;x<w;x++){
                float logical=(x+.5f)*96/dpi;
                float edge=std::min(region.featherWidth,region.width/2);
                featherMask[x]=BYTE(std::round(255*std::clamp(std::min(logical,region.width-logical)/edge,0.f,1.f)));
            }
            // Cached glyphs and the output DIB suffice for ordinary frames.
            rt=Com<ID2D1RenderTarget>();bitmap=Com<IWICBitmap>();
            return;
        }
        if(region.scroll&&region.feather){
            float fraction=std::min(.5f,region.featherWidth/region.width);
            std::vector<D2D1_GRADIENT_STOP> stops={{0,D2D1::ColorF(1,1,1,0)},{fraction,D2D1::ColorF(1,1,1,1)}};
            if(fraction<.5f)stops.push_back({1-fraction,D2D1::ColorF(1,1,1,1)});
            stops.push_back({1,D2D1::ColorF(1,1,1,0)});
            Com<ID2D1GradientStopCollection> collection;
            check(rt->CreateGradientStopCollection(stops.data(),UINT32(stops.size()),D2D1_GAMMA_1_0,D2D1_EXTEND_MODE_CLAMP,collection.put()));
            check(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(region.width,0)),collection.value,fadeBrush.put()));
            check(rt->CreateLayer(D2D1::SizeF(region.width,region.height),fadeLayer.put()));
        }
    }
    void composite(double seconds){
        double period=std::max(advance,std::max(1.f,region.width-2*region.padding))+region.gap;
        double phase=std::fmod(seconds*region.speed,period);
        double offset=region.scroll?(region.right?phase-period:-phase):0;
        int count=region.scroll?2:1,origins[2];
        for(int i=0;i<count;i++)origins[i]=int(std::round((offset+i*period-margin+(region.scroll?region.padding:0))*dpi/96));
        float opacity=region.glowStrength;
        if(region.pulse)opacity*=.25f+.75f*(.5f+.5f*float(std::cos(seconds*6.283185307179586/region.glowPeriod)));
        unsigned glowAlpha=unsigned(std::round(opacity*255));
        auto multiply=[](DWORD pixel,unsigned alpha){
            // Two independent 16-bit lanes; rounded divide by 255 for all channels.
            DWORD rb=(pixel&0x00ff00ff)*alpha+0x00800080;
            DWORD ag=((pixel>>8)&0x00ff00ff)*alpha+0x00800080;
            rb=((rb+((rb>>8)&0x00ff00ff))>>8)&0x00ff00ff;
            ag=((ag+((ag>>8)&0x00ff00ff))>>8)&0x00ff00ff;
            return rb|(ag<<8);
        };
        auto over=[&](DWORD source,DWORD destination){return source+multiply(destination,255-(source>>24));};
        unsigned bgA=unsigned(std::round(region.background.a*255));
        unsigned bgR=unsigned(std::round(region.background.r*bgA));
        unsigned bgG=unsigned(std::round(region.background.g*bgA));
        unsigned bgB=unsigned(std::round(region.background.b*bgA));
        DWORD background=(bgA<<24)|(bgR<<16)|(bgG<<8)|bgB;
        auto dest=static_cast<DWORD*>(bits);
        auto corePixels=reinterpret_cast<const DWORD*>(cpuCore.bits);
        auto haloPixels=reinterpret_cast<const DWORD*>(cpuHalo.bits);
        for(int y=0;y<height;y++)for(int x=0;x<width;x++){
            DWORD text=0;
            for(int i=0;i<count;i++){
                int sourceX=x-origins[i];
                if(sourceX<0||sourceX>=cpuCore.width)continue;
                size_t sourceIndex=size_t(y)*cpuCore.width+sourceX;
                if(haloPixels&&glowAlpha){DWORD haloPixel=multiply(haloPixels[sourceIndex],glowAlpha);if(haloPixel)text=over(haloPixel,text);}
                DWORD corePixel=corePixels[sourceIndex];if(corePixel)text=over(corePixel,text);
            }
            unsigned mask=featherMask[x];
            if(mask!=255)text=multiply(text,mask);
            dest[size_t(y)*width+x]=text?over(text,background):background;
        }
    }
    void render(int x,int y,double seconds=0,bool upload=true){
        auto start=std::chrono::steady_clock::now();
        if(cpuCore.dc)composite(seconds);
        else {
        rt->BeginDraw();rt->Clear(region.background);
        if(fadeLayer.value)rt->PushLayer(D2D1::LayerParameters(D2D1::RectF(0,0,region.width,region.height),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),1,fadeBrush.value),fadeLayer.value);
        if(core.value){
            double period=std::max(advance,std::max(1.f,region.width-2*region.padding))+region.gap;
            double phase=std::fmod(seconds*region.speed,period);
            double offset=region.scroll?(region.right?phase-period:-phase):0;
            int copies=region.scroll?2:1;
            for(int i=0;i<copies;i++){
                float left=float(offset+i*period)-margin+(region.scroll?region.padding:0);
                auto dest=D2D1::RectF(left,0,left+spriteWidth,region.height);
                if(halo.value){float opacity=region.glowStrength;
                    if(region.pulse)opacity*=.25f+.75f*(.5f+.5f*float(std::cos(seconds*6.283185307179586/region.glowPeriod)));
                    rt->DrawBitmap(halo.value,dest,opacity,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                rt->DrawBitmap(core.value,dest,1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }else{
            // Budget exhaustion changes caching only, never the configured effects.
            double period=std::max(advance,std::max(1.f,region.width-2*region.padding))+region.gap;
            double phase=std::fmod(seconds*region.speed,period);
            double offset=region.scroll?(region.right?phase-period:-phase):0;
            for(int i=0;i<(region.scroll?2:1);i++){
                float x=float(offset+i*period)+region.padding;
                if(region.glow){
                    float opacity=region.glowStrength;
                    if(region.pulse)opacity*=.25f+.75f*(.5f+.5f*float(std::cos(seconds*6.283185307179586/region.glowPeriod)));
                    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::IdentityMatrix(),opacity),nullptr);
                    GlyphPainter glow(renderer,rt.value,region,true);
                    HRESULT result=layout->Draw(nullptr,&glow,x,0);
                    rt->PopLayer();check(result);
                }
                GlyphPainter glyphs(renderer,rt.value,region,false);
                check(layout->Draw(nullptr,&glyphs,x,0));missingGlyphs=glyphs.missingGlyphs;
            }
        }
        if(fadeLayer.value)rt->PopLayer();
        check(rt->EndDraw());
        check(bitmap->CopyPixels(nullptr,width*4,width*height*4,static_cast<BYTE*>(bits)));
        }
        auto blended=std::chrono::steady_clock::now();
        auto uploaded=std::chrono::steady_clock::now();
        POINT origin{x,y},source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        if(upload&&(!valid_window()||!UpdateLayeredWindow(hwnd,nullptr,&origin,&size,dc,&source,0,&blend,ULW_ALPHA)))throw std::runtime_error("Layered surface update failed");
        frames++;
        drawMilliseconds+=std::chrono::duration<double,std::milli>(blended-start).count();
        blendMilliseconds+=std::chrono::duration<double,std::milli>(uploaded-blended).count();
        uploadMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-uploaded).count();
    }
};
