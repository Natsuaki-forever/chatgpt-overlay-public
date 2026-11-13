#pragma once
#include "config.hpp"
#include "visual_config_tests.hpp"
#include "visual_tracking_tests.hpp"

inline void config_tests(const fs::path& output){
    fs::create_directories(output);
    visual_config_tests(output/L"visual");
    visual_tracking_tests(output/L"tracking-state");
    fs::path path=output/L"配置 roundtrip.json";
    json original={{"version",1},{"regions",json::array({json{{"text","我的工作台 😀"},{"letter_spacing",2.5},
        {"scroll",true},{"scroll_feather",true},{"outline",true},{"glow",true},{"glow_period",1.5}}})}};
    {std::ofstream file(path);file<<original.dump(2);}
    ConfigState state(path);
    json unchanged;{std::ifstream file(path);file>>unchanged;}
    require(unchanged==original,"loading rewrote the original configuration");
    std::string id=state.document["regions"][0]["id"];
    state.update(id,{{"text","最后输入"},{"width",170.5}},state.revision);
    uint64_t revision=state.revision;
    bool conflict=false;try{state.update(id,{{"text","stale"}},revision-1);}catch(const std::exception&){conflict=true;}
    require(conflict&&state.document["regions"][0]["text"]=="最后输入","stale revision overwrote configuration");
    bool bad=false;try{state.update(id,{{"id",new_id()}},revision);}catch(const std::exception&){bad=true;}
    require(bad,"ID was editable");
    state.save();ConfigState reloaded(path);
    require(reloaded.document==state.document,"configuration styles did not roundtrip");
    require(state.persistedRevision==state.revision,"save acknowledgement preceded persistence");
    std::string created=state.create({{"text","第二块"}},state.revision);
    require(created!=id,"created region reused ID");state.erase(created,state.revision);state.save();
    HANDLE locked=CreateFileW(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
    require(locked!=INVALID_HANDLE_VALUE,"cannot lock test configuration");
    state.update(id,{{"text","disk failure pending"}},state.revision);
    bool saveFailed=false;try{state.save();}catch(const std::exception&){saveFailed=true;}
    CloseHandle(locked);
    require(saveFailed&&state.persistedRevision<state.revision,"failed replacement was acknowledged as persisted");
    require(ConfigState(path).document["regions"][0]["text"]=="最后输入","failed save damaged original");
    state.save();require(ConfigState(path).document==state.document,"save retry lost changes");
    fs::path broken=output/L"broken.json";{std::ofstream file(broken);file<<"{broken";}
    bool rejected=false;try{ConfigState invalid(broken);}catch(const std::exception&){rejected=true;}
    std::string bytes;{std::ifstream file(broken);std::getline(file,bytes);}
    require(rejected&&bytes=="{broken","malformed configuration was overwritten");
    ConfigState anchored(output/L"anchors.json");
    std::string anchoredId=anchored.create({{"x",600},{"y",400},{"width",200},{"height",60}},anchored.revision);
    require(anchored.document["regions"][0]["anchor_x"]=="left"&&anchored.document["regions"][0]["anchor_y"]=="top","legacy anchor defaults changed");
    anchored.set_viewport(900,600);
    anchored.update_positioned(anchoredId,{{"anchor_x","right"},{"anchor_y","bottom"}},anchored.revision);
    require(anchored.positioned(0).x==600&&anchored.positioned(0).y==400,"switching anchor moved the rectangle");
    anchored.save();auto anchorRevision=anchored.revision,anchorSequence=anchored.stateSequence;
    anchored.set_viewport(1200,800);
    require(anchored.positioned(0).x==900&&anchored.positioned(0).y==600,"right/bottom did not follow client resize");
    require(anchored.revision==anchorRevision&&anchored.persistedRevision==anchorRevision&&anchored.stateSequence>anchorSequence,
        "window resize dirtied configuration or failed to refresh effective positions");
    require(anchored.document["regions"][0]["x"]==600,"resize overwrote stored coordinates");
    anchored.update_positioned(anchoredId,{{"x",920},{"y",610}},anchored.revision);
    require(anchored.positioned(0).x==920&&anchored.positioned(0).y==610,"editing client coordinates double-applied anchor offset");
    anchored.set_viewport(900,600);
    require(anchored.positioned(0).x==620&&anchored.positioned(0).y==410,"edited right/bottom margin was not preserved");
    anchored.update_positioned(anchoredId,{{"anchor_x","center"},{"anchor_y","center"}},anchored.revision);
    anchored.set_viewport(1100,700);
    require(anchored.positioned(0).x==720&&anchored.positioned(0).y==460,"center anchor did not follow half of resize delta");
    for(int i=0;i<100;i++){anchored.set_viewport(2,2);anchored.set_viewport(1100,700);}
    require(anchored.positioned(0).x==720&&anchored.positioned(0).y==460,"shrink/restore accumulated anchor drift");
    anchored.save();ConfigState restoredAnchors(anchored.path);restoredAnchors.set_viewport(1100,700);
    require(restoredAnchors.positioned(0).x==720&&restoredAnchors.positioned(0).y==460,"anchor configuration did not roundtrip");
    anchored.update_positioned(anchoredId,{{"anchor_x","left"},{"anchor_y","top"}},anchored.revision);
    anchored.set_viewport(1300,900);
    require(anchored.positioned(0).x==720&&anchored.positioned(0).y==460,"switching back to left/top changed current position");
    std::ofstream(output/L"result.json")<<json({{"legacy_read_without_write",true},{"all_styles_roundtrip",true},
        {"stable_ids",true},{"revision_conflict",true},{"atomic_save_failure_and_retry",true},{"malformed_preserved",true},
        {"anchor_switch_no_jump",true},{"anchor_resize_without_config_write",true},{"anchor_coordinates_editable",true},
        {"center_anchor",true},{"anchor_restore_no_drift",true},{"anchor_roundtrip",true}}).dump(2);
}
