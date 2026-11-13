#pragma once
#include "render.hpp"

inline void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline std::vector<BYTE> image(Surface& s,double time){s.render(0,0,time);BYTE* data=static_cast<BYTE*>(s.bits);return {data,data+size_t(s.width)*s.height*4};}
inline void png(Renderer& renderer,Surface& surface,const fs::path& path){
    Com<IWICBitmap> bitmap;Com<IWICFormatConverter> converter;Com<IWICStream> stream;
    Com<IWICBitmapEncoder> encoder;Com<IWICBitmapFrameEncode> frame;
    check(renderer.wic->CreateBitmapFromMemory(surface.width,surface.height,GUID_WICPixelFormat32bppPBGRA,
          surface.width*4,surface.width*surface.height*4,static_cast<BYTE*>(surface.bits),bitmap.put()));
    check(renderer.wic->CreateFormatConverter(converter.put()));
    check(converter->Initialize(bitmap.value,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    check(renderer.wic->CreateStream(stream.put()));check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    check(renderer.wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));
    check(encoder->Initialize(stream.value,WICBitmapEncoderNoCache));check(encoder->CreateNewFrame(frame.put(),nullptr));
    check(frame->Initialize(nullptr));check(frame->SetSize(surface.width,surface.height));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&format));
    check(frame->WriteSource(converter.value,nullptr));check(frame->Commit());check(encoder->Commit());
}
inline void render_tests(Renderer& renderer,const fs::path& output){
    fs::create_directories(output);
    json report=json::array();
    bool invalidColor=false;
    try{color("#12#456");}catch(const std::exception&){invalidColor=true;}
    require(invalidColor,"malformed color was accepted");
    {
        size_t saved=renderer.surfaceBytes;renderer.surfaceBytes=Renderer::surfaceLimit;
        bool limited=false;
        Surface excessive(renderer,Region(json::object()));
        try{excessive.resize(160,40,96);}catch(const std::exception&){limited=true;}
        require(limited&&!excessive.dib&&!excessive.outputBytes,"surface budget did not reject allocation safely");
        renderer.surfaceBytes=saved;
        report.push_back({{"output_budget",Renderer::surfaceLimit},{"budget_failure_safe",true}});
    }
    {
        json anchored={{"text","AAA"},{"width",180},{"height",60},{"size",30},{"padding",0},{"opacity",0}};
        Surface base(renderer,Region(anchored));base.resize(180,60,96);auto first=image(base,0);
        anchored["letter_spacing"]=4;
        Surface spaced(renderer,Region(anchored));spaced.resize(180,60,96);auto second=image(spaced,0);
        auto leftmost=[](const std::vector<BYTE>& pixels){int left=180;
            for(int y=0;y<60;y++)for(int x=0;x<180;x++)if(pixels[(y*180+x)*4+3])left=std::min(left,x);return left;};
        require(leftmost(first)==leftmost(second),"letter spacing moved the first glyph anchor");
        require(spaced.advance>base.advance,"letter spacing did not increase advance");
        report.push_back({{"spacing_preserves_first_glyph",true}});
    }
    for(const auto& sample:std::vector<std::pair<std::string,std::string>>{
            {"中文，标点。\n第二行 ABC","Overlay Missing Font Test"},
            {"ABC 😀 مرحبا","Segoe UI"}}){
        Surface text(renderer,Region(json{{"text",sample.first},{"font",sample.second},{"width",360},{"height",100},{"size",24},{"opacity",0}}));
        text.resize(360,100,96);auto pixels=image(text,0);
        require(!text.missingGlyphs,"fallback or non-BMP text has missing glyphs");
        bool visible=false;for(size_t i=3;i<pixels.size();i+=4)visible=visible||pixels[i]!=0;
        require(visible,"fallback or non-BMP text rendered blank");
        report.push_back({{"fallback_text",sample.first},{"missing_glyphs",text.missingGlyphs}});
    }
    for(float dpi:{96.f,168.f})for(bool right:{false,true}){
        json values={{"text","████████████████████"},{"width",180},{"height",60},{"size",30},{"padding",0},
            {"opacity",0},{"scroll",true},{"outline",true},{"glow",true},{"glow_pulse",false},{"scroll_feather_width",30},
            {"scroll_direction",right?"right":"left"}};
        Surface hard(renderer,Region(values));hard.resize(int(180*dpi/96),int(60*dpi/96),dpi);auto a=image(hard,0);
        values["scroll_feather"]=true;
        Surface soft(renderer,Region(values));soft.resize(hard.width,hard.height,dpi);auto b=image(soft,0);
        auto energy=[&](const std::vector<BYTE>& pixels,int left,int right){unsigned long long result=0;
            for(int y=0;y<hard.height;y++)for(int x=left;x<right;x++)result+=pixels[(size_t(y)*hard.width+x)*4+3];return result;};
        int edge=int(12*dpi/96),left=int(70*dpi/96),end=int(110*dpi/96);
        require(energy(b,0,edge)<energy(a,0,edge),"left feather is not fading");
        require(energy(b,hard.width-edge,hard.width)<energy(a,hard.width-edge,hard.width),"right feather is not fading");
        require(energy(a,left,end)==energy(b,left,end),"feather changed central text");
        double period=(std::max(hard.advance,180.f)+hard.region.gap)/hard.region.speed;
        auto repeat=image(hard,period);
        unsigned long long difference=0;for(size_t i=0;i<a.size();i++)difference+=std::abs(int(a[i])-int(repeat[i]));
        require(difference<=a.size()/100,"scroll cycle is not continuous");
        require(!hard.missingGlyphs,"block font has missing glyphs");
        if(dpi==96&&!right){
            auto reference=image(soft,.5);
            Surface clipped(renderer,Region(values));clipped.resize(120,30,96);
            auto crop=image(clipped,.5);
            for(int y=0;y<30;y++)for(int x=0;x<120*4;x++)
                require(crop[size_t(y)*120*4+x]==reference[size_t(y)*180*4+x],"partial client clipping changed content origin");
            report.push_back({{"partial_client_clip",true},{"mask_origin","original_region"}});
        }
        png(renderer,soft,output/(std::to_wstring(int(dpi))+(right?L"-right.png":L"-left.png")));
        report.push_back({{"dpi",dpi},{"right",right},{"feather",true},{"cycle_difference",difference}});
    }
    json values={{"text","我的工作台 · ABC Glow"},{"width",360},{"height",80},{"size",26},{"scroll",true},{"glow",true},{"outline",true}};
    Surface normal(renderer,Region(values));normal.resize(630,140,168);auto bright=image(normal,0);
    require(!normal.missingGlyphs,"CJK/Latin fallback has missing glyphs");
    require(bright!=image(normal,normal.region.glowPeriod/2),"glow breathing does not change pixels");
    {
        auto fixed=values;fixed["glow_pulse"]=false;
        Surface moving(renderer,Region(fixed));moving.resize(630,140,168);
        require(image(moving,0)!=image(moving,1),"cached text stopped scrolling");
        fixed["scroll"]=false;fixed["glow_pulse"]=true;
        Surface breathing(renderer,Region(fixed));breathing.resize(630,140,168);
        require(image(breathing,0)!=image(breathing,breathing.region.glowPeriod/2),"cached glow stopped breathing");
    }
    values["letter_spacing"]=4;
    Surface spaced(renderer,Region(values));spaced.resize(630,140,168);image(spaced,0);
    require(spaced.advance>normal.advance,"letter spacing does not affect text layout");
    png(renderer,spaced,output/L"chinese-spacing-glow.png");
    report.push_back({{"cjk_glyphs",true},{"glow_pulse",true},{"spacing_advance",spaced.advance},{"normal_advance",normal.advance}});
    {
        // Force the bounded-cache path while retaining every requested effect.
        size_t savedCache=renderer.cachedBytes;
        renderer.cachedBytes=Renderer::cacheLimit+1;
        Surface uncached(renderer,Region(values));uncached.resize(630,140,168);
        require(!uncached.core.value&&!uncached.halo.value&&!uncached.cpuCore.dc,"cache budget was exceeded");
        auto frame=image(uncached,0);
        require(frame!=image(uncached,1),"uncached scroll stopped moving");
        require(!uncached.missingGlyphs,"uncached fallback lost CJK glyphs");
        png(renderer,uncached,output/L"uncached-effects.png");
        values["scroll"]=false;
        Surface pulse(renderer,Region(values));pulse.resize(630,140,168);
        require(image(pulse,0)!=image(pulse,pulse.region.glowPeriod/2),"uncached glow stopped breathing");
        values["scroll"]=true;
        values["text"]=std::string(2000,'W');
        Surface longText(renderer,Region(values));longText.resize(630,140,168);
        require(!longText.core.value,"oversize text unexpectedly cached");
        require(image(longText,0)!=image(longText,1),"oversize text stopped moving");
        // Restore accounting for still-live normal/spaced surfaces.
        renderer.cachedBytes=savedCache;
        report.push_back({{"uncached_effects",true},{"cache_budget",Renderer::cacheLimit}});
    }
    std::ofstream(output/L"result.json")<<report.dump(2);
}
