#pragma once
#include "visual_config.hpp"

// Tiles partition candidate top-left coordinates; their images overlap by the
// template size so a match crossing a tile edge is never cut in half.
inline int feature_scaled_size(unsigned pixels,double ratio){
    double n=pixels*ratio,f=std::floor(n),part=n-f;
    // Match Python round(), used by the worker when resizing a DPI template.
    return std::max(1,int(part>.5?f+1:part<.5?f:std::fmod(f,2)==0?f:f+1));
}
inline std::vector<RECT> feature_search_tiles(const VisualRect& origin,double radius,double scale,int width,int height,int templateWidth=0,int templateHeight=0){
    int tw=templateWidth?templateWidth:std::max(1,int(std::round(origin.width*scale)));
    int th=templateHeight?templateHeight:std::max(1,int(std::round(origin.height*scale)));
    if(tw>width||th>height)return {};
    int left=std::max(0,int(std::floor((origin.x-radius)*scale)));
    int top=std::max(0,int(std::floor((origin.y-radius)*scale)));
    int right=std::min(width-tw,int(std::ceil((origin.x+radius)*scale)));
    int bottom=std::min(height-th,int(std::ceil((origin.y+radius)*scale)));
    if(left>right||top>bottom)return {};
    int fullW=right-left+tw,fullH=bottom-top+th;
    int tileW=std::min(fullW,std::max(724,tw*2));
    int tileH=std::min(fullH,524288/tileW);
    if(tileH<th){tileH=std::min(fullH,std::max(724,th*2));tileW=std::min(fullW,524288/tileH);}
    if(tileW<tw||tileH<th)return {};
    int stepX=tileW-tw+1,stepY=tileH-th+1;
    std::vector<RECT> tiles;
    for(int y=top;y<=bottom;y+=stepY)for(int x=left;x<=right;x+=stepX){
        if(tiles.size()>=512)return {};
        tiles.push_back(RECT{x,y,std::min(right+tw,x+tileW),std::min(bottom+th,y+tileH)});
    }
    return tiles;
}

struct FeatureSearch {
    bool scanning=true,confirming=false,found=false,ambiguous=false;
    unsigned level=0;size_t at=0;
    double retryAt=0,started=0;
    VisualRect candidate{};
    std::vector<RECT> tiles;
    void reset(){*this=FeatureSearch{};}
    double radius(double limit)const{return level==0?32:level==1?std::min(128.,limit):limit;}
    void next_level(){level++;at=0;tiles.clear();found=ambiguous=false;started=0;}
    void retry(double now){reset();retryAt=now+2;}
};
