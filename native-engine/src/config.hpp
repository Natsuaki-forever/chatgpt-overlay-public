#pragma once
#include "core.hpp"
#include "visual_config.hpp"
#include "visual_asset_retirement.hpp"

inline std::string new_id(){
    GUID guid{};check(CoCreateGuid(&guid));wchar_t text[40];StringFromGUID2(guid,text,40);
    std::string result;for(wchar_t ch:std::wstring(text))if(iswxdigit(ch))result+=char(towlower(ch));
    return result;
}

inline const json& region_defaults(){
    static const json defaults={{"x",20},{"y",20},{"width",160},{"height",40},{"text","我的工作台"},
        {"anchor_x","left"},{"anchor_y","top"},{"reference_width",0},{"reference_height",0},
        {"position_mode","anchor"},{"visual_binding",nullptr},
        {"font","Segoe UI"},{"size",14},{"bold",false},{"color","#ececec"},{"background","#212121"},
        {"opacity",255},{"align","left"},{"padding",8},{"letter_spacing",0},{"scroll",false},
        {"scroll_direction","left"},{"scroll_speed",40},{"scroll_gap",40},{"scroll_feather",false},
        {"scroll_feather_width",24},{"outline",false},{"outline_width",1.5},{"outline_color","#000000"},
        {"glow",false},{"glow_color","#2ab89d"},{"glow_radius",10},{"glow_strength",75},
        {"glow_pulse",true},{"glow_period",2.0}};
    return defaults;
}
inline json canonical_region(const json& input){
    if(!input.is_object())throw std::runtime_error("Region must be an object");
    json result=input;
    const auto& defaults=region_defaults();
    for(auto it=defaults.begin();it!=defaults.end();++it)if(!result.contains(it.key()))result[it.key()]=it.value();
    if(!result.contains("id"))result["id"]=new_id();
    std::string id=result.at("id").get<std::string>();
    if(id.size()!=32||id.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)throw std::runtime_error("Invalid region ID");
    for(auto key:{"x","y","width","height"})result[key]=number(result,key,0,(key[0]=='x'||key[0]=='y')?0:8,100000);
    for(auto key:{"reference_width","reference_height"})result[key]=number(result,key,0,0,100000);
    for(auto key:{"size","opacity","padding","scroll_speed","scroll_gap","scroll_feather_width","glow_radius","glow_strength"}){
        double low=0,high=100;
        std::string name=key;
        if(name=="size"){low=6;high=200;}else if(name=="opacity")high=255;
        else if(name=="scroll_speed"){low=5;high=300;}else if(name=="scroll_gap"){low=8;high=300;}
        else if(name=="scroll_feather_width"){low=1;high=200;}else if(name=="glow_radius"){low=1;high=24;}
        result[key]=int(number(result,key,0,low,high));
    }
    result["letter_spacing"]=number(result,"letter_spacing",0,-10,50);
    result["outline_width"]=number(result,"outline_width",1.5,.5,12);
    result["glow_period"]=number(result,"glow_period",2,.5,10);
    for(auto key:{"bold","scroll","scroll_feather","outline","glow","glow_pulse"})
        if(!result[key].is_boolean())throw std::runtime_error("Region flag must be boolean");
    for(auto key:{"text","font","color","background","outline_color","glow_color","align","scroll_direction","anchor_x","anchor_y"})
        wide(result[key].get<std::string>());
    if(result["anchor_x"]!="left"&&result["anchor_x"]!="center"&&result["anchor_x"]!="right")result["anchor_x"]="left";
    if(result["anchor_y"]!="top"&&result["anchor_y"]!="center"&&result["anchor_y"]!="bottom")result["anchor_y"]="top";
    if(result["align"]!="left"&&result["align"]!="center"&&result["align"]!="right")result["align"]="left";
    if(result["scroll_direction"]!="left"&&result["scroll_direction"]!="right")result["scroll_direction"]="left";
    Region validate(result);
    if(result["position_mode"]!="anchor"&&result["position_mode"]!="visual")throw std::runtime_error("invalid_position_mode");
    validate_visual_binding(result["visual_binding"]);
    if(result["position_mode"]=="visual"&&result["visual_binding"].is_null())throw std::runtime_error("visual_calibration_required");
    return result;
}

