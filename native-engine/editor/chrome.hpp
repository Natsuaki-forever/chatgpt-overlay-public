#pragma once

#include <commctrl.h>
#include <gdiplus.h>

namespace chrome {
enum class Icon { Sun, Moon, System, Gear, Close, Chevron, More, Trash,
    Frame, Plus, Application, Window, Archive };

inline int px(int dip,UINT dpi){return MulDiv(dip,dpi,96);}

class DrawingSession {
    ULONG_PTR token=0;
public:
    DrawingSession(){
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token,&input,nullptr);
    }
    ~DrawingSession(){if(token)Gdiplus::GdiplusShutdown(token);}
};

inline Gdiplus::Color color(COLORREF value,BYTE opacity=255){
    return Gdiplus::Color(opacity,GetRValue(value),GetGValue(value),GetBValue(value));
}

inline bool round_path(Gdiplus::GraphicsPath& path,const RECT& bounds,int radius){
    float x=bounds.left+.5f,y=bounds.top+.5f,w=float(bounds.right-bounds.left-1),h=float(bounds.bottom-bounds.top-1);
    if(w<=0||h<=0)return false;
    float d=std::min({float(radius*2),w,h});
    path.AddArc(x,y,d,d,180,90);
    path.AddArc(x+w-d,y,d,d,270,90);
    path.AddArc(x+w-d,y+h-d,d,d,0,90);
    path.AddArc(x,y+h-d,d,d,90,90);
    path.CloseFigure();
    return true;
}

inline void rounded(HDC dc,const RECT& bounds,int radius,COLORREF fill,COLORREF border,BYTE opacity=255){
    Gdiplus::GraphicsPath path;
    if(!round_path(path,bounds,radius))return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(color(fill,opacity));
    Gdiplus::Pen pen(color(border));
    graphics.FillPath(&brush,&path);
    graphics.DrawPath(&pen,&path);
}

// A contained, native glass surface. It deliberately uses only the editor's
// own backing surface: no desktop capture or blur is involved, so text stays
// crisp and the material remains deterministic in light/dark/high-contrast.
inline void glass_surface(HDC dc,const RECT& bounds,int radius,COLORREF fill,
    COLORREF border,COLORREF highlight,BYTE opacity=210){
    Gdiplus::GraphicsPath path;
    if(!round_path(path,bounds,radius))return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush surface(color(fill,opacity));
    graphics.FillPath(&surface,&path);

    // A very restrained top sheen gives the panel a glass edge without
    // washing out its controls or turning the whole window into a gradient.
    RECT sheen=bounds;
    sheen.bottom=sheen.top+std::max<LONG>(1,(bounds.bottom-bounds.top)/3);
    Gdiplus::GraphicsPath sheenPath;
    if(round_path(sheenPath,sheen,radius)){
        Gdiplus::LinearGradientBrush gloss(
            Gdiplus::RectF(float(sheen.left),float(sheen.top),
                float(sheen.right-sheen.left),float(sheen.bottom-sheen.top)),
            color(highlight,36),color(highlight,0),90.f,TRUE);
        graphics.SetClip(&path,Gdiplus::CombineModeIntersect);
        graphics.FillPath(&gloss,&sheenPath);
        graphics.ResetClip();
    }
    Gdiplus::Pen edge(color(border,220),1.0f);
    graphics.DrawPath(&edge,&path);
}

inline void shadow(HDC dc,RECT bounds,int radius,UINT dpi,bool dark,int spreadLimit=5){
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath inside;
    if(!round_path(inside,bounds,radius))return;
    graphics.SetClip(&inside,Gdiplus::CombineModeExclude);
    OffsetRect(&bounds,0,px(spreadLimit==2?1:3,dpi));
    for(int spread=spreadLimit;spread>0;spread--){
        RECT layer=bounds;
        InflateRect(&layer,px(spread,dpi),px(spread,dpi));
        Gdiplus::GraphicsPath path;
        if(!round_path(path,layer,radius+px(spread,dpi)))continue;
        Gdiplus::SolidBrush brush(color(RGB(34,25,58),dark?22:5));
        graphics.FillPath(&brush,&path);
    }
}

inline void backdrop_glow(HDC dc,const RECT& bounds,int radius,COLORREF tint){
    Gdiplus::GraphicsPath shell;
    if(!round_path(shell,bounds,radius))return;
    float width=float(bounds.right-bounds.left),height=float(bounds.bottom-bounds.top);
    Gdiplus::Graphics graphics(dc);
    graphics.SetClip(&shell,Gdiplus::CombineModeIntersect);
    auto reflect=[&](float x,float y,float w,float h,BYTE alpha){
        Gdiplus::GraphicsPath shape;
        shape.AddEllipse(bounds.left+width*x,bounds.top+height*y,width*w,height*h);
        Gdiplus::PathGradientBrush glow(&shape);
        glow.SetCenterColor(color(tint,alpha));
        auto outside=color(tint,0);
        INT count=1;
        glow.SetSurroundColors(&outside,&count);
        graphics.FillPath(&glow,&shape);
    };
    reflect(-.36f,-.46f,1.16f,1.36f,125);
    reflect(.22f,.06f,1.42f,1.72f,185);
}

