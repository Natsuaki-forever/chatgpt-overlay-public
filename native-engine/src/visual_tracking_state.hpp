#pragma once
#include "visual_config.hpp"

// Main-thread policy only. No capture, process launch, configuration writes or
// publication is performed here. The coordinator must revoke permission on
// target/geometry/calibration changes before accepting another result.
enum class VisualPhase {Uncalibrated,Verifying,Following,Paused,NeedsManual,Unavailable};
enum class VisualObservation {Complete,Unchanged,Changed,NotFound,OutOfBounds};
struct VisualTicket {
    uint64_t epoch=0,request=0,session=0;
    bool complete=false;
};
struct VisualTrackingState {
    VisualPhase phase=VisualPhase::Uncalibrated;
    std::string reason="uncalibrated";
    VisualRect reference{},boundary{},candidate{};
    VisualRect featureOrigin{};
    bool featureRecovery=false;
    bool retryTemporaryLoss=false;
    double searchLimit=500;
    double dpi=96,readyAt=0,lastCheck=0,lastAudit=0,candidateTime=0,lastSampleTime=0;
    uint64_t epoch=0,nextRequest=0,inflight=0,lastSample=0,session=0;
    unsigned failures=0;
    bool permitted=false,hasCandidate=false;

    bool locked()const{return phase==VisualPhase::NeedsManual||phase==VisualPhase::Unavailable;}
    void invalidate(){++epoch;inflight=0;hasCandidate=false;}
    void pause(const std::string& why){
        permitted=false;invalidate();
        if(!locked()&&phase!=VisualPhase::Uncalibrated){phase=VisualPhase::Paused;reason=why;}
    }
    void unavailable(const std::string& why){
        if(phase==VisualPhase::NeedsManual){pause(why);return;}
        pause(why);phase=VisualPhase::Unavailable;reason=why;
    }
    // Only after the coordinator authenticates a newly launched worker.
    // Session changes invalidate every old ticket, but cannot unlock a region.
    void bind_session(uint64_t identity){
        if(!identity)throw std::runtime_error("invalid_worker_session");
        if(identity==session)return;
        pause("worker_session_changed");session=identity;lastSample=0;lastSampleTime=0;
    }
    bool retry_unavailable(){
        if(phase!=VisualPhase::Unavailable)return false;
        invalidate();permitted=false;failures=0;
        phase=VisualPhase::Paused;reason="awaiting_permission";return true;
    }
    static bool valid(const VisualRect& r){
        return std::isfinite(r.x)&&std::isfinite(r.y)&&std::isfinite(r.width)&&std::isfinite(r.height)&&
            r.width>0&&r.height>0&&std::isfinite(r.x+r.width)&&std::isfinite(r.y+r.height);
    }
    // Only an explicit, successfully persisted manual calibration calls this.
    // This does not reset the worker's sample sequence.
    void calibrate(const VisualRect& r,double newDpi){
        if(!valid(r)||!std::isfinite(newDpi)||newDpi<48||newDpi>768)throw std::runtime_error("invalid_tracking_reference");
        invalidate();reference=r;featureOrigin=r;dpi=newDpi;permitted=false;failures=0;
        phase=VisualPhase::Paused;reason="awaiting_permission";
    }
    void permit(double now,double settle=.35){
        if(permitted||locked()||phase==VisualPhase::Uncalibrated)return;
        permitted=true;readyAt=now+settle;reason="settling";
    }
    void tick(double now,bool preserveGeometry=false){
        if(!preserveGeometry&&phase==VisualPhase::Following&&
            (now-lastCheck>=1.0||now-lastAudit>=2.0)){
            pause("freshness_expired");return;
        }
        if(permitted&&phase==VisualPhase::Paused&&now>=readyAt){
            phase=VisualPhase::Verifying;reason="confirming";
        }
    }
    bool visible()const{return permitted&&phase==VisualPhase::Following;}
    bool due_complete(double now)const{
        return phase==VisualPhase::Verifying||(phase==VisualPhase::Following&&now-lastAudit>=1.0);
    }
    VisualTicket begin(double now,bool forceComplete=false,bool sharedCadence=false,bool preserveGeometry=false){
        tick(now,preserveGeometry);
        if(!session||!permitted||inflight||locked()||
           (phase!=VisualPhase::Verifying&&phase!=VisualPhase::Following))return {};
        bool complete=forceComplete||due_complete(now);
        if(!complete&&!sharedCadence&&now-lastCheck<.25)return {};
        inflight=++nextRequest;return {epoch,inflight,session,complete};
    }
    bool allowed(const VisualRect& r)const{
        const auto& origin=featureRecovery?featureOrigin:reference;
        double limit=featureRecovery?searchLimit+1e-6:32;
        return valid(r)&&r.width>=reference.width/1.25&&r.width<=reference.width*1.25&&
            r.height>=reference.height/1.25&&r.height<=reference.height*1.25&&
            std::abs(r.x-origin.x)<=limit&&std::abs(r.y-origin.y)<=limit&&
            std::abs(r.x+r.width-origin.x-origin.width)<=limit&&
            std::abs(r.y+r.height-origin.y-origin.height)<=limit;
    }
    bool consistent(const VisualRect& a,const VisualRect& b)const{
        double tolerance=4*96/dpi;
        return std::abs(a.x-b.x)<=tolerance&&std::abs(a.y-b.y)<=tolerance&&
            std::abs(a.x+a.width-b.x-b.width)<=tolerance&&
            std::abs(a.y+a.height-b.y-b.height)<=tolerance;
    }
    // Return true only when a fresh observation is accepted. A true return is
    // not permission to display: the caller must also inspect visible().
    bool observe(const VisualTicket& ticket,uint64_t sample,double sampleTime,double now,
                 VisualObservation result,const VisualRect& r={},bool preserveGeometry=false){
        tick(now,preserveGeometry);
        if(!ticket.request||ticket.session!=session||ticket.epoch!=epoch||ticket.request!=inflight||!permitted||locked())return false;
        inflight=0;
        if(!std::isfinite(sampleTime)||sampleTime>now||now-sampleTime>=1.0||
           sample<=lastSample||sampleTime<=lastSampleTime)return false;
        if((ticket.complete&&result==VisualObservation::Unchanged)||
           (!ticket.complete&&result==VisualObservation::Complete))return false;
        lastSample=sample;lastSampleTime=sampleTime;
        if(result==VisualObservation::Changed){
            hasCandidate=false;phase=VisualPhase::Verifying;reason="image_changed";return true;
        }
        if(result==VisualObservation::OutOfBounds||
           (result==VisualObservation::Complete&&!allowed(r))){
            if(featureRecovery||retryTemporaryLoss){hasCandidate=false;phase=VisualPhase::Verifying;reason="search_outside_range";return true;}
            invalidate();phase=VisualPhase::NeedsManual;reason="outside_reference_limits";return true;
        }
        if(result==VisualObservation::NotFound){
            hasCandidate=false;phase=VisualPhase::Verifying;reason="recognition_failed";
            if(++failures>=3&&!featureRecovery&&!retryTemporaryLoss){invalidate();phase=VisualPhase::NeedsManual;reason="reselect_required";}
            return true;
        }
        if(result==VisualObservation::Unchanged){
            if(phase!=VisualPhase::Following)return false;
            lastCheck=sampleTime;return true;
        }
        failures=0;
        if(phase==VisualPhase::Following&&consistent(boundary,r)){
            boundary=r;lastCheck=lastAudit=sampleTime;return true;
        }
        // A changed audit hides immediately and needs another full observation.
        phase=VisualPhase::Verifying;reason="confirming";
        if(hasCandidate&&consistent(candidate,r)){
            if(sampleTime-candidateTime<.08)return true;
            boundary=r;lastCheck=lastAudit=sampleTime;hasCandidate=false;
            phase=VisualPhase::Following;reason="following";return true;
        }
        candidate=r;candidateTime=sampleTime;hasCandidate=true;return true;
    }
};
