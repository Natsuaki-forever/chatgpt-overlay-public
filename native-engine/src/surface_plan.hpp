#pragma once
#include "core.hpp"

struct SurfaceGroup {
    std::vector<size_t> indices;
    RECT bounds{};
    bool operator==(const SurfaceGroup& other)const{
        return indices==other.indices&&EqualRect(&bounds,&other.bounds);
    }
};
inline std::vector<SurfaceGroup> plan_rect_groups(const std::vector<RECT>& rectangles,bool separate=false){
    std::vector<SurfaceGroup> groups;
    for(size_t i=0;i<rectangles.size();i++)
        if(!IsRectEmpty(&rectangles[i]))groups.push_back({{i},rectangles[i]});
    auto area=[](RECT r){return int64_t(r.right-r.left)*(r.bottom-r.top);};
    while(!separate&&groups.size()>1){
        int bestI=-1,bestJ=-1;int64_t best=INT64_MAX;RECT combined{};
        for(size_t i=0;i<groups.size();i++)for(size_t j=i+1;j<groups.size();j++){
            RECT united{},overlap{};UnionRect(&united,&groups[i].bounds,&groups[j].bounds);
            auto overhead=area(united)-area(groups[i].bounds)-area(groups[j].bounds);
            if((IntersectRect(&overlap,&groups[i].bounds,&groups[j].bounds)||overhead<=32768||groups.size()>8)&&overhead<best){
                best=overhead;bestI=int(i);bestJ=int(j);combined=united;
            }
        }
        if(bestI<0)break;
        auto indices=groups[bestI].indices;indices.insert(indices.end(),groups[bestJ].indices.begin(),groups[bestJ].indices.end());
        std::sort(indices.begin(),indices.end());
        groups.erase(groups.begin()+bestJ);groups.erase(groups.begin()+bestI);groups.push_back({indices,combined});
    }
    std::sort(groups.begin(),groups.end(),[](const auto& a,const auto& b){return a.indices.front()<b.indices.front();});return groups;
}
