#pragma once
#include "core.hpp"
#include <wincrypt.h>
#include <map>
#include <functional>
#include <set>

inline bool visual_hex(const json& value,size_t length){
    if(!value.is_string())return false;const auto s=value.get<std::string>();
    return s.size()==length&&s.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;
}
inline double visual_number(const json& value,double low,double high){
    if(!value.is_number()||value.is_boolean())throw std::runtime_error("invalid_visual_number");
    double n=value.get<double>();if(!std::isfinite(n)||n<low||n>high)throw std::runtime_error("invalid_visual_number");return n;
}
inline void visual_array(const json& values,size_t count,double low,double high){
    if(!values.is_array()||values.size()!=count)throw std::runtime_error("invalid_visual_array");
    for(const auto& value:values)visual_number(value,low,high);
}
inline void visual_keys(const json& value,std::initializer_list<const char*> keys){
    if(!value.is_object()||value.size()!=keys.size())throw std::runtime_error("invalid_visual_fields");
    for(auto key:keys)if(!value.contains(key))throw std::runtime_error("invalid_visual_fields");
}
inline void validate_visual_relation(const json& relation){
    visual_keys(relation,{"mode","anchor_x","anchor_y","offset_dip","fixed_size_dip","margins_dip","padding_physical"});
    if(relation["mode"]!="position_only"&&relation["mode"]!="follow_bounds")throw std::runtime_error("invalid_visual_relation");
    if(relation["anchor_x"]!="left"&&relation["anchor_x"]!="center"&&relation["anchor_x"]!="right")throw std::runtime_error("invalid_visual_anchor");
    if(relation["anchor_y"]!="top"&&relation["anchor_y"]!="center"&&relation["anchor_y"]!="bottom")throw std::runtime_error("invalid_visual_anchor");
    visual_array(relation["offset_dip"],2,-100000,100000);
    visual_array(relation["fixed_size_dip"],2,8,100000);
    visual_array(relation["margins_dip"],4,-4096,4096);
    if(!relation["padding_physical"].is_number_integer())throw std::runtime_error("invalid_visual_padding");
    visual_number(relation["padding_physical"],0,2);
}
inline void validate_visual_binding(const json& binding){
    if(binding.is_null())return;
    auto fields=binding;fields.erase("tracking_kind");fields.erase("search_limit_dip");
    visual_keys(fields,{"version","calibration_id","reference_rect_dip","calibration_dpi","template_id","template_sha256","edge_limit_dip","size_ratio","relation"});
    std::string kind=binding.value("tracking_kind",std::string("boundary"));
    if(kind!="boundary"&&kind!="features")throw std::runtime_error("invalid_tracking_kind");
    if(binding.contains("search_limit_dip"))visual_number(binding["search_limit_dip"],32,500);
    if(kind=="features"&&binding.at("relation").at("mode")!="position_only")throw std::runtime_error("feature_position_requires_fixed_size");
    if(!binding["version"].is_number_integer()||binding["version"]!=1||
       !visual_hex(binding["calibration_id"],32)||!visual_hex(binding["template_id"],32)||!visual_hex(binding["template_sha256"],64))
        throw std::runtime_error("invalid_visual_identity");
    visual_array(binding["reference_rect_dip"],4,0,100000);
    if(binding["reference_rect_dip"][2]<=0||binding["reference_rect_dip"][3]<=0)throw std::runtime_error("invalid_visual_reference");
    visual_number(binding["calibration_dpi"],48,768);
    if(visual_number(binding["edge_limit_dip"],32,32)!=32||visual_number(binding["size_ratio"],1.25,1.25)!=1.25)
        throw std::runtime_error("invalid_visual_limits");
    validate_visual_relation(binding["relation"]);
}
inline std::string visual_sha256(const std::vector<BYTE>& bytes){
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))throw std::runtime_error("visual_hash_unavailable");
    try{
        if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)||!CryptHashData(hash,bytes.data(),DWORD(bytes.size()),0))throw std::runtime_error("visual_hash_failed");
        BYTE digest[32];DWORD length=sizeof(digest);
        if(!CryptGetHashParam(hash,HP_HASHVAL,digest,&length,0)||length!=32)throw std::runtime_error("visual_hash_failed");
        const char* digits="0123456789abcdef";std::string result;
        for(BYTE b:digest){result+=digits[b>>4];result+=digits[b&15];}
        CryptDestroyHash(hash);CryptReleaseContext(provider,0);return result;
    }catch(...){if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);throw;}
}
inline std::vector<BYTE> visual_template(unsigned width,unsigned height,const std::vector<BYTE>& pixels){
    if(!width||!height||uint64_t(width)*height>131072||pixels.size()!=uint64_t(width)*height)throw std::runtime_error("invalid_visual_template");
    std::vector<BYTE> result={'V','T','P','1'};
    for(unsigned n:{width,height})for(int i=0;i<4;i++)result.push_back(BYTE(n>>(i*8)));
    result.insert(result.end(),pixels.begin(),pixels.end());return result;
}
inline void visual_atomic_write(const fs::path& path,const std::vector<BYTE>& bytes,const std::string& nonce,bool replace=false){
    fs::create_directories(path.parent_path());fs::path temporary=path;temporary+=L".tmp."+wide(nonce);
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("visual_temporary_failed");
    try{
        size_t offset=0;
        while(offset<bytes.size()){
            DWORD written=0,chunk=DWORD(std::min<size_t>(bytes.size()-offset,1048576));
            if(!WriteFile(file,bytes.data()+offset,chunk,&written,nullptr)||!written)throw std::runtime_error("visual_write_failed");
            offset+=written;
        }
        if(!FlushFileBuffers(file))throw std::runtime_error("visual_flush_failed");
        CloseHandle(file);file=INVALID_HANDLE_VALUE;
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0)))throw std::runtime_error("visual_replace_failed");
    }catch(...){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);DeleteFileW(temporary.c_str());throw;}
}
inline bool visual_asset_valid(const fs::path& config,const json& binding){
    if(binding.is_null())return false;
    try{
        validate_visual_binding(binding);
        fs::path file=config.parent_path()/L"visual-templates"/(wide(binding["template_id"].get<std::string>())+L".vt1");
        // Do not follow a replaced template directory or file outside the config directory.
        DWORD attributes=GetFileAttributesW(file.c_str()),parentAttributes=GetFileAttributesW(file.parent_path().c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||parentAttributes==INVALID_FILE_ATTRIBUTES||
           (attributes&FILE_ATTRIBUTE_REPARSE_POINT)||(parentAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        auto size=fs::file_size(file);if(size<13||size>131084)return false;
        std::ifstream stream(file,std::ios::binary);std::vector<BYTE> bytes(size);
        if(!stream.read(reinterpret_cast<char*>(bytes.data()),size)||std::string(bytes.begin(),bytes.begin()+4)!="VTP1")return false;
        unsigned w=0,h=0;for(int i=0;i<4;i++){w|=unsigned(bytes[4+i])<<(8*i);h|=unsigned(bytes[8+i])<<(8*i);}
        return w&&h&&uint64_t(w)*h<=131072&&uint64_t(w)*h+12==size&&visual_sha256(bytes)==binding["template_sha256"];
    }catch(...){return false;}
}
inline double visual_now(){
    LARGE_INTEGER time,frequency;QueryPerformanceCounter(&time);QueryPerformanceFrequency(&frequency);
    return double(time.QuadPart)/frequency.QuadPart;
}
struct VisualRect {double x=0,y=0,width=0,height=0;};
inline json visual_rect_json(const VisualRect& r){return {{"x",r.x},{"y",r.y},{"width",r.width},{"height",r.height}};}
struct VisualPosition {VisualRect nominal,cover;double expires=0,transportHold=0;uint64_t generation=0;std::string calibration;};
inline VisualRect visual_nominal(const VisualRect& boundary,const json& relation){
    if(relation["mode"]=="position_only"){
        double ax=relation["anchor_x"]=="right"?1:relation["anchor_x"]=="center"?.5:0;
        double ay=relation["anchor_y"]=="bottom"?1:relation["anchor_y"]=="center"?.5:0;
        return {boundary.x+boundary.width*ax+relation["offset_dip"][0].get<double>(),
            boundary.y+boundary.height*ay+relation["offset_dip"][1].get<double>(),
            relation["fixed_size_dip"][0],relation["fixed_size_dip"][1]};
    }
    const auto& m=relation["margins_dip"];double l=m[0],t=m[1],r=m[2],b=m[3];
    return {boundary.x-l,boundary.y-t,boundary.width+l+r,boundary.height+t+b};
}
inline json visual_relation_at_cover(json relation,const VisualRect& reference,const VisualRect& cover){
    if(relation["mode"]=="position_only"){
        double ax=relation["anchor_x"]=="right"?1:relation["anchor_x"]=="center"?.5:0;
        double ay=relation["anchor_y"]=="bottom"?1:relation["anchor_y"]=="center"?.5:0;
        relation["offset_dip"]={cover.x-reference.x-reference.width*ax,cover.y-reference.y-reference.height*ay};
        relation["fixed_size_dip"]={cover.width,cover.height};
    }else relation["margins_dip"]={reference.x-cover.x,reference.y-cover.y,
        cover.x+cover.width-reference.x-reference.width,cover.y+cover.height-reference.y-reference.height};
    validate_visual_relation(relation);return relation;
}
inline VisualPosition visual_geometry(const VisualRect& boundary,const json& relation,double width,double height,double dpi){
    auto nominal=visual_nominal(boundary,relation);
    double right=std::min(width,nominal.x+nominal.width),bottom=std::min(height,nominal.y+nominal.height);
    nominal.x=std::max(0.,nominal.x);nominal.y=std::max(0.,nominal.y);nominal.width=right-nominal.x;nominal.height=bottom-nominal.y;
    if(nominal.width<=0||nominal.height<=0)return {};
    double scale=dpi/96.,pad=relation["mode"]=="follow_bounds"?relation["padding_physical"].get<double>():0;
    double left=std::max(0.,std::floor(nominal.x*scale)-pad),top=std::max(0.,std::floor(nominal.y*scale)-pad);
    right=std::min(std::round(width*scale),std::ceil(right*scale)+pad);
    bottom=std::min(std::round(height*scale),std::ceil(bottom*scale)+pad);
    return {nominal,{left/scale,top/scale,(right-left)/scale,(bottom-top)/scale}};
}