inline void accent_edge(HDC dc,const RECT& bounds,UINT dpi,COLORREF accent){
    const float thickness=float(px(2,dpi)),radius=float(px(7,dpi));
    float x=bounds.left+thickness/2,y=bounds.top+thickness/2,bottom=bounds.bottom-thickness/2;
    float end=x+(bounds.right-bounds.left)*.2f;
    if(end<=x+radius||bottom-y<=radius*2)return;
    Gdiplus::GraphicsPath edge;
    edge.AddLine(end,y,x+radius,y);
    edge.AddArc(x,y,radius*2,radius*2,270,-90);
    edge.AddLine(x,y+radius,x,bottom-radius);
    edge.AddArc(x,bottom-radius*2,radius*2,radius*2,180,-90);
    edge.AddLine(x+radius,bottom,end,bottom);
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(color(accent),thickness);
    graphics.DrawPath(&pen,&edge);
}

inline void label(HDC dc,RECT bounds,const wchar_t* title,COLORREF ink,UINT alignment=DT_CENTER){
    if(IsRectEmpty(&bounds))return;
    auto prior=SetTextColor(dc,ink);
    auto mode=SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,title,-1,&bounds,alignment|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    SetBkMode(dc,mode);
    SetTextColor(dc,prior);
}

inline void icon(HDC dc,RECT bounds,Icon kind,COLORREF ink,UINT dpi,bool compact=false,bool up=false){
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.TranslateTransform((bounds.left+bounds.right)*.5f,(bounds.top+bounds.bottom)*.5f);
    float scale=dpi/96.f;
    graphics.ScaleTransform(scale,scale);
    Gdiplus::Pen pen(color(ink),compact?1.5f:1.35f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    auto line=[&](float x,float y,float right,float bottom){graphics.DrawLine(&pen,x,y,right,bottom);};
    auto circle=[&](float r){graphics.DrawEllipse(&pen,-r,-r,r*2,r*2);};
    if(kind==Icon::Close){line(-5.5,-5.5,5.5,5.5);line(-5.5,5.5,5.5,-5.5);}
    else if(kind==Icon::Chevron){
        float direction=up?-1.f:1.f;
        line(-3,-1.5f*direction,0,1.5f*direction);line(0,1.5f*direction,3,-1.5f*direction);
    }
    else if(kind==Icon::Plus){line(-3.5,0,3.5,0);line(0,-3.5,0,3.5);}
    else if(kind==Icon::Frame){
        float edge=compact?4.25f:6.f,inner=compact?1.75f:2.5f;
        for(float x:{-1.f,1.f})for(float y:{-1.f,1.f}){
            line(x*edge,y*edge,x*inner,y*edge);line(x*edge,y*edge,x*edge,y*inner);
        }
    }
    else if(kind==Icon::Application){
        line(-2,-5,6,-5);line(6,-5,6,2);
        graphics.DrawRectangle(&pen,-6.f,-2.f,9.f,7.f);
    }
    else if(kind==Icon::Window){
        graphics.DrawRectangle(&pen,-6.f,-4.5f,12.f,9.f);
        line(-6,-1.5,6,-1.5);
    }
    else if(kind==Icon::Archive){
        graphics.DrawRectangle(&pen,-6.f,-4.5f,12.f,3.f);
        graphics.DrawRectangle(&pen,-5.f,-1.5f,10.f,6.f);
        line(-1.5f,1,1.5f,1);
    }
    else if(kind==Icon::More){
        Gdiplus::SolidBrush dot(color(ink));
        for(float x:{-4.f,0.f,4.f})graphics.FillEllipse(&dot,x-1.1f,-1.1f,2.2f,2.2f);
    }
    else if(kind==Icon::Trash){
        line(-2,-4.25f,2,-4.25f);line(-4.25f,-2,4.25f,-2);
        line(-3.25f,0,-3.25f,4.25f);line(3.25f,0,3.25f,4.25f);
        line(-3.25f,4.25f,3.25f,4.25f);
    }
    else if(kind==Icon::System){
        Gdiplus::GraphicsPath outline;
        outline.AddArc(-6.8f,-5.2f,3.f,3.f,180.f,90.f);
        outline.AddArc(3.8f,-5.2f,3.f,3.f,270.f,90.f);
        outline.AddArc(3.8f,2.2f,3.f,3.f,0.f,90.f);
        outline.AddArc(-6.8f,2.2f,3.f,3.f,90.f,90.f);
        outline.CloseFigure();
        graphics.DrawPath(&pen,&outline);
        line(-3,7,3,7);
    }else if(kind==Icon::Sun){
        circle(3.4);
        for(int i=0;i<8;i++){double angle=i*3.14159265/4;line(5*std::cos(angle),5*std::sin(angle),7*std::cos(angle),7*std::sin(angle));}
    }else if(kind==Icon::Moon){
        const Gdiplus::PointF moon[]={{-1,-7},{-9,-6},{-9,5},{-2,7},{2,8},{6,5},{7,1},{1,4},{-4,-1},{-1,-7}};
        graphics.DrawBeziers(&pen,moon,static_cast<INT>(std::size(moon)));
    }else if(kind==Icon::Gear){
        Gdiplus::PointF ring[41];
        for(int i=0;i<40;i++){
            double angle=(i/5+(i%5)*.2)*3.14159265/4;
            double radius=i%5==1||i%5==2||i%5==3?7.1:5.5;
            ring[i]={float(std::cos(angle)*radius),float(std::sin(angle)*radius)};
        }
        ring[40]=ring[0];graphics.DrawLines(&pen,ring,41);circle(2.6f);
    }
}
}
