#pragma once
#include "config.hpp"

inline json visual_test_relation(const char* mode="follow_bounds"){
    return {{"mode",mode},{"anchor_x","left"},{"anchor_y","top"},{"offset_dip",{0,0}},
        {"fixed_size_dip",{180,80}},{"margins_dip",{1,2,3,4}},{"padding_physical",2}};
}
inline void visual_config_tests(const fs::path& output){
    fs::create_directories(output);ConfigState state(output/L"config.json");
    auto id=state.create({{"x",600},{"y",400},{"width",180},{"height",80},{"anchor_x","right"},{"glow",true}},state.revision);
    auto other=state.create({{"x",20},{"y",30}},state.revision);
    state.set_viewport(900,600);state.set_geometry_context("test-geometry",168);state.save();
    json before=state.document;std::vector<BYTE> pixels(128);for(size_t i=0;i<pixels.size();i++)pixels[i]=BYTE(i);
    state.install_visual(id,{100,100,320,110},168,visual_test_relation(),16,8,pixels,state.revision);
    require(state.document["version"]==2&&state.persistedRevision==state.revision,"visual migration not durable");
    json backup;{std::ifstream input(output/wide(state.document["visual_migration_backup"]));input>>backup;}
    require(backup==before,"v1 backup did not preserve previous configuration");
    auto binding=state.document["regions"][0]["visual_binding"];auto calibration=binding["calibration_id"].get<std::string>();
    require(visual_asset_valid(state.path,binding),"template asset not valid");
    require(!state.positioned(0).drawable&&state.positioned(1).drawable,"visual waiting changed anchor availability");
    auto revision=state.revision,sequence=state.stateSequence;
    std::string rejection;
    unsigned notifications=0;state.runtimeChanged=[&](){notifications++;};
    require(!state.publish_visual_boundary(id,calibration,state.visualGeneration+1,{101,103,320,110},visual_now(),&rejection)&&rejection=="visual_result_stale","old generation accepted or unexplained");
    require(!state.publish_visual_boundary(id,calibration,state.visualGeneration,{101,103,320,110},visual_now()-2,&rejection)&&rejection=="visual_sample_expired","expired sample accepted or unexplained");
    require(state.publish_visual_boundary(id,calibration,state.visualGeneration,{101,103,320,110},visual_now()),"valid test boundary rejected");
    require(notifications==1,"static visual update did not notify host");
    auto r=state.positioned(0);auto nominal=state.visualPositions.at(id).nominal;
    require(r.drawable&&nominal.x==100&&nominal.y==101&&nominal.width==324&&nominal.height==116,"visual relation incorrect");
    require(r.x<nominal.x&&r.width>nominal.width,"physical outset mixed into nominal geometry");
    require(state.revision==revision&&state.stateSequence>sequence,"runtime result dirtied configuration");
    bool denied=false;try{state.update_positioned(id,{{"x",500}},state.revision);}catch(...){denied=true;}
    require(denied,"visual geometry accepted a legacy coordinate patch");
    state.update(id,{{"bold",true}},state.revision);
    require(state.positioned(0).x==r.x,"style change lost effective geometry");
    state.switch_to_anchor(id,state.revision);
    require(state.positioned(0).x==100&&state.positioned(0).width==324,"mode switch used padded or stale geometry");
    state.set_viewport(1000,600);require(state.positioned(0).x==200,"anchor mode did not retain original anchor choice");
    state.activate_visual(id,state.revision);require(!state.positioned(0).drawable,"reactivation reused old coordinates");
    require(state.publish_visual_boundary(id,calibration,state.visualGeneration,{101,103,320,110},visual_now()),"reactivation result rejected");
    state.set_geometry_context("moved-window",168);require(!state.positioned(0).drawable,"window movement did not invalidate result");
    require(state.publish_visual_boundary(id,calibration,state.visualGeneration,{101,103,320,110},visual_now()),"current generation rejected");
    state.visualPositions.at(id).expires=visual_now()-1;state.expire_visual();require(!state.positioned(0).drawable,"expired result still visible");
    state.save();ConfigState restored(state.path);restored.set_viewport(1000,600);
    require(restored.document==state.document&&!restored.positioned(0).drawable,"restart restored runtime coordinates or lost styles");
    fs::path asset=output/L"visual-templates"/(wide(binding["template_id"])+L".vt1");
    {std::ofstream broken(asset,std::ios::binary);broken<<"bad";}
    require(!state.publish_visual_boundary(id,calibration,state.visualGeneration,{101,103,320,110},visual_now(),&rejection)&&rejection=="template_unavailable","corrupt asset accepted or unexplained");
    require(state.tracking_states()[id]["state"]=="UNAVAILABLE","corrupt template reported as waiting forever");
    require(state.positioned(1).drawable,"corrupt visual asset hid anchor region");

    ConfigState failure(output/L"locked.json");auto failedId=failure.create(json::object(),failure.revision);failure.save();
    auto original=failure.document;auto priorRevision=failure.revision;
    HANDLE lock=CreateFileW(failure.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"cannot lock migration test");bool failed=false;
    try{failure.install_visual(failedId,{100,100,320,110},168,visual_test_relation(),16,8,pixels,failure.revision);}catch(...){failed=true;}
    CloseHandle(lock);
    require(failed&&failure.document==original&&failure.revision==priorRevision&&failure.persistedRevision==priorRevision,"failed migration changed memory or acknowledged persistence");
    require(ConfigState(failure.path).document==original,"failed migration damaged original configuration");
    failure.install_visual(failedId,{100,100,320,110},168,visual_test_relation(),16,8,pixels,failure.revision);
    require(failure.document["version"]==2,"migration retry failed");
    unsigned backups=0;for(const auto& file:fs::directory_iterator(output))
        if(file.path().filename().wstring().find(L"locked.json.pre-visual-v1.")==0)backups++;
    require(backups==1,"identical migration retry duplicated v1 backup");
    auto featureRelation=visual_test_relation("position_only");
    failure.install_visual(failedId,{100,100,120,30},168,featureRelation,16,8,pixels,failure.revision,"features");
    require(ConfigState(failure.path).document["regions"][0]["visual_binding"]["tracking_kind"]=="features","feature type not persisted");
    auto featureBefore=failure.document;bool invalidFeature=false;
    try{failure.update_cover_relation(failedId,visual_test_relation("follow_bounds"),failure.revision);}catch(...){invalidFeature=true;}
    require(invalidFeature&&failure.document==featureBefore,"feature size-follow change was not rejected atomically");
    failure.set_viewport(2000,1200);failure.set_geometry_context("feature-search",168);
    auto featureCal=failure.document["regions"][0]["visual_binding"]["calibration_id"].get<std::string>();
    require(failure.publish_visual_boundary(failedId,featureCal,failure.visualGeneration,{500,100,120,30},visual_now()),"feature publication still limited to initial 32 DIP");
    failure.set_viewport(2100,1200);
    require(failure.publish_visual_boundary(failedId,featureCal,failure.visualGeneration,{900,100,120,30},visual_now()),"last confirmed feature origin did not advance");
    require(!failure.publish_visual_boundary(failedId,featureCal,failure.visualGeneration,{1401,100,120,30},visual_now()),"feature publication exceeded recovery range");
    failure.update_search_limit(failedId,300,failure.revision);failure.save();
    require(ConfigState(failure.path).document["regions"][0]["visual_binding"]["search_limit_dip"]==300,"custom search limit did not persist");
    failure.set_geometry_context(json::array({11,22,10,20,2110,1220,true}),168);
    auto transportCal=failure.document["regions"][0]["visual_binding"]["calibration_id"].get<std::string>();
    require(failure.publish_visual_boundary(failedId,transportCal,failure.visualGeneration,{900,100,120,30},visual_now()),"transport test position not published");
    auto layoutGeneration=failure.visualGeneration;auto sampleExpiry=failure.visualPositions.at(failedId).expires;
    failure.set_geometry_context(json::array({11,22,60,90,2160,1290,true}),168);
    require(failure.visualGeneration==layoutGeneration&&failure.translationSerial>0&&failure.positioned(0).drawable,"pure translation discarded valid client geometry");
    require(failure.visualPositions.at(failedId).expires==sampleExpiry&&failure.visualPositions.at(failedId).transportHold>visual_now(),"translation forged sample time instead of bounded pose lease");
    failure.set_geometry_context(json::array({11,22,60,90,2200,1290,true}),168);
    require(failure.visualGeneration>layoutGeneration&&!failure.positioned(0).drawable,"resize retained stale client geometry");
    failure.stableVisualPresentation=true;
    require(failure.publish_visual_boundary(failedId,transportCal,failure.visualGeneration,{900,100,120,30},visual_now()),"stable position not confirmed");
    auto stableWidth=failure.positioned(0).width;
    failure.visualPositions.at(failedId).expires=0;failure.visualPositions.at(failedId).transportHold=0;failure.expire_visual();
    require(failure.positioned(0).drawable,"confirmed presentation expired with sampling lease");
    failure.set_viewport(1000,1200);
    require(failure.positioned(0).drawable&&failure.positioned(0).width<=100,"resize discarded or failed to clip retained geometry");
    failure.set_viewport(2200,1200);require(failure.positioned(0).width==stableWidth,"restored viewport lost retained full cover");
    failure.set_visual_check_interval(30);failure.save();
    require(ConfigState(failure.path).visual_check_interval()==30,"refresh interval did not persist");
    ConfigState anchorProbe(failure.path);anchorProbe.stableVisualPresentation=true;anchorProbe.set_viewport(2200,1200);
    anchorProbe.confirmedBoundaries=failure.confirmedBoundaries;
    anchorProbe.switch_to_anchor(failedId,anchorProbe.revision);
    require(anchorProbe.document["regions"][0]["position_mode"]=="anchor"&&anchorProbe.confirmedBoundaries.empty(),"retained position could not switch back to anchor");
    failure.set_geometry_context(json::array({33,44,60,90,2200,1290,true}),168);
    require(!failure.positioned(0).drawable&&failure.confirmedBoundaries.empty(),"retained position leaked into another target identity");
    bool malformed=false;auto invalid=binding;invalid["template_id"]="../escape";
    try{validate_visual_binding(invalid);}catch(...){malformed=true;}require(malformed,"unsafe asset ID accepted");
    std::ofstream(output/L"result.json")<<json{{"passed",true},{"v1_backup_and_v2_asset",true},
        {"failed_migration_rollback_and_retry",true},{"runtime_not_persisted",true},{"geometry_generation_and_expiry",true},
        {"style_and_mode_roundtrip",true},{"corrupt_asset_isolated",true},{"legacy_geometry_write_rejected",true}}.dump(2);
}
