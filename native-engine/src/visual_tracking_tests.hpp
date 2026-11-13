#pragma once
#include "visual_tracking_state.hpp"

inline void visual_tracking_tests(const fs::path& output){
    fs::create_directories(output);
    try{
    VisualTrackingState s;VisualRect initial{100,100,200,80};
    require(!s.begin(10).request&&!s.visible(),"uncalibrated tracking started");
    s.bind_session(101);
    s.calibrate(initial,168);s.permit(10);
    require(!s.begin(10.34).request,"sampling started before settling");
    auto first=s.begin(10.36);
    require(first.complete&&s.observe(first,1,10.36,10.37,VisualObservation::Complete,initial)&&!s.visible(),
        "one observation displayed coverage");
    auto second=s.begin(10.40);
    require(s.observe(second,2,10.40,10.41,VisualObservation::Complete,initial)&&!s.visible(),
        "closely spaced frames confirmed coverage");
    auto third=s.begin(10.46);
    require(s.observe(third,3,10.46,10.47,VisualObservation::Complete,initial)&&s.visible(),
        "two fresh separated observations did not confirm");
    require(!s.begin(10.60).request,"watch task scheduled too early");
    auto aligned=s;auto alignedWatch=aligned.begin(10.60,false,true);
    require(alignedWatch.request&&!alignedWatch.complete&&!aligned.begin(10.61,false,true).request,
        "shared cadence failed to schedule or allowed multiple in-flight tasks");
    auto watch=s.begin(10.72);require(watch.request&&!watch.complete,"wrong watch task");
    require(!s.begin(10.73).request,"multiple requests in flight");
    require(s.observe(watch,4,10.72,10.73,VisualObservation::Unchanged)&&s.visible(),"watch failed to renew");
    auto watchChange=s;auto changed=watchChange.begin(10.98);
    require(watchChange.observe(changed,5,10.98,10.99,VisualObservation::Changed)&&!watchChange.visible()&&watchChange.failures==0,
        "changed pixels remained visible or counted as recognition failure");
    auto old=s.begin(11.0);s.pause("moving");s.permit(11.0);
    require(!s.visible()&&!s.observe(old,5,11.0,11.01,VisualObservation::Unchanged),"revoked result restored coverage");
    require(!s.begin(11.34).request,"resume skipped settling");
    auto resumed=s.begin(11.36);
    require(!s.observe(resumed,4,11.36,11.37,VisualObservation::Complete,initial),"replayed sample accepted");
    auto stale=s.begin(11.38);
    require(!s.observe(stale,6,10.0,11.39,VisualObservation::Complete,initial),"stale result accepted");
    auto future=s.begin(11.40);
    require(!s.observe(future,7,12.0,11.41,VisualObservation::Complete,initial),"future result accepted");
    auto wrongKind=s.begin(11.42);
    require(!s.observe(wrongKind,8,11.42,11.43,VisualObservation::Unchanged),"watch used for full confirmation");
    for(int i=0;i<3;i++){
        double t=11.5+i*.1;auto failed=s.begin(t);
        require(s.observe(failed,9+i,t,t+.01,VisualObservation::NotFound),"recognition failure not consumed");
    }
    require(s.phase==VisualPhase::NeedsManual&&!s.visible(),"three failures did not lock");
    s.pause("blurred");s.permit(12.0);s.tick(13.0);
    require(s.phase==VisualPhase::NeedsManual&&!s.begin(13).request,"focus change unlocked manual state");
    s.calibrate(initial,168);s.permit(13);auto limit=s.begin(13.36);
    require(s.observe(limit,12,13.36,13.37,VisualObservation::Complete,{133,100,200,80})&&
        s.phase==VisualPhase::NeedsManual,"out-of-domain boundary did not lock immediately");
    require(!s.allowed({100,100,200,63})&&!s.allowed({100,100,251,80})&&
        !s.allowed({100,100,NAN,80})&&s.allowed({132,100,200,80}),"reference constraints changed");

    auto confirm=[&](double t,uint64_t sample,const VisualRect& r){
        s.calibrate(initial,168);s.permit(t);
        auto a=s.begin(t+.36);s.observe(a,sample,t+.36,t+.37,VisualObservation::Complete,r);
        auto b=s.begin(t+.46);s.observe(b,sample+1,t+.46,t+.47,VisualObservation::Complete,r);
        require(s.visible(),"test precondition confirmation failed");
    };
    confirm(20,20,initial);
    for(int i=0;i<3;i++){
        double t=20.72+i*.26;auto w=s.begin(t);
        require(w.request&&!w.complete&&s.observe(w,22+i,t,t+.01,VisualObservation::Unchanged),
            "watch before audit failed");
    }
    auto audit=s.begin(21.47);require(audit.complete,"audit overdue but watch scheduled");
    VisualRect moved{110,100,200,80};
    require(s.observe(audit,25,21.47,21.48,VisualObservation::Complete,moved)&&!s.visible(),
        "changed boundary displayed before reconfirmation");
    auto reconfirm=s.begin(21.57);
    require(s.observe(reconfirm,26,21.57,21.58,VisualObservation::Complete,moved)&&s.visible(),
        "changed boundary did not recover");
    s.tick(22.58);require(!s.visible()&&s.phase==VisualPhase::Paused,"expired check remained visible");
    confirm(30,30,initial);
    // Fresh pixel checks cannot indefinitely postpone the full uniqueness audit.
    for(int i=1;i<=7;i++){
        s.lastCheck=30.46+i*.25;s.tick(s.lastCheck);
        require(s.visible(),"watch lease expired prematurely");
    }
    s.lastCheck=32.47;s.tick(32.47);
    require(!s.visible()&&s.phase==VisualPhase::Paused,"audit deadline extended by local checks");
    s.unavailable("worker_crashed");s.permit(40);s.tick(41);
    require(s.phase==VisualPhase::Unavailable&&!s.begin(41).request,"component failure restarted automatically");
    s.bind_session(102);
    require(s.phase==VisualPhase::Unavailable,"session replacement unlocked component failure");
    require(s.retry_unavailable(),"explicit retry not supported");s.permit(42);
    auto newSession=s.begin(42.36);auto previousSession=newSession;previousSession.session=101;
    require(!s.observe(previousSession,100,42.36,42.37,VisualObservation::Complete,initial),
        "old session accepted or consumed current request");
    require(s.observe(newSession,1,42.36,42.37,VisualObservation::Complete,initial)&&!s.visible(),
        "new session sequence did not restart safely");
    auto newSessionSecond=s.begin(42.46);
    require(s.observe(newSessionSecond,2,42.46,42.47,VisualObservation::Complete,initial)&&s.visible(),
        "explicit retry failed to reconfirm");
    s.pause("moving");s.permit(43);auto out=s.begin(43.36);
    s.observe(out,3,43.36,43.37,VisualObservation::OutOfBounds);
    s.unavailable("capture_environment_changed");
    s.bind_session(103);
    require(!s.retry_unavailable()&&s.phase==VisualPhase::NeedsManual,"retry or new session unlocked manual lock");
    std::ofstream(output/L"result.json")<<json({{"passed",true},{"settle_before_capture",true},
        {"two_distinct_separated_samples",true},{"one_inflight",true},{"revoked_and_replayed_rejected",true},
        {"stale_future_and_wrong_kind_rejected",true},{"manual_lock_survives_focus",true},
        {"fixed_reference_limits",true},{"change_hides_until_confirmed",true},
        {"check_and_audit_deadlines",true},{"unavailable_requires_explicit_recovery",true},
        {"worker_session_isolation_and_explicit_retry",true},
        {"production_coordinator_connected",false},{"real_capture",false}}).dump(2);
    }catch(const std::exception& error){
        std::ofstream(output/L"result.json")<<json{{"passed",false},{"error",error.what()}}.dump(2);throw;
    }
}