struct ConfigState {
    fs::path path;
    bool persistent=true;
    fs::path otherApplicationDirectory;
    json document;
    json hiddenRegions=json::array();
    std::string scopeWindow;
    json persistedVisualAssets=json::object();
    int64_t lastAssetMaintenance=0;
    uint64_t revision=0,persistedRevision=0,stateSequence=0;
    std::string selected;
    float viewportWidth=0,viewportHeight=0;
    float viewportDpi=96;
    uint64_t visualGeneration=0;
    uint64_t bindingEpoch=0;
    std::string applicationId;
    HMONITOR targetMonitor=nullptr;
    uint64_t translationSerial=0;
    double translatedAt=0;
    json geometryContext;
    std::map<std::string,VisualPosition> visualPositions;
    bool stableVisualPresentation=false;
    std::map<std::string,std::pair<std::string,VisualRect>> confirmedBoundaries;
    std::map<std::string,std::pair<std::string,VisualRect>> featureOrigins;
    std::set<std::string> unavailableVisualAssets;
    std::map<std::string,json> visualRuntimeStates;
    std::function<void()> runtimeChanged;
    explicit ConfigState(const fs::path& file,bool persist=true,const std::string& scope={}):
        path(fs::absolute(file)),persistent(persist),scopeWindow(scope){
        bool exists=persistent&&fs::exists(path);
        if(exists){std::ifstream stream(path);if(!stream)throw std::runtime_error("Cannot read configuration");stream>>document;}
        else document={{"version",1},{"regions",json::array()}};
        if(!document.is_object()||!document.contains("version")||!document["version"].is_number_integer()||
           (document["version"]!=1&&document["version"]!=2)||!document.contains("regions")||!document["regions"].is_array())
            throw std::runtime_error("Unsupported configuration");
        json original=document;
        if(document.contains("visual_check_interval_seconds")&&!document["visual_check_interval_seconds"].is_number_integer())throw std::runtime_error("invalid_visual_check_interval");
        int interval=document.value("visual_check_interval_seconds",15);
        if(interval!=0&&interval!=15&&interval!=30&&interval!=60)throw std::runtime_error("invalid_visual_check_interval");
        std::vector<std::string> ids;
        for(auto& entry:document["regions"]){
            entry=canonical_region(entry);std::string id=entry["id"];
            if(document["version"]==1&&(entry["position_mode"]!="anchor"||!entry["visual_binding"].is_null()))throw std::runtime_error("visual_requires_config_v2");
            if(std::find(ids.begin(),ids.end(),id)!=ids.end())throw std::runtime_error("Duplicate region ID");
            ids.push_back(id);
            if(entry["position_mode"]=="visual"&&!visual_asset_valid(path,entry["visual_binding"]))unavailableVisualAssets.insert(id);
        }
        if(!exists||original!=document)revision=1;
        persistedVisualAssets=visual_asset_references(document);
        if(!scopeWindow.empty()){
            auto& regions=document["regions"];
            for(auto it=regions.begin();it!=regions.end();){
                auto binding=it->value("window_profile",std::string());
                if(binding.empty()||binding==scopeWindow){++it;continue;}
                hiddenRegions.push_back(std::move(*it));
                it=regions.erase(it);
            }
        }
        if(!persistent)persistedRevision=revision;
    }
    void set_region_scope(const std::string& id,const std::string& profile){
        int i=index(id);if(i<0)throw std::runtime_error("region_not_found");
        auto& regions=document["regions"];
        auto& region=regions[i];
        if(region.value("window_profile",std::string())==profile)return;
        if(profile.empty())region.erase("window_profile");
        else region["window_profile"]=profile;
        if(!profile.empty()&&profile!=scopeWindow){
            hiddenRegions.push_back(std::move(region));
            regions.erase(regions.begin()+i);
            if(selected==id)selected.clear();
            unavailableVisualAssets.erase(id);
            invalidate_visual();
        }
        revision++;stateSequence++;
    }
    bool remove_region_scope(const std::string& profile){
        bool removed=false;
        for(auto it=hiddenRegions.begin();it!=hiddenRegions.end();){
            if(it->value("window_profile",std::string())==profile){it=hiddenRegions.erase(it);removed=true;}
            else ++it;
        }
        if(removed){revision++;stateSequence++;}
        return removed;
    }
    int index(const std::string& id)const{
        const auto& entries=document.at("regions");
        for(size_t i=0;i<entries.size();i++)if(entries[i].at("id")==id)return int(i);
        return -1;
    }
    void expect(uint64_t base)const{if(base!=revision)throw std::runtime_error("revision_conflict");}
    int visual_check_interval()const{return document.value("visual_check_interval_seconds",15);}
    void set_visual_check_interval(int seconds){
        if(seconds!=0&&seconds!=15&&seconds!=30&&seconds!=60)throw std::runtime_error("invalid_visual_check_interval");
        if(seconds!=visual_check_interval()){document["visual_check_interval_seconds"]=seconds;revision++;stateSequence++;}
    }
    void invalidate_visual(){visualPositions.clear();translatedAt=0;visualGeneration++;stateSequence++;}
    void set_geometry_context(const json& context,float dpi=96){
        bool translation=context.is_array()&&geometryContext.is_array()&&context.size()==7&&geometryContext.size()==7&&
            context!=geometryContext&&dpi==viewportDpi&&context[0]==geometryContext[0]&&context[1]==geometryContext[1]&&
            context[6]==true&&geometryContext[6]==true&&
            context[4].get<double>()-context[2].get<double>()==geometryContext[4].get<double>()-geometryContext[2].get<double>()&&
            context[5].get<double>()-context[3].get<double>()==geometryContext[5].get<double>()-geometryContext[3].get<double>();
        if(translation){
            geometryContext=context;translatedAt=visual_now();translationSerial++;stateSequence++;
            for(auto& entry:visualPositions)if(std::max(entry.second.expires,entry.second.transportHold)>translatedAt)
                entry.second.transportHold=translatedAt+.8;
            return;
        }
        if(context.is_array()&&geometryContext.is_array()&&context.size()>=2&&geometryContext.size()>=2&&
           (context[0]!=geometryContext[0]||context[1]!=geometryContext[1])){featureOrigins.clear();confirmedBoundaries.clear();}
        if(context!=geometryContext||dpi!=viewportDpi){geometryContext=context;viewportDpi=dpi;invalidate_visual();}
    }
    bool set_viewport(float width,float height){
        if(width==viewportWidth&&height==viewportHeight)return false;
        viewportWidth=width;viewportHeight=height;invalidate_visual();return true;
    }
    void set_target_geometry(HWND target){
        bool valid=IsWindow(target)!=FALSE;RECT box=valid?client_box(target):RECT{};
        float dpi=valid?float(GetDpiForWindow(target)):96;
        auto monitor=valid?MonitorFromWindow(target,MONITOR_DEFAULTTONEAREST):nullptr;
        if(targetMonitor&&monitor!=targetMonitor){
            geometryContext=nullptr;confirmedBoundaries.clear();
        }
        targetMonitor=monitor;
        set_geometry_context(json::array({reinterpret_cast<uintptr_t>(target),process_id(target),
            box.left,box.top,box.right,box.bottom,available(target)}),dpi);
        if(valid&&dpi>0)set_viewport((box.right-box.left)*96/dpi,(box.bottom-box.top)*96/dpi);
    }
    Region positioned(int i)const{
        Region region(document["regions"][i]);region.locate(viewportWidth,viewportHeight);
        if(region.visual){
            auto id=document["regions"][i]["id"].get<std::string>();auto found=visualPositions.find(id);
            auto retained=confirmedBoundaries.find(id);
            if(stableVisualPresentation&&!unavailableVisualAssets.count(id)&&retained!=confirmedBoundaries.end()&&
               retained->second.first==document["regions"][i]["visual_binding"]["calibration_id"]){
                auto pose=visual_geometry(retained->second.second,document["regions"][i]["visual_binding"]["relation"],viewportWidth,viewportHeight,viewportDpi);
                if(pose.cover.width>0&&pose.cover.height>0){
                    region.x=float(pose.cover.x);region.y=float(pose.cover.y);region.width=float(pose.cover.width);region.height=float(pose.cover.height);region.drawable=true;
                }
                return region;
            }
            if(!unavailableVisualAssets.count(id)&&found!=visualPositions.end()&&found->second.generation==visualGeneration&&std::max(found->second.expires,found->second.transportHold)>visual_now()&&
               found->second.calibration==document["regions"][i]["visual_binding"]["calibration_id"]){
                const auto& r=found->second.cover;region.x=float(r.x);region.y=float(r.y);
                region.width=float(r.width);region.height=float(r.height);region.drawable=true;
            }
        }
        return region;
    }
    json effective_rects()const{
        json result=json::object();for(size_t i=0;i<document["regions"].size();i++){
            Region r=positioned(int(i));result[document["regions"][i]["id"].get<std::string>()]=r.drawable?
                json{{"x",r.x},{"y",r.y},{"width",r.width},{"height",r.height}}:json(nullptr);
        }return result;
    }
    json tracking_states()const{
        json result=json::object();for(size_t i=0;i<document["regions"].size();i++){
            const auto& entry=document["regions"][i];if(entry["position_mode"]!="visual")continue;
            auto id=entry["id"].get<std::string>();
            result[id]={{"state",unavailableVisualAssets.count(id)?"UNAVAILABLE":positioned(int(i)).drawable?"FOLLOWING":"VERIFYING"},
                {"reason",unavailableVisualAssets.count(id)?"template_unavailable":""},{"generation",visualGeneration}};
            if(!unavailableVisualAssets.count(id)&&visualRuntimeStates.count(id))result[id]=visualRuntimeStates.at(id);
        }return result;
    }
    json effective_positions()const{
        json result=json::object();
        for(size_t i=0;i<document["regions"].size();i++){
            const auto& entry=document["regions"][i];
            if(entry["position_mode"]=="visual")continue;
            if(entry["anchor_x"]=="left"&&entry["anchor_y"]=="top")continue;
            Region region=positioned(int(i));result[document["regions"][i]["id"].get<std::string>()]={{"x",region.x},{"y",region.y}};
        }
        return result;
    }
    json positioned_patch(const json& source,const json& patch)const{
        if(!patch.is_object())throw std::runtime_error("Patch must be an object");
        Region current(canonical_region(source));current.locate(viewportWidth,viewportHeight);
        if(current.visual)for(auto key:{"x","y","width","height","anchor_x","anchor_y","reference_width","reference_height"})
            if(patch.contains(key))throw std::runtime_error("visual_geometry_read_only");
        json result=patch;
        if(patch.contains("x")||(patch.contains("anchor_x")&&patch["anchor_x"]!=current.anchorX)){
            if(viewportWidth<=0)throw std::runtime_error("target_geometry_unavailable");
            result["x"]=patch.value("x",double(current.x));result["reference_width"]=viewportWidth;
        }
        if(patch.contains("y")||(patch.contains("anchor_y")&&patch["anchor_y"]!=current.anchorY)){
            if(viewportHeight<=0)throw std::runtime_error("target_geometry_unavailable");
            result["y"]=patch.value("y",double(current.y));result["reference_height"]=viewportHeight;
        }
        return result;
    }
    void update_positioned(const std::string& id,const json& patch,uint64_t base){
        expect(base);int i=index(id);if(i<0)throw std::runtime_error("region_not_found");
        update(id,positioned_patch(document["regions"][i],patch),base);
    }
    void update(const std::string& id,const json& patch,uint64_t base){
        expect(base);int i=index(id);if(i<0)throw std::runtime_error("region_not_found");
        if(!patch.is_object())throw std::runtime_error("Patch must be an object");
        json merged=document["regions"][i];
        for(auto it=patch.begin();it!=patch.end();++it){
            if(!region_defaults().contains(it.key()))throw std::runtime_error("Invalid editable field");
            if(it.key()=="position_mode"||it.key()=="visual_binding")throw std::runtime_error("visual_operation_required");
            if(merged["position_mode"]=="visual"&&(it.key()=="x"||it.key()=="y"||it.key()=="width"||it.key()=="height"||it.key()=="anchor_x"||it.key()=="anchor_y"||it.key()=="reference_width"||it.key()=="reference_height"))throw std::runtime_error("visual_geometry_read_only");
            merged[it.key()]=it.value();
        }
        merged=canonical_region(merged);
        if(merged!=document["regions"][i]){document["regions"][i]=std::move(merged);revision++;stateSequence++;}
    }
    std::string create(const json& properties,uint64_t base){
        expect(base);if(!properties.is_object())throw std::runtime_error("Region properties must be an object");
        for(auto it=properties.begin();it!=properties.end();++it)if(!region_defaults().contains(it.key()))throw std::runtime_error("Invalid editable field");
        json entry=canonical_region(properties);
        if(entry["position_mode"]!="anchor"||!entry["visual_binding"].is_null())throw std::runtime_error("visual_operation_required");
        selected=entry["id"];
        document["regions"].push_back(entry);revision++;stateSequence++;return selected;
    }
    void erase(const std::string& id,uint64_t base){
        expect(base);int i=index(id);if(i<0)throw std::runtime_error("region_not_found");
        document["regions"].erase(document["regions"].begin()+i);if(selected==id)selected.clear();revision++;stateSequence++;
        visualPositions.erase(id);featureOrigins.erase(id);confirmedBoundaries.erase(id);
        unavailableVisualAssets.erase(id);
    }
    void erase_all(uint64_t base){
        expect(base); if(document["regions"].empty()) return;
        document["regions"].clear(); selected.clear(); revision++; stateSequence++;
        visualPositions.clear(); featureOrigins.clear(); confirmedBoundaries.clear(); unavailableVisualAssets.clear();
    }
    // P2 internal transaction. No editor/worker IPC exposes this operation yet.
    void install_visual(const std::string& id,json reference,double dpi,json relation,unsigned width,unsigned height,
                        const std::vector<BYTE>& pixels,uint64_t base,const std::string& trackingKind="boundary",double searchLimit=500){
        expect(base);int i=index(id);if(i<0)throw std::runtime_error("region_not_found");
        auto bytes=visual_template(width,height,pixels);std::string templateId=new_id();
        json binding={{"version",1},{"calibration_id",new_id()},{"reference_rect_dip",reference},{"calibration_dpi",dpi},
            {"template_id",templateId},{"template_sha256",visual_sha256(bytes)},{"edge_limit_dip",32},{"size_ratio",1.25},{"relation",relation}};
        if(trackingKind!="boundary")binding["tracking_kind"]=trackingKind;
        if(trackingKind=="features")binding["search_limit_dip"]=searchLimit;
        validate_visual_binding(binding);
        json entry=document["regions"][i];entry["position_mode"]="visual";entry["visual_binding"]=binding;entry=canonical_region(entry);
        save(); // Make the previous, still usable state durable before migration.
        json candidate=document;
        if(candidate["version"]==1){
            auto size=fs::file_size(path);if(size>16777216)throw std::runtime_error("configuration_backup_too_large");
            std::vector<BYTE> original(size);std::ifstream stream(path,std::ios::binary);
            if(!stream.read(reinterpret_cast<char*>(original.data()),size))throw std::runtime_error("configuration_backup_failed");
            fs::path backup=path;backup+=L".pre-visual-v1."+wide(visual_sha256(original))+L".json";
            if(fs::exists(backup)){
                if(fs::file_size(backup)!=original.size())throw std::runtime_error("configuration_backup_mismatch");
                std::vector<BYTE> saved(original.size());std::ifstream prior(backup,std::ios::binary);
                if(!prior.read(reinterpret_cast<char*>(saved.data()),saved.size())||saved!=original)throw std::runtime_error("configuration_backup_mismatch");
            }else visual_atomic_write(backup,original,new_id());
            candidate["visual_migration_backup"]=utf8(backup.filename().wstring());
        }
        fs::path asset=path.parent_path()/L"visual-templates"/(wide(templateId)+L".vt1");
        fs::create_directories(asset.parent_path());
        DWORD attrs=GetFileAttributesW(asset.parent_path().c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("invalid_visual_asset_directory");
        visual_atomic_write(asset,bytes,new_id());
        json previous=document;auto previousRevision=revision,previousPersisted=persistedRevision;
        try{
            candidate["version"]=2;candidate["regions"][i]=entry;document=std::move(candidate);revision++;stateSequence++;
            save();unavailableVisualAssets.erase(id);invalidate_visual();
        }catch(...){
            document=std::move(previous);revision=previousRevision;persistedRevision=previousPersisted;stateSequence++;
            DeleteFileW(asset.c_str());throw;
        }
    }
    bool publish_visual_boundary(const std::string& id,const std::string& calibration,uint64_t generation,
                                 const VisualRect& boundary,double sampled,std::string* rejection=nullptr){
        if(rejection)rejection->clear();
        auto reject=[&](const char* reason){if(rejection)*rejection=reason;return false;};
        int i=index(id);double now=visual_now();
        expire_visual();
        if(i<0)return reject("visual_region_missing");
        if(generation!=visualGeneration)return reject("visual_result_stale");
        if(!std::isfinite(sampled)||sampled>now||now-sampled>1)return reject("visual_sample_expired");
        if(viewportWidth<=0||viewportHeight<=0||viewportDpi<48||viewportDpi>768)return reject("visual_viewport_unavailable");
        if(visualPositions.find(id)==visualPositions.end()&&visualPositions.size()>=4)return reject("visual_region_limit");
        const auto& entry=document["regions"][i];const auto& binding=entry["visual_binding"];
        if(entry["position_mode"]!="visual"||binding["calibration_id"]!=calibration)return reject("visual_binding_changed");
        if(!visual_asset_valid(path,binding)){
            visualPositions.erase(id);unavailableVisualAssets.insert(id);stateSequence++;
            if(runtimeChanged)runtimeChanged();return reject("template_unavailable");
        }
        unavailableVisualAssets.erase(id);
        for(double v:{boundary.x,boundary.y,boundary.width,boundary.height})if(!std::isfinite(v)||v<0||v>100000)return reject("visual_result_invalid");
        if(boundary.width<=0||boundary.height<=0)return reject("visual_result_invalid");
        const auto& ref=binding["reference_rect_dip"];
        double rx=ref[0],ry=ref[1],rw=ref[2],rh=ref[3];
        bool features=binding.value("tracking_kind",std::string("boundary"))=="features";
        double limit=features?binding.value("search_limit_dip",500.)+1e-6:32;
        auto prior=featureOrigins.find(id);
        if(features&&prior!=featureOrigins.end()&&prior->second.first==calibration){rx=prior->second.second.x;ry=prior->second.second.y;}
        if(std::max({std::abs(boundary.x-rx),std::abs(boundary.y-ry),std::abs(boundary.x+boundary.width-rx-rw),std::abs(boundary.y+boundary.height-ry-rh)})>limit||
           boundary.width/rw<.8||boundary.width/rw>1.25||boundary.height/rh<.8||boundary.height/rh>1.25)return reject("visual_result_outside_limits");
        // The target position is valid even if the user's separate cover
        // offset puts the cover outside this client. Keep tracking that target.
        if(features)featureOrigins[id]={calibration,boundary};
        if(stableVisualPresentation)confirmedBoundaries[id]={calibration,boundary};
        auto position=visual_geometry(boundary,binding["relation"],viewportWidth,viewportHeight,viewportDpi);
        if(position.nominal.width<=0||position.nominal.height<=0){
            if(visualPositions.erase(id)){stateSequence++;if(runtimeChanged)runtimeChanged();}
            return reject("visual_cover_outside_client");
        }
        position.expires=sampled+1;position.generation=generation;position.calibration=calibration;
        visualPositions[id]=position;stateSequence++;
        if(runtimeChanged)runtimeChanged();return true;
    }
    void expire_visual(){
        double now=visual_now();bool changed=false;
        for(auto it=visualPositions.begin();it!=visualPositions.end();)if(std::max(it->second.expires,it->second.transportHold)<=now){it=visualPositions.erase(it);changed=true;}else ++it;
        if(changed)stateSequence++;
    }
    void update_search_limit(const std::string& id,double limit,uint64_t base){
        expect(base);visual_number(limit,32,500);int i=index(id);
        if(i<0)throw std::runtime_error("region_not_found");
        auto entry=document["regions"][i];
        if(entry["visual_binding"].is_null()||entry["visual_binding"].value("tracking_kind",std::string("boundary"))!="features")
            throw std::runtime_error("feature_mode_required");
        entry["visual_binding"]["search_limit_dip"]=limit;entry=canonical_region(entry);
        if(entry==document["regions"][i])return;
        document["regions"][i]=entry;visualPositions.erase(id);revision++;stateSequence++;
    }
    void update_cover_relation(const std::string& id,const json& relation,uint64_t base){
        expect(base);validate_visual_relation(relation);int i=index(id);
        if(i<0)throw std::runtime_error("region_not_found");
        if(document["regions"][i]["position_mode"]!="visual")throw std::runtime_error("visual_mode_required");
        const auto& a=document["regions"][i]["visual_binding"]["reference_rect_dip"];
        VisualRect reference{a[0],a[1],a[2],a[3]};auto nominal=visual_nominal(reference,relation);
        if(nominal.width<8||nominal.height<8)throw std::runtime_error("invalid_visual_cover_size");
        if(viewportWidth>0&&viewportHeight>0){
            auto clipped=visual_geometry(reference,relation,viewportWidth,viewportHeight,viewportDpi).cover;
            if(clipped.width<=0||clipped.height<=0)throw std::runtime_error("cover_outside_client");
        }
        auto entry=document["regions"][i];entry["visual_binding"]["relation"]=relation;entry=canonical_region(entry);
        if(entry==document["regions"][i])return;
        document["regions"][i]=entry;visualPositions.erase(id);revision++;stateSequence++;
    }
    void switch_to_anchor(const std::string& id,uint64_t base,bool savedFallback=false){
        expect(base);int i=index(id);if(i<0)throw std::runtime_error("region_not_found");
        auto current=positioned(i);if(!current.visual)return;
        if(!current.drawable){
            if(!savedFallback)throw std::runtime_error("visual_position_not_ready");
            document["regions"][i]["position_mode"]="anchor";visualPositions.erase(id);confirmedBoundaries.erase(id);revision++;stateSequence++;return;
        }
        auto known=confirmedBoundaries.find(id);
        const auto rect=stableVisualPresentation&&known!=confirmedBoundaries.end()&&known->second.first==document["regions"][i]["visual_binding"]["calibration_id"]?
            visual_geometry(known->second.second,document["regions"][i]["visual_binding"]["relation"],viewportWidth,viewportHeight,viewportDpi).nominal:visualPositions.at(id).nominal;
        json entry=document["regions"][i];
        entry.update({{"position_mode","anchor"},{"x",rect.x},{"y",rect.y},{"width",rect.width},{"height",rect.height},
            {"reference_width",viewportWidth},{"reference_height",viewportHeight}});
        document["regions"][i]=canonical_region(entry);visualPositions.erase(id);confirmedBoundaries.erase(id);revision++;stateSequence++;
    }
    void activate_visual(const std::string& id,uint64_t base){
        expect(base);int i=index(id);if(i<0)throw std::runtime_error("region_not_found");
        if(document["version"]!=2||!visual_asset_valid(path,document["regions"][i]["visual_binding"]))throw std::runtime_error("visual_calibration_required");
        unavailableVisualAssets.erase(id);
        if(document["regions"][i]["position_mode"]=="visual")return;
        document["regions"][i]["position_mode"]="visual";visualPositions.erase(id);revision++;stateSequence++;
    }
    void save(){
        if(!persistent){persistedRevision=revision;return;}
        auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if(revision==persistedRevision&&fs::exists(path)){
            if(!lastAssetMaintenance||now-lastAssetMaintenance>=86400){
                try{visual_retire_assets(path,persistedVisualAssets,persistedVisualAssets,new_id(),now,true,otherApplicationDirectory);}catch(...){}
                lastAssetMaintenance=now;
            }
            return;
        }
        json persisted=document;
        for(const auto& region:hiddenRegions)persisted["regions"].push_back(region);
        auto currentAssets=visual_asset_references(persisted);
        auto maintenanceNonce=new_id();
        fs::create_directories(path.parent_path());
        fs::path temporary=path;temporary+=L".tmp."+wide(new_id());
        HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create configuration temporary file");
        try{
            std::string bytes=persisted.dump(2);size_t offset=0;
            while(offset<bytes.size()){
                DWORD written=0,chunk=DWORD(std::min<size_t>(bytes.size()-offset,1048576));
                if(!WriteFile(file,bytes.data()+offset,chunk,&written,nullptr)||!written)throw std::runtime_error("Cannot write configuration");
                offset+=written;
            }
            if(!FlushFileBuffers(file))throw std::runtime_error("Cannot flush configuration");
            CloseHandle(file);file=INVALID_HANDLE_VALUE;
            if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot replace configuration");
            persistedRevision=revision;
            bool collect=!lastAssetMaintenance||now-lastAssetMaintenance>=86400;
            visual_retire_assets(path,persistedVisualAssets,currentAssets,maintenanceNonce,now,collect,otherApplicationDirectory);
            persistedVisualAssets=std::move(currentAssets);if(collect)lastAssetMaintenance=now;
        }catch(...){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);DeleteFileW(temporary.c_str());throw;}
    }
};
