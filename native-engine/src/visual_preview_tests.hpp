#pragma once
#include "visual_runtime.hpp"

inline void visual_preview_tests(Renderer& renderer,const fs::path& output){
    fs::create_directories(output);try{ConfigState state(output/L"config.json");
    auto id=state.create({{"x",160},{"y",135},{"width",120},{"height",35},{"text","SMALL COVER"}},state.revision);
    state.selected=id;state.set_viewport(900,600);state.set_geometry_context("preview-test",168);state.save();
    NormalSurfaces normal(renderer);std::vector<std::unique_ptr<Surface>> surfaces;
    VisualRuntime runtime(state,normal,surfaces,nullptr);
    auto original=state.document;auto revision=state.revision;
    runtime.begin_preview(id);auto token=runtime.previewToken;
    runtime.set_reference(token,{100,100,260,100});
    auto preview=runtime.preview_state();
    require(std::abs(preview["cover"]["x"].get<double>()-160)<1&&std::abs(preview["cover"]["y"].get<double>()-135)<1&&preview["cover"]["width"]==120,
        "independent reference changed original cover");
    bool invalid=false;try{runtime.set_reference("wrong-token",{10,10,200,80});}catch(...){invalid=true;}
    require(invalid&&runtime.previewToken==token,"invalid token changed preview");
    auto relation=runtime.previewRelation;relation["mode"]="follow_bounds";
    relation["margins_dip"]={-50,-20,-40,-30};relation["padding_physical"]=2;
    runtime.set_preview_relation(token,relation);preview=runtime.preview_state();
    require(preview["cover"]["x"].get<double>()<150&&preview["cover"]["width"].get<double>()>170,
        "preview did not use physical padding");
    require(state.document==original&&state.revision==revision&&ConfigState(state.path).document==original,
        "preview wrote configuration");
    runtime.cancel_preview();require(state.document==original&&runtime.stage.empty(),"cancel changed original configuration");
    runtime.begin_preview(id,"features");token=runtime.previewToken;
    runtime.set_reference(token,{100,100,120,30});
    require(runtime.preview_state()["tracking_kind"]=="features","feature kind missing from preview");
    bool featureInvalid=false;
    auto featureRelation=runtime.previewRelation;featureRelation["mode"]="follow_bounds";
    try{runtime.set_preview_relation(token,featureRelation);}catch(...){featureInvalid=true;}
    require(featureInvalid&&state.document==original,"feature preview allowed bounds following");
    featureInvalid=false;
    try{runtime.set_reference(token,{10,10,600,200});}catch(...){featureInvalid=true;}
    require(featureInvalid,"oversized feature reference passed early validation");
    runtime.confirm_preview(token);
    require(runtime.operationKind=="features"&&runtime.trackingKind=="features","confirm lost feature kind");
    runtime.abort_calibration();
    runtime.begin_preview(id);token=runtime.previewToken;runtime.set_reference(token,{100,100,260,100});
    state.update(id,{{"bold",true}},state.revision);runtime.tick(nullptr,true);
    require(runtime.stage.empty()&&runtime.error=="calibration_context_changed","config change did not revoke calibration token");
    std::vector<BYTE> pixels(128);for(size_t i=0;i<pixels.size();i++)pixels[i]=BYTE(i);
    relation["mode"]="position_only";relation["offset_dip"]={60,35};relation["fixed_size_dip"]={120,35};
    state.install_visual(id,{100,100,260,100},168,relation,16,8,pixels,state.revision);
    auto binding=state.document["regions"][0]["visual_binding"];
    runtime.activate_region(id);runtime.reference={100,100,260,100};runtime.policy.calibrate(runtime.reference,168);
    runtime.policy.phase=VisualPhase::NeedsManual;
    runtime.begin_preview(id);token=runtime.previewToken;runtime.set_reference(token,{105,100,260,100});runtime.cancel_preview();
    require(runtime.policy.phase==VisualPhase::NeedsManual&&state.document["regions"][0]["visual_binding"]==binding,
        "cancelled manual reselect unlocked or damaged calibration");
    auto changed=relation;changed["offset_dip"]={70,40};
    state.update_cover_relation(id,changed,state.revision);state.save();
    auto after=state.document["regions"][0]["visual_binding"];
    require(after["calibration_id"]==binding["calibration_id"]&&after["template_id"]==binding["template_id"]&&
        after["reference_rect_dip"]==binding["reference_rect_dip"]&&after["relation"]==changed&&runtime.policy.phase==VisualPhase::NeedsManual,
        "relation edit changed calibration or unlocked manual state");
    invalid=false;auto bad=changed;bad["margins_dip"][0]=5000;
    auto before=state.document;try{state.update_cover_relation(id,bad,state.revision);}catch(...){invalid=true;}
    require(invalid&&state.document==before,"invalid relation partially changed configuration");
    invalid=false;bad=changed;bad["offset_dip"]={0,2000};
    try{state.update_cover_relation(id,bad,state.revision);}catch(...){invalid=true;}
    require(invalid&&state.document==before,"fully vertical offscreen relation was accepted");
    runtime.begin_preview(id);token=runtime.previewToken;runtime.set_reference(token,{100,100,260,100});
    auto replacement=runtime.previewRelation;replacement["mode"]="follow_bounds";replacement["margins_dip"]={0,0,0,0};
    runtime.set_preview_relation(token,replacement);runtime.confirm_preview(token);
    require(runtime.stage=="queued"&&runtime.pendingRelation==replacement&&state.document==before,
        "confirm bypassed capability check or wrote prematurely");
    runtime.abort_calibration();require(runtime.stage.empty()&&runtime.policy.phase==VisualPhase::NeedsManual&&state.document==before,
        "cancel after confirmation unlocked manual calibration");
    runtime.policy.phase=VisualPhase::Paused;runtime.error.clear();
    runtime.begin_preview(id);token=runtime.previewToken;runtime.set_reference(token,{100,100,260,100});runtime.confirm_preview(token);
    runtime.abort_calibration();require(runtime.stage=="queued"&&!runtime.manual&&state.document==before,
        "cancelled replacement did not restore the existing visual binding");
    runtime.stage.clear();
    // A deleted owner's reply must be discarded before parsing its payload;
    // the currently selected surviving region must never consume that reply.
    auto survivor=state.create({{"text","SURVIVOR"}},state.revision);
    runtime.activate_region(survivor);runtime.policy.phase=VisualPhase::Following;
    runtime.policy.reason="following";runtime.captureReady=true;
    runtime.taskOwner=id;runtime.serial=41;runtime.task={{"task_id",41}};
    runtime.remove_region(id);
    runtime.route_result({{"task_id",41},{"status","unavailable"},{"reason","late-deleted-owner"}},nullptr);
    require(runtime.task.is_null()&&runtime.id==survivor&&runtime.captureReady&&
        runtime.policy.phase==VisualPhase::Following&&runtime.error.empty(),"deleted owner result affected survivor");
    runtime.route_result({{"task_id",40}},nullptr);
    require(runtime.policy.phase==VisualPhase::Following,"old result affected survivor");
    bool futureRejected=false;try{runtime.route_result({{"task_id",42}},nullptr);}catch(...){futureRejected=true;}
    require(futureRejected,"unsolicited future result accepted");
    state.applicationId="active-application";state.bindingEpoch=8;
    for(const auto& oldGrant:{json{{"application_id","previous-application"},{"binding_epoch",8}},
                              json{{"application_id",state.applicationId},{"binding_epoch",7}}}){
        runtime.grant=oldGrant;runtime.grant["generation"]=state.visualGeneration;
        runtime.taskOwner=survivor;runtime.task={{"task_id",++runtime.serial}};
        runtime.route_result({{"task_id",runtime.serial},{"status","unavailable"},{"reason","late-binding"}},nullptr);
        require(runtime.task.is_null()&&runtime.policy.phase==VisualPhase::Following&&runtime.error.empty(),
            "late application or binding result affected the current region");
    }
    runtime.activate_region(id);runtime.calibration="locked-calibration";runtime.policy.phase=VisualPhase::NeedsManual;
    runtime.region_status(runtime);runtime.pause_region(id,"geometry_changed");runtime.remove_region(id);
    runtime.activate_region(survivor);
    require(runtime.policy.phase==VisualPhase::Following&&runtime.manualLocks.count("locked-calibration"),
        "region removal lost manual lock or paused survivor");
    runtime.lastDelivered=10;runtime.workerStarted=100;
    require(!runtime.capture_idle_expired(100.01)&&!runtime.capture_idle_expired(104.99),
        "fresh worker inherited an expired old capture lease");
    require(runtime.capture_idle_expired(105),"unsampled worker never became idle");
    runtime.lastDelivered=104;
    require(!runtime.capture_idle_expired(108.99)&&runtime.capture_idle_expired(109),
        "new samples did not renew the worker idle interval");
    // The reference remains inside a short window while its independent cover
    // offset puts the cover just below the client (the reported real geometry).
    ConfigState clipped(output/L"clipped.json");
    auto clippedId=clipped.create(json::object(),clipped.revision);clipped.selected=clippedId;
    clipped.set_viewport(1451.4285714285713f,816.5714285714286f);clipped.set_geometry_context("clipped",168);clipped.save();
    auto clippedRelation=visual_test_relation("position_only");
    clippedRelation["anchor_x"]="center";clippedRelation["anchor_y"]="center";
    clippedRelation["fixed_size_dip"]={73.71,17.14};clippedRelation["offset_dip"]={6.0028424944,42.8557216099};
    clipped.install_visual(clippedId,{893.7142857143,758.8571428571,94.2857142857,33.7142857143},168,
        clippedRelation,16,8,pixels,clipped.revision,"features");
    auto clippedDocument=clipped.document;auto clippedRevision=clipped.revision;
    NormalSurfaces clippedNormal(renderer);std::vector<std::unique_ptr<Surface>> clippedSurfaces;
    VisualRuntime clippedRuntime(clipped,clippedNormal,clippedSurfaces,nullptr);clippedRuntime.activate_region(clippedId);
    clippedRuntime.trackingKind="features";clippedRuntime.calibration=clipped.document["regions"][0]["visual_binding"]["calibration_id"];
    clippedRuntime.reference={893.7142857143,758.8571428571,94.2857142857,33.7142857143};
    auto& cp=clippedRuntime.policy;cp.featureRecovery=true;cp.calibrate(clippedRuntime.reference,168);
    cp.boundary=clippedRuntime.reference;cp.phase=VisualPhase::Following;cp.permitted=true;
    clippedRuntime.generation=clipped.visualGeneration;clippedRuntime.publish_observation(visual_now());clippedRuntime.status();
    require(cp.visible()&&!cp.locked()&&!clipped.positioned(0).drawable&&clippedRuntime.error.empty(),
        "off-client cover was treated as permanent tracking failure");
    require(clipped.tracking_states()[clippedId]["state"]=="HIDDEN"&&
        clipped.tracking_states()[clippedId]["reason"]=="visual_cover_outside_client","off-client cover has no clear status");
    require(clipped.featureOrigins.at(clippedId).second.y==cp.boundary.y,"hidden cover lost confirmed target origin");
    cp.pause("target_not_available");clippedRuntime.status();
    require(clipped.tracking_states()[clippedId]["state"]=="HIDDEN","opening editor erased the off-client explanation");
    cp.phase=VisualPhase::Following;cp.permitted=true;
    clipped.set_viewport(1451.4285714285713f,850);clippedRuntime.generation=clipped.visualGeneration;
    clippedRuntime.publish_observation(visual_now());clippedRuntime.status();
    require(clipped.positioned(0).drawable&&clippedRuntime.presentationReason.empty(),"cover did not return after window grew");
    clipped.set_viewport(1451.4285714285713f,816.5714285714286f);clippedRuntime.generation=clipped.visualGeneration;
    clippedRuntime.publish_observation(visual_now());
    cp.boundary.y=650;clippedRuntime.publish_observation(visual_now());clippedRuntime.status();
    require(clipped.positioned(0).drawable,"cover did not return when the reference moved inside without a resize");
    auto trusted=cp.featureOrigin;cp.boundary.x+=501;clippedRuntime.publish_observation(visual_now());
    require(!cp.locked()&&!cp.permitted&&cp.reason=="visual_result_outside_limits"&&cp.featureOrigin.x==trusted.x,
        "rejected publication advanced origin or permanently locked feature tracking");
    require(clipped.document==clippedDocument&&clipped.revision==clippedRevision,"clipping recovery modified saved settings");
    std::ofstream(output/L"result.json")<<json{{"passed",true},{"independent_reference_preserves_cover",true},
        {"preview_uses_shared_geometry_and_padding",true},{"preview_and_cancel_do_not_write",true},
        {"late_application_and_binding_results_isolated",true},
        {"token_and_revision_guard",true},{"cancel_preserves_manual_lock_and_binding",true},
        {"relation_edit_preserves_reference_template_and_lock",true},{"invalid_relation_atomic",true},
        {"confirm_queues_without_early_persistence",true},{"deleted_and_stale_results_isolated",true},
        {"manual_lock_retained_without_pausing_survivor",true},{"restarted_worker_has_own_idle_interval",true},
        {"reported_off_client_geometry_reproduced",true},{"hidden_target_remains_tracked",true},
        {"window_growth_and_reference_motion_restore_cover",true},{"rejected_origin_does_not_advance",true}}.dump(2);
    }catch(const std::exception& e){std::ofstream(output/L"result.json")<<json{{"passed",false},{"error",e.what()}}.dump(2);throw;}
}
