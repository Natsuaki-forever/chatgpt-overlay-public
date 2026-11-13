#pragma once
#include "issues.hpp"
#include "visual_tracking_state.hpp"
#include "visual_search.hpp"
#include "visual_refresh.hpp"
#include "system.hpp"

inline std::vector<BYTE> visual_unhex(const std::string& text){
    if(text.size()%2||text.size()>262168||text.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::runtime_error("invalid_worker_template");
    std::vector<BYTE> bytes;bytes.reserve(text.size()/2);
    for(size_t i=0;i<text.size();i+=2)bytes.push_back(BYTE(std::stoul(text.substr(i,2),nullptr,16)));
    return bytes;
}
inline std::string visual_bytes_hex(const std::vector<BYTE>& bytes){
    const char* digits="0123456789abcdef";std::string text;text.reserve(bytes.size()*2);
    for(BYTE byte:bytes){text+=digits[byte>>4];text+=digits[byte&15];}return text;
}
inline std::wstring visual_arg(const std::wstring& value){
    std::wstring out=L"\"";size_t count=0;
    for(wchar_t c:value){if(c==L'\\'){count++;continue;}out.append(count*(c==L'"'?2:1),L'\\');count=0;if(c==L'"')out+=L'\\';out+=c;}
    out.append(count*2,L'\\');return out+L"\"";
}

struct VisualRegionContext {
    std::string reportedIssue;
    VisualTrackingState policy;
    std::string id,calibration,error,templateHex,trackingKind="boundary";
    VisualRect reference{},editBoundary{};
    uint64_t generation=0,editGeneration=0,checks=0,audits=0,reusedChecks=0;
    double lastTask=0,lastAttempt=0;
    bool editBoundaryValid=false,captureReady=false;
    FeatureSearch search;
    json searchDiagnostic;
    std::string presentationReason;
    uint64_t presentationGeneration=0;
    VisualRefreshSchedule refresh;
};

struct VisualRuntime : VisualRegionContext {
    ConfigState& config;NormalSurfaces& normal;std::vector<std::unique_ptr<Surface>>& surfaces;
    HWND host;fs::path python,script,deps,executable;
    std::function<void()> reconcile;
    std::unique_ptr<PipeServer> pipe;
    WinHandle process,job,workerWake;
    DWORD workerPid=0;
    std::string stage,taskOwner,lastServed,operationOwner,operationError;
    uint64_t serial=0,lastMessage=0,connection=0,queuedRevision=0,queuedGeneration=0,workerSession=0;
    uint64_t handledTranslation=0;
    double queuedAt=0,taskStarted=0,workerStarted=0,idleSince=0,budgetAt=0,budgetMs=0,lastDelivered=0;
    bool manual=false,authenticated=false;
    VisualPhase priorManualPhase=VisualPhase::Uncalibrated;
    VisualRect original{},operationReference{};
    json grant,task,savedCalibration,lastObservation;
    std::string previewToken,previewKind="boundary",operationKind="boundary";
    double previewSearchLimit=500,operationSearchLimit=500;
    uint64_t previewRevision=0,previewGeneration=0;
    VisualRect previewOriginal{},previewReference{};
    json previewRelation,pendingRelation;
    bool previewHasReference=false,referencePicking=false;
    bool previewResumePending=false;
    std::map<std::string,VisualRegionContext> parked;
    std::set<std::string> manualLocks;
    bool windowPermitted=false,sharedFault=false,exclusive=false;
    double resumeQualificationAt=0;
    HWND blockedWindow=nullptr;
    VisualTicket ticket;
    std::map<HWND,LONG_PTR> qualified;
    std::map<HWND,DWORD> previousAffinity;
    VisualRuntime(ConfigState& config,NormalSurfaces& normal,std::vector<std::unique_ptr<Surface>>& surfaces,HWND host):
        config(config),normal(normal),surfaces(surfaces),host(host){config.stableVisualPresentation=true;}
    ~VisualRuntime(){stop();}
    template<class F> void each_region(F visit){
        if(!id.empty())visit(static_cast<VisualRegionContext&>(*this));
        for(auto& entry:parked)visit(entry.second);
    }
    VisualRegionContext* region(const std::string& key){
        if(key==id&&!id.empty())return this;
        auto found=parked.find(key);return found==parked.end()?nullptr:&found->second;
    }
    const VisualRegionContext* region(const std::string& key)const{
        if(key==id&&!id.empty())return this;
        auto found=parked.find(key);return found==parked.end()?nullptr:&found->second;
    }
    void activate_region(const std::string& key){
        if(key==id)return;
        if(!id.empty()){auto old=id;parked[old]=std::move(static_cast<VisualRegionContext&>(*this));}
        auto found=parked.find(key);
        if(found!=parked.end()){static_cast<VisualRegionContext&>(*this)=std::move(found->second);parked.erase(found);}
        else {static_cast<VisualRegionContext&>(*this)=VisualRegionContext{};id=key;}
    }
    void region_status(VisualRegionContext& r){
        if(r.id.empty())return;
        if(r.policy.phase==VisualPhase::NeedsManual&&!r.calibration.empty())manualLocks.insert(r.calibration);
        const char* names[]={"UNCALIBRATED","VERIFYING","FOLLOWING","PAUSED","NEEDS_MANUAL","UNAVAILABLE"};
        json value={{"state",names[int(r.policy.phase)]},{"reason",r.policy.reason},{"generation",config.visualGeneration}};
        if(!r.policy.locked()&&!r.presentationReason.empty()&&r.presentationGeneration==config.visualGeneration)
            value.update({{"state","HIDDEN"},{"reason",r.presentationReason}});
        int index=config.index(r.id);
        if(index>=0){
            const auto& reason=r.policy.reason;
            std::string issue;
            if(r.presentationReason=="visual_cover_outside_client"&&r.presentationGeneration==config.visualGeneration)issue="tracking_outside";
            else if(r.policy.phase==VisualPhase::NeedsManual)issue="tracking_manual";
            else if(r.policy.phase==VisualPhase::Unavailable)issue="tracking_unavailable";
            else if(reason=="roi_occluded"||reason=="transient_occlusion")issue="tracking_occluded";
            else if(reason=="search_ambiguous")issue="tracking_ambiguous";
            else if(reason=="search_not_found")issue="tracking_not_found";
            // A search in progress does not prove that the preceding problem has recovered.
            if(issue!=r.reportedIssue&&(!issue.empty()||r.policy.phase==VisualPhase::Following))
                update_native_issue(config.path.parent_path()/L"diagnostics",r.reportedIssue,issue,index+1);
        }
        if(index>=0&&config.positioned(index).drawable&&(!r.policy.visible()||!r.refresh.active))
            value.update({{"state","HOLDING"},{"reason",r.refresh.active?"checking_retained_position":r.policy.reason=="roi_occluded"?"occluded_retained_position":"retained_position"}});
        if(config.visualRuntimeStates[r.id]!=value){config.visualRuntimeStates[r.id]=value;config.stateSequence++;}
        if(!r.policy.visible()&&config.visualPositions.erase(r.id)){config.stateSequence++;if(config.runtimeChanged)config.runtimeChanged();}
    }
    void pause_all(const std::string& why){
        each_region([&](auto& r){r.policy.pause(why);region_status(r);});
    }
    void remove_region(const std::string& key){
        if(auto r=region(key);r&&r->policy.phase==VisualPhase::NeedsManual&&!r->calibration.empty())manualLocks.insert(r->calibration);
        config.visualPositions.erase(key);config.visualRuntimeStates.erase(key);parked.erase(key);
        if(key==id)static_cast<VisualRegionContext&>(*this)=VisualRegionContext{};
        config.stateSequence++;
    }
    void deactivate_region(const std::string& key){
        if(operationOwner==key)exclusive=false;
        if(operationOwner==key&&!stage.empty()){stop();stage.clear();previewToken.clear();referencePicking=false;}
        if(operationOwner==key){operationError.clear();pendingRelation=json();operationOwner.clear();}
        remove_region(key);
        bool active=false;for(const auto& r:config.document["regions"])if(r["position_mode"]=="visual")active=true;
        normal.separate=active;if(!active){stop();qualified.clear();}
    }
    void pause_region(const std::string& key,const std::string& why){
        if(auto r=region(key)){
            if(why=="relation_changed")r->presentationReason.clear();
            r->policy.pause(why);region_status(*r);
        }
    }
    void fail_all(const std::string& why){
        if(!stage.empty())operationError=why;
        sharedFault=true;restore_affinity();stage.clear();task=json();qualified.clear();
        each_region([&](auto& r){r.captureReady=false;r.error=why;r.policy.unavailable(why);region_status(r);});
        if(reconcile)reconcile();
    }
    bool timer_needed()const{
        if(process.value||!stage.empty())return true;
        if(!windowPermitted)return false;
        if(!id.empty()&&!policy.locked()&&(policy.permitted||!captureReady))return true;
        for(const auto& entry:parked)if(!entry.second.policy.locked()&&(entry.second.policy.permitted||!entry.second.captureReady))return true;
        return false;
    }
    UINT timer_period()const{
        if(!stage.empty()||!task.is_null())return 50;
        if(!windowPermitted)return 1000;
        double now=visual_now();int wait=refresh.wait_ms(now);
        for(const auto& r:parked)wait=std::min(wait,r.second.refresh.wait_ms(now));
        return UINT(wait);
    }
    void wake_worker(){if(workerWake.value)SetEvent(workerWake.value);}
    void request_refresh(VisualRegionContext& r,double now,double settle=.15){
        bool notify=!r.refresh.requested;r.refresh.request(now,settle);
        if(notify)wake_worker();
    }
    bool capture_idle_expired(double now)const{
        return now-std::max(workerStarted,lastDelivered)>=5;
    }
    std::string selected_error()const{
        if(config.selected==operationOwner&&!operationError.empty())return operationError;
        auto selected=region(config.selected);return selected?selected->error:std::string();
    }
    json diagnostics()const{
        json result=json::object();
        auto append=[&](const VisualRegionContext& r){if(!r.id.empty())result[r.id]={{"checks",r.checks},{"audits",r.audits},
            {"capture_ready",r.captureReady},{"phase",int(r.policy.phase)},{"inflight",r.policy.inflight},
            {"capture_reuses",r.reusedChecks},{"last_sample",r.policy.lastSample},{"last_sample_t",r.policy.lastSampleTime},
            {"reference",{r.reference.x,r.reference.y,r.reference.width,r.reference.height}},
            {"search_origin",visual_rect_json(r.policy.featureOrigin)},{"search_detail",r.searchDiagnostic}};};
        append(*this);for(const auto& item:parked)append(item.second);return result;
    }
    void begin_edit(){
        each_region([&](auto& r){r.editBoundaryValid=r.policy.visible()&&r.generation==config.visualGeneration&&visual_now()-r.policy.lastCheck<1;
            auto retained=config.confirmedBoundaries.find(r.id);
            if(retained!=config.confirmedBoundaries.end()&&retained->second.first==r.calibration){r.editBoundaryValid=true;r.editBoundary=retained->second.second;}
            else r.editBoundary=r.policy.boundary;
            r.editGeneration=config.visualGeneration;});
    }
    json render_preview()const{
        if(stage=="preview")return preview_state();
        int i=config.index(config.selected);if(i<0||config.document["regions"][i]["position_mode"]!="visual")return nullptr;
        const auto& binding=config.document["regions"][i]["visual_binding"];auto a=binding["reference_rect_dip"];
        VisualRect r{a[0],a[1],a[2],a[3]};auto search=r;
        auto context=region(config.selected);
        if(context&&context->editBoundaryValid&&context->editGeneration==config.visualGeneration)r=context->editBoundary;
        auto cover=visual_geometry(r,binding["relation"],config.viewportWidth,config.viewportHeight,config.viewportDpi).cover;
        return {{"id",config.selected},{"reference",{r.x,r.y,r.width,r.height}},
            {"search_reference",{search.x,search.y,search.width,search.height}},{"cover",visual_rect_json(cover)}};
    }
    static VisualRect saved_visual_cover(const json& binding){
        const auto& reference=binding.at("reference_rect_dip");
        return visual_nominal({reference[0],reference[1],reference[2],reference[3]},binding.at("relation"));
    }
    void begin_preview(const std::string& selected,const std::string& kind="boundary",double searchLimit=500){
        if(kind!="boundary"&&kind!="features")throw std::runtime_error("invalid_tracking_kind");
        visual_number(searchLimit,32,500);
        int i=config.index(selected);if(i<0)throw std::runtime_error("region_not_found");
        bool resumePending=stage=="queued"&&!manual;
        if(!stage.empty()&&!resumePending)throw std::runtime_error("calibration_in_progress");
        size_t count=0;for(const auto& r:config.document["regions"])if(r["position_mode"]=="visual"&&r["id"]!=selected)count++;
        if(count>=4)throw std::runtime_error("visual_region_limit");
        auto r=config.positioned(i);previewOriginal={r.x,r.y,r.width,r.height};
        previewResumePending=resumePending;
        activate_region(selected);stop();stage="preview";error.clear();operationOwner=selected;operationError.clear();previewToken=new_id();
        previewRevision=config.revision;previewGeneration=config.visualGeneration;previewHasReference=false;referencePicking=true;
        previewKind=kind;previewSearchLimit=searchLimit;
        previewRelation={{"mode","position_only"},{"anchor_x","left"},{"anchor_y","top"},{"offset_dip",{0,0}},
            {"fixed_size_dip",{r.width,r.height}},{"margins_dip",{0,0,0,0}},{"padding_physical",0}};
        if(r.visual){
            const auto& binding=config.document["regions"][i]["visual_binding"];
            previewRelation=binding.at("relation");
            // A wrong live match must not become the new saved cover when reselecting.
            previewOriginal=saved_visual_cover(binding);
        }
        if(kind=="features")previewRelation["mode"]="position_only";
        status();config.stateSequence++;
    }
    void expect_preview(const std::string& token){
        if(stage!="preview"||token!=previewToken||token.empty())throw std::runtime_error("calibration_token_invalid");
        if(config.revision!=previewRevision||config.visualGeneration!=previewGeneration||config.index(id)<0||config.selected!=id)
            throw std::runtime_error("calibration_context_changed");
    }
    void cancel_preview(bool resume=true){
        if(stage!="preview")return;
        stage.clear();previewToken.clear();previewHasReference=false;referencePicking=false;previewRelation=json();
        config.stateSequence++;
        if(config.index(id)<0||config.document["regions"][config.index(id)]["position_mode"]!="visual"){auto old=id;remove_region(old);}
        bool restore=previewResumePending;previewResumePending=false;
        if(resume&&restore&&!id.empty())queue(id,false);
    }
    void interrupt_reference(){
        if(stage!="preview"||!referencePicking)return;
        // Keep the unsaved preview (and any previous reference) available for
        // retry, but release the visibility gate even if capture was lost.
        referencePicking=false;error="reference_selection_interrupted";config.stateSequence++;
    }
    void set_reference(const std::string& token,const json& rect){
        expect_preview(token);
        auto next=checked_reference(rect,previewKind);
        auto cover=previewHasReference?visual_nominal(previewReference,previewRelation):previewOriginal;
        if(previewRelation["mode"]=="position_only")previewRelation=visual_relation_at_cover(previewRelation,next,cover);
        previewReference=next;previewHasReference=true;referencePicking=false;config.stateSequence++;
        error.clear();
    }
    VisualRect checked_reference(const json& rect,const std::string& kind)const{
        visual_array(rect,4,0,100000);VisualRect next{rect[0],rect[1],rect[2],rect[3]};
        if(next.width<8||next.height<8||next.x+next.width>config.viewportWidth+.01||
            next.y+next.height>config.viewportHeight+.01)throw std::runtime_error("reference_outside_client");
        double scale=config.viewportDpi/96.;
        if(kind=="features"&&(std::round(next.width*scale)<12||std::round(next.height*scale)<12))
            throw std::runtime_error("feature_reference_too_small");
        if(kind=="features"&&std::round(next.width*scale)*std::round(next.height*scale)>131072)
            throw std::runtime_error("feature_reference_too_large");
        if(std::ceil((next.width+64)*scale+2)*std::ceil((next.height+64)*scale+2)>524288)
            throw std::runtime_error("visual_roi_budget_exceeded");
        return next;
    }
    void recalibrate_reference(const std::string& selected,const json& rect){
        int index=config.index(selected);
        if(index<0||config.selected!=selected)throw std::runtime_error("region_not_found");
        const auto& entry=config.document["regions"][index];
        if(entry["position_mode"]!="visual")throw std::runtime_error("visual_mode_required");
        const auto& binding=entry.at("visual_binding");
        auto kind=binding.value("tracking_kind",std::string("boundary"));
        auto reference=checked_reference(rect,kind);
        auto relation=binding.at("relation");
        if(relation["mode"]=="position_only")
            relation=visual_relation_at_cover(relation,reference,saved_visual_cover(binding));
        queue(selected,true,rect,relation,kind,binding.value("search_limit_dip",500.));
    }
    void set_preview_relation(const std::string& token,const json& relation){
        expect_preview(token);if(!previewHasReference)throw std::runtime_error("reference_required");
        validate_visual_relation(relation);auto cover=visual_nominal(previewReference,relation);
        if(previewKind=="features"&&relation["mode"]!="position_only")throw std::runtime_error("feature_position_requires_fixed_size");
        if(cover.width<8||cover.height<8)throw std::runtime_error("invalid_visual_cover_size");
        auto clipped=visual_geometry(previewReference,relation,config.viewportWidth,config.viewportHeight,config.viewportDpi);
        if(clipped.cover.width<=0||clipped.cover.height<=0)throw std::runtime_error("cover_outside_client");
        previewRelation=relation;error.clear();config.stateSequence++;
    }
    json preview_state()const{
        if(stage!="preview"||config.index(id)<0)return nullptr;
        json cover=visual_rect_json(previewHasReference?visual_geometry(previewReference,previewRelation,
            config.viewportWidth,config.viewportHeight,config.viewportDpi).cover:previewOriginal);
        return {{"token",previewToken},{"id",id},{"picking",referencePicking},{"tracking_kind",previewKind},{"search_limit_dip",previewSearchLimit},
            {"reference",previewHasReference?json::array({previewReference.x,previewReference.y,previewReference.width,previewReference.height}):json(nullptr)},
            {"cover",cover},{"relation",previewRelation},{"saved_position",config.document["regions"][config.index(id)]["position_mode"]=="visual"}};
    }
    void confirm_preview(const std::string& token){
        expect_preview(token);if(!previewHasReference)throw std::runtime_error("reference_required");
        auto selected=id;auto r=previewReference;auto relation=previewRelation;auto kind=previewKind;auto limit=previewSearchLimit;
        cancel_preview(false);queue(selected,true,json::array({r.x,r.y,r.width,r.height}),relation,kind,limit);
    }
    bool holding()const{return stage=="baseline"||stage=="positive"||stage=="excluded1"||stage=="excluded2";}
    bool needed()const{
        if(process.value)return true;
        if(!stage.empty())return true;
        for(const auto& r:config.document["regions"])if(r["position_mode"]=="visual")return true;
        return false;
    }
    void abort_calibration(){
        exclusive=false;
        if(stage=="preview"){cancel_preview();return;}
        if(!manual||stage.empty())return;
        auto selected=id;int i=config.index(selected);
        bool restore=i>=0&&config.document["regions"][i]["position_mode"]=="visual"&&
            priorManualPhase!=VisualPhase::NeedsManual&&priorManualPhase!=VisualPhase::Unavailable;
        stop();stage.clear();error="calibration_cancelled";
        if(restore){error.clear();queue(selected,false);}
        else {if(priorManualPhase==VisualPhase::NeedsManual)policy.phase=VisualPhase::NeedsManual;status();}
    }
    bool can_retry_calibration()const{
        return stage.empty()&&manual&&!operationError.empty()&&!pendingRelation.is_null()&&config.index(operationOwner)>=0&&config.selected==operationOwner&&
            config.revision==queuedRevision&&config.visualGeneration==queuedGeneration;
    }
    void retry_calibration(){
        if(!can_retry_calibration())throw std::runtime_error("calibration_retry_unavailable");
        auto selected=operationOwner;json r={operationReference.x,operationReference.y,operationReference.width,operationReference.height};auto relation=pendingRelation;
        queue(selected,true,r,relation,operationKind,operationSearchLimit);
    }
    void status(){
        region_status(*this);
    }
    void stop(){
        restore_affinity();
        wake_worker();
        pipe.reset();
        if(process.value&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT){
            // This exact handle is the child we created, never a discovered PID.
            if(WaitForSingleObject(process.value,200)!=WAIT_OBJECT_0){TerminateProcess(process.value,2);WaitForSingleObject(process.value,500);}
        }
        process.reset();job.reset();workerWake.reset();workerPid=0;authenticated=false;connection=0;lastMessage=0;workerStarted=lastDelivered=0;
        pause_all("worker_stopped");task=json();ticket={};taskOwner.clear();
    }
    void fail(const std::string& why){
        restore_affinity();
        if(!stage.empty()||id==operationOwner)operationError=why;
        error=why;stage.clear();task=json();captureReady=false;policy.unavailable(why);status();
        if(reconcile)reconcile();
    }
    void defer_qualification(double now){
        restore_affinity();stage.clear();task=json();savedCalibration=json();captureReady=false;
        policy.pause("waiting_for_target");region_status(*this);
        resumeQualificationAt=now+.5;
        if(reconcile)reconcile();
    }
    void restore_affinity(){
        for(const auto& entry:previousAffinity)if(IsWindow(entry.first)&&process_id(entry.first)==GetCurrentProcessId())
            SetWindowDisplayAffinity(entry.first,entry.second);
        previousAffinity.clear();
    }
    bool component_available()const{
        std::error_code ec;
        if(!executable.empty())return fs::is_regular_file(executable,ec);
        return !python.empty()&&!script.empty()&&fs::is_regular_file(python,ec)&&fs::is_regular_file(script,ec);
    }
    void launch(){
        if(process.value)return;
        if(!component_available())throw std::runtime_error("visual_worker_missing");
        pipe=std::make_unique<PipeServer>(host,0);
        auto wakeName=L"Local\\ChatGPTOverlay.VisualWake."+wide(new_id());
        workerWake.reset(create_user_wake_event(wakeName));
        fs::path program=executable.empty()?python:executable;
        fs::path working=executable.empty()?script.parent_path():executable.parent_path();
        std::wstring command=visual_arg(program.wstring())+(executable.empty()?L" "+visual_arg(script.wstring()):L"")+L" --pipe "+visual_arg(pipe->name)+
            L" --session "+visual_arg(wide(pipe->nonce))+L" --parent-pid "+std::to_wstring(GetCurrentProcessId())+L" --wake-event "+visual_arg(wakeName);
        if(!deps.empty())command+=L" --deps "+visual_arg(deps.wstring());
        LPWCH inherited=GetEnvironmentStringsW();if(!inherited)throw std::runtime_error("worker_environment_failed");
        std::vector<std::wstring> environment;
        for(auto p=inherited;*p;p+=wcslen(p)+1){
            std::wstring entry=p;bool replace=false;
            for(auto key:{L"OPENBLAS_NUM_THREADS=",L"OMP_NUM_THREADS=",L"MKL_NUM_THREADS=",L"NUMEXPR_NUM_THREADS="})
                if(_wcsnicmp(entry.c_str(),key,wcslen(key))==0)replace=true;
            if(!replace)environment.push_back(entry);
        }
        FreeEnvironmentStringsW(inherited);
        for(auto key:{L"OPENBLAS_NUM_THREADS=1",L"OMP_NUM_THREADS=1",L"MKL_NUM_THREADS=1",L"NUMEXPR_NUM_THREADS=1"})environment.push_back(key);
        std::sort(environment.begin(),environment.end(),[](const auto& a,const auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
        std::vector<wchar_t> block;for(const auto& e:environment){block.insert(block.end(),e.begin(),e.end());block.push_back(0);}block.push_back(0);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
        if(!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
            block.data(),working.c_str(),&startup,&child))throw std::runtime_error("visual_worker_launch_failed");
        process.reset(child.hProcess);WinHandle thread(child.hThread);workerPid=child.dwProcessId;pipe->expectedPid=workerPid;
        try{
        job.reset(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job.value||!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||
           !AssignProcessToJobObject(job.value,process.value)){
            throw std::runtime_error("visual_worker_job_failed");
        }
        workerSession=++serial;each_region([&](auto& r){r.policy.bind_session(workerSession);region_status(r);});
        sharedFault=false;workerStarted=taskStarted=visual_now();lastDelivered=0;lastMessage=0;authenticated=false;
        if(ResumeThread(thread.value)==DWORD(-1))throw std::runtime_error("visual_worker_resume_failed");
        }catch(...){
            if(process.value){TerminateProcess(process.value,2);WaitForSingleObject(process.value,500);}
            process.reset();job.reset();pipe.reset();workerWake.reset();workerPid=0;authenticated=false;connection=0;
            workerStarted=lastDelivered=0;throw;
        }
    }
    void queue(const std::string& selected,bool newCalibration,const json& selectedReference=nullptr,const json& selectedRelation=nullptr,const std::string& kind="boundary",double searchLimit=500){
        int i=config.index(selected);if(i<0)throw std::runtime_error("region_not_found");
        if(!stage.empty())throw std::runtime_error("calibration_in_progress");
        size_t count=0;for(const auto& r:config.document["regions"])if(r["position_mode"]=="visual"&&r["id"]!=selected)count++;
        if(count>=4)throw std::runtime_error("visual_region_limit");
        auto r=config.positioned(i);
        if(newCalibration&&r.visual&&selectedReference.is_null())throw std::runtime_error("switch_to_anchor_before_recalibrating");
        auto bindingId=config.document["regions"][i]["visual_binding"].is_null()?std::string():config.document["regions"][i]["visual_binding"]["calibration_id"].get<std::string>();
        auto previous=region(selected);
        if(!newCalibration&&(manualLocks.count(bindingId)||(previous&&previous->policy.phase==VisualPhase::NeedsManual)))throw std::runtime_error("manual_recalibration_required");
        VisualRect nextOriginal{r.x,r.y,r.width,r.height},nextReference=nextOriginal;
        std::string nextCalibration,nextTemplate;
        if(!newCalibration){
            auto binding=config.document["regions"][i]["visual_binding"];if(!visual_asset_valid(config.path,binding))throw std::runtime_error("template_unavailable");
            auto a=binding["reference_rect_dip"];nextReference={a[0],a[1],a[2],a[3]};
            nextCalibration=binding["calibration_id"];
            fs::path file=config.path.parent_path()/L"visual-templates"/(wide(binding["template_id"])+L".vt1");
            auto size=fs::file_size(file);if(size<13||size>131084)throw std::runtime_error("template_unavailable");
            std::ifstream input(file,std::ios::binary);std::vector<BYTE> bytes(static_cast<size_t>(size));
            if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size())||visual_sha256(bytes)!=binding["template_sha256"])
                throw std::runtime_error("template_unavailable");
            nextTemplate=visual_bytes_hex(bytes);
        }
        if(!selectedReference.is_null()){
            visual_array(selectedReference,4,0,100000);validate_visual_relation(selectedRelation);
            nextReference={selectedReference[0],selectedReference[1],selectedReference[2],selectedReference[3]};
            nextOriginal=visual_nominal(nextReference,selectedRelation);
        }
        activate_region(selected);if(newCalibration)priorManualPhase=policy.phase;
        if(!task.is_null()||sharedFault)stop();else pause_all("calibrating");
        trackingKind=newCalibration?kind:config.document["regions"][i]["visual_binding"].value("tracking_kind",std::string("boundary"));
        if(trackingKind!="boundary"&&trackingKind!="features")throw std::runtime_error("invalid_tracking_kind");
        operationKind=trackingKind;
        operationSearchLimit=newCalibration?searchLimit:config.document["regions"][i]["visual_binding"].value("search_limit_dip",500.);
        visual_number(operationSearchLimit,32,500);
        error.clear();manual=newCalibration;captureReady=false;operationOwner=selected;operationError.clear();
        original=nextOriginal;reference=nextReference;operationReference=nextReference;pendingRelation=selectedRelation;
        queuedRevision=config.revision;queuedGeneration=config.visualGeneration;queuedAt=visual_now();stage="queued";
        if(!manual){calibration=nextCalibration;templateHex=std::move(nextTemplate);policy.retry_unavailable();}
        normal.separate=true;
    }
    json context(HWND target){
        RECT c=client_box(target);double scale=GetDpiForWindow(target)/96.;
        int x=std::max(int(c.left),int(std::floor(c.left+(reference.x-32)*scale)));
        int y=std::max(int(c.top),int(std::floor(c.top+(reference.y-32)*scale)));
        int right=std::min(int(c.right),int(std::ceil(c.left+(reference.x+reference.width+32)*scale))+1);
        int bottom=std::min(int(c.bottom),int(std::ceil(c.top+(reference.y+reference.height+32)*scale))+1);
        if(stage.empty()&&trackingKind=="features"){
            if(search.tiles.empty()){
                auto origin=search.confirming?search.candidate:policy.featureOrigin;
                unsigned dims[2]{};
                if(templateHex.size()<24)throw std::runtime_error("invalid_worker_template");
                for(int axis=0;axis<2;axis++)for(int byte=0;byte<4;byte++)
                    dims[axis]|=unsigned(std::stoul(templateHex.substr(8+axis*8+byte*2,2),nullptr,16))<<(byte*8);
                double ratio=GetDpiForWindow(target)/config.document["regions"][config.index(id)]["visual_binding"]["calibration_dpi"].get<double>();
                search.tiles=feature_search_tiles(origin,search.scanning?search.radius(policy.searchLimit):32,scale,c.right-c.left,c.bottom-c.top,
                    feature_scaled_size(dims[0],ratio),feature_scaled_size(dims[1],ratio));
                search.at=0;
            }
            if(search.tiles.empty())throw std::runtime_error("feature_search_area_unavailable");
            auto tile=search.tiles.at(search.at);
            x=c.left+tile.left;y=c.top+tile.top;right=c.left+tile.right;bottom=c.top+tile.bottom;
        }
        if(right<=x||bottom<=y||uint64_t(right-x)*(bottom-y)>524288)throw std::runtime_error("visual_roi_budget_exceeded");
        json handles=json::array();for(const auto& s:surfaces)if(s->valid_window()&&IsWindowVisible(s->hwnd))handles.push_back(reinterpret_cast<uintptr_t>(s->hwnd));
        json allowed=json::array(),calibrations=json::object();size_t count=0;
        each_region([&](auto& r){int i=config.index(r.id);if(i>=0&&config.document["regions"][i]["position_mode"]=="visual"){
            count++;if(!r.policy.locked()){
                allowed.push_back(r.id);calibrations[r.id]=config.document["regions"][i]["visual_binding"]["calibration_id"];
            }
        }});
        return {{"hwnd",reinterpret_cast<uintptr_t>(target)},{"pid",process_id(target)},{"client",{c.left,c.top,c.right-c.left,c.bottom-c.top}},
            {"dpi",GetDpiForWindow(target)},{"roi",{x,y,right-x,bottom-y}},{"generation",config.visualGeneration},
            {"application_id",config.applicationId},{"binding_epoch",config.bindingEpoch},
            {"monitor",reinterpret_cast<uintptr_t>(MonitorFromWindow(target,MONITOR_DEFAULTTONEAREST))},
            {"owner_pid",GetCurrentProcessId()},{"surfaces",handles},{"region_id",id},{"calibration_id",calibration},
            {"cache_regions",allowed},{"cache_calibrations",calibrations},{"cache_budget_pixels",2097152-std::min<size_t>(4,count)*131072}};
    }
    VisualRect boundary(const json& r)const{
        visual_array(r,4,-100000,100000);double scale=grant["dpi"].get<double>()/96.;
        return {(r[0].get<double>()-grant["client"][0].get<double>())/scale,
            (r[1].get<double>()-grant["client"][1].get<double>())/scale,r[2].get<double>()/scale,r[3].get<double>()/scale};
    }
    bool permitted(HWND target,const json& granted,bool positive=false){
        blockedWindow=nullptr;
        if(!available(target)||GetForegroundWindow()!=target)return false;
        auto current=context(target);
        for(auto key:{"hwnd","pid","client","dpi","roi","generation","application_id","binding_epoch","monitor"})if(current[key]!=granted[key])return false;
        struct Check {const json& context;HWND target;bool positive,found=false,blocked=false;HWND blocking=nullptr;};
        Check check{granted,target,positive};
        EnumWindows([](HWND window,LPARAM raw)->BOOL{
            auto& c=*reinterpret_cast<Check*>(raw);
            if(window==c.target){c.found=true;return FALSE;}
            if(!IsWindowVisible(window)||IsIconic(window))return TRUE;
            DWORD cloaked=0;if(FAILED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))){c.blocked=true;c.blocking=window;return FALSE;}
            if(cloaked)return TRUE;
            RECT box{};if(!GetWindowRect(window,&box)){c.blocked=true;c.blocking=window;return FALSE;}
            const auto& r=c.context["roi"];int x=r[0],y=r[1],w=r[2],h=r[3];
            if(std::min(int(box.right),x+w)<=std::max(int(box.left),x)||std::min(int(box.bottom),y+h)<=std::max(int(box.top),y))return TRUE;
            auto handle=reinterpret_cast<uintptr_t>(window);const auto& allowed=c.context["surfaces"];DWORD affinity=0;
            if(process_id(window)==GetCurrentProcessId()&&std::find(allowed.begin(),allowed.end(),handle)!=allowed.end()&&
               (c.positive||(GetWindowDisplayAffinity(window,&affinity)&&affinity==WDA_EXCLUDEFROMCAPTURE)))return TRUE;
            c.blocked=true;c.blocking=window;return FALSE;
        },reinterpret_cast<LPARAM>(&check));
        blockedWindow=check.blocking;return check.found&&!check.blocked;
    }
    void issue(const std::string& op,HWND target){
        grant=context(target);generation=config.visualGeneration;taskStarted=lastTask=visual_now();
        task={{"task_id",++serial},{"operation",op},{"context",grant},{"tracking_kind",trackingKind}};
        if(op=="locate"||op=="watch")task["minimum_sample_t_ns"]=uint64_t(std::ceil(policy.readyAt*1e9));
        taskOwner=id;lastServed=id;
        wake_worker();
        if(op=="calibrate"){
            double scale=grant["dpi"].get<double>()/96.;
            task["selection"]={int(std::round(grant["client"][0].get<double>()+reference.x*scale))-grant["roi"][0].get<int>(),
                int(std::round(grant["client"][1].get<double>()+reference.y*scale))-grant["roi"][1].get<int>(),
                int(std::round(reference.width*scale)),int(std::round(reference.height*scale))};
        }else if(op=="locate"){
            int i=config.index(id);task["template_hex"]=templateHex;
            task["calibration_dpi"]=config.document["regions"][i]["visual_binding"]["calibration_dpi"];
        }
    }
    void positive(HWND target){
        normal.sync(surfaces,target,0,&config);
        int i=config.index(id);
        for(size_t k=0;k<surfaces.size();k++){
            auto& s=*surfaces[k];s.ensure_window();
            if(int(k)==i){
                json p=config.document["regions"][i];p.update({{"position_mode","anchor"},{"x",reference.x},{"y",reference.y},
                    {"width",reference.width},{"height",reference.height},{"anchor_x","left"},{"anchor_y","top"},
                    {"text",""},{"background","#e913bc"},{"opacity",255},{"scroll",false},{"glow",false},{"outline",false}});
                s.release();s.region=Region(p);
            }
        }
        // Render all as independent product Surface windows. Only this explicit
        // capability check shows a temporary solid colour; no polling fallback.
        normal.sync(surfaces,target,0,nullptr);
        for(const auto& s:surfaces)if(s->valid_window()&&IsWindowVisible(s->hwnd)){
            DWORD affinity=0;if(!GetWindowDisplayAffinity(s->hwnd,&affinity))throw std::runtime_error("capture_affinity_query_failed");
            previousAffinity[s->hwnd]=affinity;
            if(!SetWindowDisplayAffinity(s->hwnd,WDA_NONE))throw std::runtime_error("capture_positive_control_failed");
        }
        DwmFlush();stage="positive";issue("positive",target);
    }
    void completed(const json& response,HWND target){
        if(!response.is_object()||response.at("task_id")!=task["task_id"])throw std::runtime_error("worker_task_mismatch");
        json sent=task;task=json();
        if(config.bindingEpoch!=grant["binding_epoch"]||config.applicationId!=grant["application_id"])return;
        if(stage.empty()&&config.visualGeneration!=grant["generation"]){policy.invalidate();return;}
        std::string result=response.at("status");
        lastObservation=json::object();for(auto key:{"status","reason","score","boundary_rect","sample_id","elapsed_ms","difference","cached_regions","retained_pixels","capture_reused","analysis_reused","capture_roi","permission_detail"})
            if(response.contains(key))lastObservation[key]=response[key];
        if(result=="unavailable")throw std::runtime_error(response.value("reason",std::string("worker_error")));
        // A moved capture grant may be discarded without discarding the client
        // geometry. Other permission failures must still pause immediately.
        bool movedGrant=result!="paused"||(response.contains("permission_detail")&&
            response["permission_detail"].value("reason",std::string())=="client_geometry_changed");
        if(stage.empty()&&movedGrant&&available(target)&&GetForegroundWindow()==target){
            RECT box=client_box(target);json b={box.left,box.top,box.right-box.left,box.bottom-box.top};const auto& a=grant["client"];
            if(reinterpret_cast<uintptr_t>(target)==grant["hwnd"]&&process_id(target)==grant["pid"]&&GetDpiForWindow(target)==grant["dpi"]&&
               config.visualGeneration==grant["generation"]&&a[2]==b[2]&&a[3]==b[3]&&(a[0]!=b[0]||a[1]!=b[1])){
                config.set_target_geometry(target);policy.invalidate();return;
            }
        }
        if(result=="paused"){
            if(holding()&&!manual){defer_qualification(visual_now());return;}
            if(stage.empty())refresh.finish(visual_now(),config.visual_check_interval());
            policy.pause("capture_permission_revoked");if(stage.empty()&&trackingKind=="features")search.reset();if(holding())throw std::runtime_error("capture_permission_revoked");status();return;
        }
        bool contextMatches=response.at("context")==grant&&config.visualGeneration==grant["generation"];
        blockedWindow=nullptr;
        bool contextPermitted=contextMatches&&permitted(target,grant,stage=="positive");
        if(!contextPermitted){
            if(holding()&&!manual){defer_qualification(visual_now());return;}
            if(stage.empty())refresh.finish(visual_now(),config.visual_check_interval());
            policy.pause("context_changed");if(stage.empty()&&trackingKind=="features")search.reset();if(holding())throw std::runtime_error("calibration_context_changed");status();return;
        }
        double sampled=response.at("sample_t_ns").get<double>()/1e9,now=visual_now();
        if(!std::isfinite(sampled)||sampled>now||now-sampled>=1)throw std::runtime_error("worker_sample_expired");
        if(response.at("sample_t_ns").get<double>()<sent.value("minimum_sample_t_ns",uint64_t(0)))
            throw std::runtime_error("worker_sample_before_settle");
        lastDelivered=now;
        if(result=="complete"&&(!response.value("unique",false)||!response.value(trackingKind=="features"?"position_checked":"boundary_checked",false)||
            !response.contains("score")||!response["score"].is_number()||
            !std::isfinite(response["score"].get<double>())||response["score"].get<double>()<.84))
            throw std::runtime_error("unconfirmed_worker_boundary");
        if(stage=="baseline"){
            if(manual&&result!="complete")throw std::runtime_error(response.value("reason",std::string("unsupported_region")));
            savedCalibration=response;positive(target);return;
        }
        if(stage=="positive"){
            if(result!="proof"||response.at("difference").get<double>()<2)throw std::runtime_error("capture_positive_control_invisible");
            for(const auto& s:surfaces)if(s->valid_window()){
                if(!SetWindowDisplayAffinity(s->hwnd,WDA_EXCLUDEFROMCAPTURE))throw std::runtime_error("capture_exclusion_failed");
            }
            DwmFlush();stage="excluded1";issue("excluded",target);return;
        }
        if(stage=="excluded1"||stage=="excluded2"){
            if(result!="proof"||response.at("difference").get<double>()>=.5)throw std::runtime_error("capture_pixels_not_clean");
            if(stage=="excluded1"){stage="excluded2";issue("excluded",target);return;}
            if(manual){
                if(config.revision!=queuedRevision)throw std::runtime_error("calibration_revision_changed");
                auto rect=boundary(savedCalibration.at("boundary_rect"));
                VisualTrackingState guard;guard.reference=reference;
                if(!guard.allowed(rect))throw std::runtime_error("calibration_outside_authorized_reference");
                auto bytes=visual_unhex(savedCalibration.at("template_hex"));
                if(bytes.size()<13||visual_sha256(bytes)!=savedCalibration.at("template_sha256"))throw std::runtime_error("invalid_worker_template");
                unsigned w=0,h=0;for(int b=0;b<4;b++){w|=unsigned(bytes[4+b])<<(b*8);h|=unsigned(bytes[8+b])<<(b*8);}
                json relation={{"mode","position_only"},{"anchor_x","left"},{"anchor_y","top"},
                    {"offset_dip",{original.x-rect.x,original.y-rect.y}},{"fixed_size_dip",{original.width,original.height}},
                    {"margins_dip",{0,0,0,0}},{"padding_physical",0}};
                if(!pendingRelation.is_null())relation=pendingRelation["mode"]=="follow_bounds"?pendingRelation:
                    visual_relation_at_cover(pendingRelation,rect,original);
                config.install_visual(id,json::array({rect.x,rect.y,rect.width,rect.height}),
                    grant["dpi"],relation,w,h,std::vector<BYTE>(bytes.begin()+12,bytes.end()),config.revision,trackingKind,operationSearchLimit);
                reference=rect;templateHex=savedCalibration.at("template_hex");
                calibration=config.document["regions"][config.index(id)]["visual_binding"]["calibration_id"];
            }
            qualified.clear();for(const auto& s:surfaces)if(s->valid_window())qualified[s->hwnd]=GetWindowLongPtrW(s->hwnd,GWLP_USERDATA);
            previousAffinity.clear(); // Verified exclusions remain enabled.
            stage.clear();savedCalibration=json();generation=config.visualGeneration;
            policy.bind_session(workerSession);policy.featureRecovery=trackingKind=="features";policy.searchLimit=operationSearchLimit;
            policy.retryTemporaryLoss=true;
            policy.calibrate(reference,grant["dpi"]);search.reset();captureReady=true;
            request_refresh(*this,now);
            auto priorOrigin=config.featureOrigins.find(id);
            if(policy.featureRecovery&&priorOrigin!=config.featureOrigins.end()&&priorOrigin->second.first==calibration)
                policy.featureOrigin=priorOrigin->second.second;
            policy.permit(now);error.clear();operationError.clear();status();
            if(reconcile)reconcile();return;
        }
        auto rect=result=="complete"?boundary(response.at("boundary_rect")):VisualRect{};
        auto kind=result=="complete"?VisualObservation::Complete:result=="unchanged"?VisualObservation::Unchanged:
            result=="changed"?VisualObservation::Changed:VisualObservation::NotFound;
        if(trackingKind=="features"&&ticket.complete&&search.scanning){
            bool consumed=policy.observe(ticket,response.at("sample_id"),sampled,now,VisualObservation::NotFound,{},true);
            if(!consumed)return;
            budgetMs+=response.value("elapsed_ms",0.);checks++;audits++;
            if(result=="complete"){
                searchDiagnostic={{"candidate",visual_rect_json(rect)},{"origin",visual_rect_json(policy.featureOrigin)},
                    {"allowed",policy.allowed(rect)},{"prior",visual_rect_json(search.candidate)},{"already_found",search.found},
                    {"radius",search.radius(policy.searchLimit)},{"tile",search.at},{"tiles",search.tiles.size()}};
                if(policy.allowed(rect)){
                    if(search.found&&!policy.consistent(search.candidate,rect))search.ambiguous=true;
                    else {search.found=true;search.candidate=rect;}
                }
            }else if(response.value("reason",std::string())=="ambiguous-features")search.ambiguous=true;
            search.at++;
            if(search.at<search.tiles.size()){policy.reason="searching_"+std::to_string(int(search.radius(policy.searchLimit)));status();return;}
            if(search.ambiguous){search.retry(now);policy.reason="search_ambiguous";}
            else if(search.found){
                search.scanning=false;search.confirming=true;search.tiles.clear();search.at=0;policy.hasCandidate=false;policy.reason="confirming";
            }else if(search.radius(policy.searchLimit)<policy.searchLimit){search.next_level();policy.reason="searching_"+std::to_string(int(search.radius(policy.searchLimit)));}
            else {search.retry(now);policy.reason="search_not_found";}
            status();return;
        }
        bool accepted=policy.observe(ticket,response.at("sample_id"),sampled,now,
            kind,rect,true);
        if(trackingKind=="features"&&accepted){
            if(kind==VisualObservation::NotFound||(!policy.allowed(rect)&&kind==VisualObservation::Complete)){search.reset();policy.reason="searching_32";}
        }
        if(accepted){checks++;if(ticket.complete)audits++;if(response.value("capture_reused",false))reusedChecks++;}
        if(accepted&&policy.visible())publish_observation(sampled);
        budgetMs+=response.value("elapsed_ms",0.);status();
    }
    void publish_observation(double sampled){
        std::string why;
        bool published=config.publish_visual_boundary(id,calibration,generation,policy.boundary,sampled,&why);
        if(published||why=="visual_cover_outside_client"){
            refresh.finish(visual_now(),config.visual_check_interval());
            presentationReason=published?"":why;
            presentationGeneration=config.visualGeneration;
            if(trackingKind=="features"){
                policy.featureOrigin=policy.boundary;
                search=FeatureSearch{};search.scanning=false;
            }
            return;
        }
        presentationReason.clear();
        if(why=="template_unavailable"||why=="visual_result_invalid"||why=="visual_region_limit"){
            fail(why);return;
        }
        // Stale geometry/time and temporary clipping are not worker failures.
        // Do not advance the policy origin when the publisher rejected it.
        policy.pause(why);search.reset();
        auto origin=config.featureOrigins.find(id);
        policy.featureOrigin=origin!=config.featureOrigins.end()&&origin->second.first==calibration?origin->second.second:reference;
    }
    void route_result(const json& response,HWND target){
        auto resultId=response.at("task_id").get<uint64_t>();
        if(!task.is_null()&&response["task_id"]==task["task_id"]){
            if(region(taskOwner)&&config.index(taskOwner)>=0){
                activate_region(taskOwner);
                try{completed(response,target);}catch(const std::exception& e){fail(e.what());}
            }else task=json();
        }else if(resultId>serial)throw std::runtime_error("unsolicited_worker_result");
    }
    json dispatch(const PipeServer::Ticket& message,HWND target){
        const auto& r=message.request;auto request=r.at("request_id").get<uint64_t>();
        json reply={{"ok",false},{"request_id",request}};
        try{
            if(r.at("protocol")!="visual-worker-1"||r.at("session")!=pipe->nonce||request<=lastMessage)throw std::runtime_error("invalid_worker_session");
            if(!authenticated){
                if(r.at("op")!="hello"||message.clientPid!=workerPid)throw std::runtime_error("worker_handshake_required");
                if(!r.contains("capabilities")||!r["capabilities"].is_array()||
                    std::find(r["capabilities"].begin(),r["capabilities"].end(),"multi_region_v1")==r["capabilities"].end())
                    throw std::runtime_error("visual_worker_component_mismatch");
                if(std::find(r["capabilities"].begin(),r["capabilities"].end(),"feature_position_v1")==r["capabilities"].end())
                    throw std::runtime_error("visual_worker_component_mismatch");
                authenticated=true;connection=message.connection;
            }else if(message.connection!=connection||r.at("op")!="next")throw std::runtime_error("worker_connection_changed");
            lastMessage=request;
            if(r.contains("result")&&!r["result"].is_null()){
                route_result(r["result"],target);
            }
            reply["ok"]=true;
            if(stage.empty()&&task.is_null()&&windowPermitted)pump(target,visual_now());
            if(!task.is_null())reply["task"]=task;else reply["wait_ms"]=timer_period();
        }catch(const std::exception& e){fail_all(e.what());reply["stop"]=true;reply["ok"]=true;}
        return reply;
    }
    void synchronize_regions(){
        std::set<std::string> enabled;size_t count=0;
        for(const auto& entry:config.document["regions"])if(entry["position_mode"]=="visual"){
            auto key=entry["id"].get<std::string>();
            if(++count>4){
                json value={{"state","UNAVAILABLE"},{"reason","visual_region_limit"},{"generation",config.visualGeneration}};
                if(config.visualRuntimeStates[key]!=value){config.visualRuntimeStates[key]=value;config.visualPositions.erase(key);config.stateSequence++;}
                continue;
            }
            enabled.insert(key);
            if(!region(key)){VisualRegionContext r;r.id=key;parked.emplace(key,std::move(r));}
            auto r=region(key);auto cal=entry["visual_binding"]["calibration_id"].get<std::string>();
            if(r->calibration.empty())r->calibration=cal;
            if(manualLocks.count(cal)){r->policy.phase=VisualPhase::NeedsManual;r->policy.reason="reselect_required";}
        }
        std::vector<std::string> removed;
        each_region([&](auto& r){
            bool pending=r.id==id&&manual&&!error.empty()&&!pendingRelation.is_null();
            if(!enabled.count(r.id)&&!pending)removed.push_back(r.id);
        });
        for(const auto& key:removed)remove_region(key);
        normal.separate=!enabled.empty()||!stage.empty();
    }
    bool surface_permission(HWND target){
        auto c=context(target);const auto& a=c["roi"];RECT roi{a[0],a[1],a[0].get<LONG>()+a[2].get<LONG>(),a[1].get<LONG>()+a[3].get<LONG>()};
        for(const auto& s:surfaces)if(s->valid_window()&&IsWindowVisible(s->hwnd)){
            RECT box{},overlap{};GetWindowRect(s->hwnd,&box);if(!IntersectRect(&overlap,&roi,&box))continue;
            DWORD affinity=0;
            if(!qualified.count(s->hwnd)||qualified[s->hwnd]!=GetWindowLongPtrW(s->hwnd,GWLP_USERDATA)||
                !GetWindowDisplayAffinity(s->hwnd,&affinity)||affinity!=WDA_EXCLUDEFROMCAPTURE){fail("capture_surface_changed_retry_required");return false;}
        }
        return permitted(target,c);
    }
    void pump(HWND target,double now){
        if(exclusive||!stage.empty()||!task.is_null()||!windowPermitted)return;
        if(config.translatedAt>0&&now-config.translatedAt<.15)return;
        // Initial qualification is serialized. A region that fails it is locked
        // independently, so the next eligible region can still be qualified.
        std::vector<std::string> keys;
        each_region([&](auto& r){int i=config.index(r.id);if(i>=0&&config.document["regions"][i]["position_mode"]=="visual")keys.push_back(r.id);});
        std::sort(keys.begin(),keys.end());
        auto split=std::upper_bound(keys.begin(),keys.end(),lastServed);std::rotate(keys.begin(),split,keys.end());
        for(const auto& key:keys){
            auto r=region(key);if(!r||r->policy.locked()||r->captureReady)continue;
            if(now<resumeQualificationAt)continue;
            activate_region(key);
            try{queue(key,false);}catch(const std::exception& e){fail(e.what());}
            return;
        }
        if(now-budgetAt>=1){budgetAt=now;budgetMs=0;}
        if(budgetMs>=100)return;
        for(const auto& key:keys){
            auto r=region(key);if(!r||r->policy.locked()||!r->captureReady)continue;
            if(!r->refresh.due(now,config.visual_check_interval()))continue;
            if(!r->refresh.active){
                r->refresh.start(now);r->policy.pause("periodic_check");r->search.reset();
            }
            if(now>=r->refresh.deadline){
                r->refresh.finish(now,config.visual_check_interval());r->policy.pause("check_deferred");region_status(*r);continue;
            }
            if(r->trackingKind=="features"&&now<r->search.retryAt)continue;
            bool following=r->policy.phase==VisualPhase::Following;
            // Follow the common quarter-second cadence rather than retaining
            // each region's initial phase offset. Still only one task per slot.
            if(following&&std::floor(now*4)<=std::floor(r->lastTask*4)&&now-r->policy.lastAudit<1)continue;
            if(!following&&now-r->lastAttempt<(r->policy.phase==VisualPhase::Verifying?.10:.25))continue;
            activate_region(key);lastAttempt=now;
            if(trackingKind=="features"&&search.scanning){
                if(!search.started)search.started=now;
                if(now-search.started>20){search.retry(now);policy.reason="search_not_found";status();continue;}
            }
            bool surfaceAllowed=false;
            try{surfaceAllowed=surface_permission(target);}
            catch(const std::exception& e){
                if(trackingKind!="features"||std::string(e.what())!="feature_search_area_unavailable")throw;
                policy.pause("search_not_found");search.retry(now);status();continue;
            }
            if(!surfaceAllowed){
                refresh.finish(now,config.visual_check_interval());
                if(!policy.locked()){policy.pause("roi_occluded");status();}continue;
            }
            if(!process.value){
                try{launch();}catch(const std::exception& e){fail_all(e.what());return;}
            }
            if(!authenticated)return;
            // The refresh scheduler already waited for geometry to settle.
            // Keep the calibration proof's separate delay, without adding it
            // again to every ordinary refresh round.
            policy.bind_session(workerSession);policy.dpi=GetDpiForWindow(target);policy.permit(now,0);policy.tick(now,true);status();
            if(policy.phase!=VisualPhase::Verifying&&policy.phase!=VisualPhase::Following)continue;
            ticket=policy.begin(now,true,following,true);
            if(ticket.request){issue(ticket.complete?"locate":"watch",target);return;}
        }
    }
    void tick(HWND target,bool editing){
        double now=visual_now();
        windowPermitted=!editing&&available(target)&&GetForegroundWindow()==target;
        if(stage.empty()&&handledTranslation!=config.translationSerial){
            handledTranslation=config.translationSerial;
            each_region([&](auto& r){r.policy.invalidate();if(!r.policy.visible())r.search.reset();});
            task=json();ticket={};taskOwner.clear();
        }
        if(stage=="preview"){
            if(config.revision!=previewRevision||config.visualGeneration!=previewGeneration||config.index(id)<0||config.selected!=id){
                cancel_preview();error="calibration_context_changed";
                operationError="calibration_context_changed";
                if(reconcile)reconcile();
            }
            return;
        }
        if(process.value&&WaitForSingleObject(process.value,0)==WAIT_OBJECT_0){
            bool fault=!sharedFault;stop();if(needed()&&fault)fail_all("visual_worker_exited");
        }
        if(process.value&&!authenticated&&now-workerStarted>=2.5){
            fail_all("visual_worker_handshake_timeout");stop();return;
        }
        if(exclusive&&stage.empty())return;
        if(stage=="queued"){
            if(!manual&&config.index(id)>=0&&config.document["regions"][config.index(id)]["visual_binding"]["calibration_id"]==calibration)
                queuedRevision=config.revision;
            if(config.revision!=queuedRevision||config.visualGeneration!=queuedGeneration){
                if(manual)fail("calibration_context_changed");else defer_qualification(now);
                return;
            }
            if(editing)return;
            if(now-queuedAt>5){
                if(manual)fail("calibration_wait_expired");else defer_qualification(now);
                return;
            }
            if(!available(target)||GetForegroundWindow()!=target)return;
            policy.pause("calibrating");status();
            normal.hide();for(auto& s:surfaces)s->hide();
            try{launch();}catch(const std::exception& e){fail_all(e.what());return;}
            stage="baseline";issue(manual?"calibrate":"baseline",target);return;
        }
        if(holding()){
            if(editing||!available(target)||GetForegroundWindow()!=target||config.visualGeneration!=generation){
                if(manual)fail("calibration_context_changed");else defer_qualification(now);
                return;
            }
            if(now-taskStarted>2.5)fail("visual_worker_timeout");return;
        }
        synchronize_regions();bool runnable=false;
        each_region([&](auto& r){
            int i=config.index(r.id);if(i<0||config.document["regions"][i]["position_mode"]!="visual")return;
            if(!windowPermitted||r.generation!=config.visualGeneration){
                bool layoutChanged=r.generation!=config.visualGeneration;
                if(layoutChanged)request_refresh(r,now);
                if(r.trackingKind=="features"){
                    r.search.reset();auto origin=config.featureOrigins.find(r.id);
                    r.policy.featureOrigin=origin!=config.featureOrigins.end()&&origin->second.first==r.calibration?origin->second.second:r.reference;
                }
                r.policy.pause(!windowPermitted?"target_not_available":"geometry_changed");r.generation=config.visualGeneration;
            }
            r.policy.tick(now,true);
            if(r.policy.phase==VisualPhase::Paused&&r.policy.reason=="freshness_expired"&&budgetMs>=100)r.policy.reason="compute_budget_exceeded";
            region_status(r);if(!r.policy.locked()&&windowPermitted)runnable=true;
        });
        if(!runnable){
            if(!idleSince)idleSince=now;
            if(process.value&&now-idleSince>=5)stop();return;
        }
        idleSince=0;
        if(!task.is_null()){
            if(now-taskStarted>=1){fail_all("visual_worker_timeout");}return;
        }
        pump(target,now);
        bool permit=false;each_region([&](auto& r){if(r.policy.permitted&&!r.policy.locked())permit=true;});
        bool pending=false;each_region([&](auto& r){if(r.refresh.active||r.refresh.requested)pending=true;});
        if(process.value&&task.is_null()&&(!permit||(config.visual_check_interval()==0&&!pending))&&capture_idle_expired(now))stop();
    }
};
