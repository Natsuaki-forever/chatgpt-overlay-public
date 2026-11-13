#include "render_tests.hpp"
#include "config_tests.hpp"
#include "protocol.hpp"
#include "edit_canvas.hpp"
#include "system.hpp"
#include "group_surfaces.hpp"
#include "visual_runtime.hpp"
#include "visual_preview_tests.hpp"
#include "visual_search_tests.hpp"
#include "visual_retirement_tests.hpp"
#include "application_profiles.hpp"
#include <utility>


struct Engine {
    Renderer renderer;
    HWND host=nullptr,target=nullptr;
    bool diagnostic=false;
    std::vector<std::unique_ptr<Surface>> surfaces;
    NormalSurfaces normal{renderer};
    std::vector<HWINEVENTHOOK> hooks;
    fs::path samplePath;
    bool pending=false;
    bool targetDestroyed=false;
    bool cancelGesturePending=false;
    bool visualCaptureInvalid=false;
    uint64_t gestureCancelSequence=0;
    bool targetPinned=false;
    bool animationTimer=false;
    HANDLE frameTimer=nullptr;
    bool highResolutionTimer=false;
    DWORD targetPid=0;
    unsigned errors=0;
    std::unique_ptr<ConfigState> configState;
    std::unique_ptr<ApplicationProfiles> applications;
    std::unique_ptr<ApplicationPicker> applicationPicker;
    WindowIdentity targetIdentity,pendingApplicationWindow;
    std::vector<WindowIdentity> editorApplicationWindows;
    std::string pendingApplication,pendingDeletion;
    bool independentWindows=false;
    bool regionScopes=false;
    bool applicationActionRunning=false;
    std::string windowProfile,pendingWindowProfile;
    struct WindowProfileBinding { WindowIdentity identity; std::string application, profile; };
    std::map<HWND,WindowProfileBinding> windowBindings;
    bool pendingApplicationPick=false,pendingApplicationRemove=false,resumeApplicationEditor=false;
    uint64_t bindingEpoch=0;
    std::map<std::string,std::set<std::string>> applicationManualLocks;
    std::set<std::string> applicationsAwaitingWindowChoice;
    std::map<std::string,std::vector<WindowIdentity>> reconnectBlockers;
    std::vector<WindowIdentity> targetSiblings;
    std::unique_ptr<SessionProtocol> protocol;
    std::unique_ptr<PipeServer> pipe;
    std::unique_ptr<VisualRuntime> visual;
    std::unique_ptr<EditCanvas> editCanvas;
    enum class CanvasMode {Idle, Once, Free, Cover};
    CanvasMode canvasMode=CanvasMode::Idle;
    WinHandle editorProcess;
    DWORD editorPid=0;
    double retiredEditorCpu=0;
    HWND editorHwnd=nullptr;
    fs::path editorScript,pythonExecutable,editorExecutable;
    fs::path visualPython,visualScript,visualDeps;
    bool editing=false,editorVisible=false,simulateFocus=false;
    bool editorOwnsShow=false;
    bool calibrationEditorSuspended=false;
    bool expectedEditorExit=false;
    bool shutdownRequested=false;
    bool sessionSavePending=false;
    std::unique_ptr<NativeShell> shell;
    DWORD diagnosticTargetPid=0;
    std::string pendingNotice;
    uint64_t lastRelated=0,finishSequence=0,focusTextSequence=0;
    uint64_t editorRestoreSequence=0;
    bool reopenEditor=false;
    json editorState=json::object();
    std::string saveError;
    std::string targetIssue, saveIssue, renderIssue, pickIssue, editorIssue;
    std::string calibrationResult,calibrationResultOwner;
    std::string renderError,pickError,pickId,pickSession;
    std::string pickField="background";
    POINT pickPoint{};
    bool pickWaiting=false;
    void report_issue(std::string& active, const std::string& code){
        if(configState&&active!=code)update_native_issue(configState->path.parent_path()/L"diagnostics",active,code);
    }
    void report_save_issue(){report_issue(saveIssue,saveError.empty()?"":"autosave_failed");}
    void begin_editing();
    void end_editing();
    void reconcile(bool synchronize=true);
    void hide_editing_canvas();
    void editor_tick();
    void edit_frame();
    bool canvas_input()const{
        return editing&&((!applications&&!(editCanvas&&editCanvas->coverDrag))||canvasMode!=CanvasMode::Idle||pickWaiting||
            (editCanvas&&editCanvas->picking)||(visual&&visual->stage=="preview"&&visual->referencePicking));
    }
    bool canvas_display()const{return canvas_input()||
        (editing&&((visual&&visual->stage=="preview")||(editCanvas&&editCanvas->coverDrag)));}
    void set_canvas_mode(CanvasMode mode);
    void request_finish();
    bool editing_visible();
    json editor_operation(const std::string&,const json&,DWORD);
    LRESULT canvas_message(HWND,UINT,WPARAM,LPARAM);
    void complete_pick();
    void request_quit();
    void system_command(UINT command);
    void open_or_restore_editor();
    std::unique_ptr<VisualRuntime> make_visual(ConfigState& state);
    void bind_window(const WindowIdentity& identity);
    void forget_sibling(HWND window){
        targetSiblings.erase(std::remove_if(targetSiblings.begin(),targetSiblings.end(),
            [window](const auto& sibling){return sibling.window==window;}),targetSiblings.end());
    }
    void note_sibling(HWND window){
        if(!applications||!target||targetDestroyed||window==target||!available(target,true)||!applications->accepts(window,true))return;
        auto identity=WindowIdentity::read(window);
        if(!applications->matches(applications->app(applications->active()),identity))return;
        auto sibling=std::find_if(targetSiblings.begin(),targetSiblings.end(),
            [window](const auto& candidate){return candidate.window==window;});
        if(sibling==targetSiblings.end())targetSiblings.push_back(std::move(identity));
        else *sibling=std::move(identity);
    }
    bool awaiting_window_choice()const{
        return applications&&applicationsAwaitingWindowChoice.count(applications->active());
    }
    bool application_action_pending()const{
        return pendingApplicationPick||pendingApplicationRemove||!pendingDeletion.empty()||!pendingApplication.empty()||pendingApplicationWindow.window;
    }
    void queue_application(const std::string& id,const WindowIdentity& identity={},bool pick=false,bool remove=false,
                           const std::string& deletion={},const std::string& profile={});
    bool profile_bound_elsewhere(const std::string& app,const std::string& profile,HWND window)const{
        return std::any_of(windowBindings.begin(),windowBindings.end(),[&](const auto& entry){
            const auto& binding=entry.second;
            return entry.first!=window&&binding.application==app&&binding.profile==profile&&binding.identity.current();
        });
    }
    bool archived_window_profile(const std::string& app,const std::string& profile)const{
        return (app!=applications->active()||profile!=windowProfile)&&!profile_bound_elsewhere(app,profile,nullptr);
    }
    void delete_archived_window_profile(const std::string& profile){
        delete_archived_window_profile(applications->active(),profile);
    }
    void delete_archived_window_profile(const std::string& app,const std::string& profile){
        if(app.empty()||!archived_window_profile(app,profile))throw std::runtime_error("invalid_target");
        applications->delete_window_profile(app,profile,[&]{
            if(!regionScopes||!fs::exists(applications->scoped_config_path(app)))return;
            if(app==applications->active()&&configState->persistent){
                configState->remove_region_scope(profile);
                configState->save();
            }else{
                ConfigState scoped(applications->scoped_config_path(app),true,"archive");
                if(scoped.remove_region_scope(profile))scoped.save();
            }
        });
        applicationManualLocks.erase(app+"/"+profile);
    }
    void finish_application_action();
    void visual_tick(){
        if(!visual)return;
        if(applicationPicker){KillTimer(host,11);return;}
        if(visualCaptureInvalid){
            visualCaptureInvalid=false;
            if(visual->needed())visual->fail_all("capture_environment_changed_retry_required");
        }
        try{visual->tick(target,canvas_display()&&!calibrationEditorSuspended);}
        catch(const std::exception& e){visual->fail(e.what());}
        if(calibrationEditorSuspended&&visual->stage.empty()){
            calibrationEditorSuspended=false;visual->exclusive=false;configState->stateSequence++;
            calibrationResultOwner=visual->operationOwner;
            if(visual->operationError.empty()){
                calibrationResult="saved";saveError.clear();report_save_issue();
                if(!visual->manual)request_finish();
            }else{
                calibrationResult="failed";
                record_error("visual calibration: "+visual->operationError);
            }
            if(shell)shell->calibration_status(L"");
            PostMessageW(host,WM_APP+1,0,0);
        }
        if(visual->needed()&&visual->timer_needed())SetTimer(host,11,visual->timer_period(),nullptr);
        else KillTimer(host,11);
    }
    bool editor_window_valid()const{return IsWindow(editorHwnd)&&process_id(editorHwnd)==editorPid;}
    void failed(Surface& surface,const std::exception& error){
        errors++;surface.release();surface.hide();
        record_error(error.what());
    }
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    double seconds() const {return std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();}
    void animate(){
        if(calibrationEditorSuspended||(visual&&visual->holding()))return;
        if(!available(target))return;
        if(canvas_display()){if(editorVisible)edit_frame();return;}
        try{normal.animate(surfaces,target,seconds(),configState.get());report_issue(renderIssue,"");}
        catch(const std::exception& error){normal.hide();for(auto& surface:surfaces)surface->hide();errors++;record_error(error.what());report_issue(renderIssue,"render_failed");}
        if(!normal.active(surfaces)){CancelWaitableTimer(frameTimer);animationTimer=false;}
    }
    static Engine* instance;
    static void CALLBACK event(HWINEVENTHOOK,DWORD kind,HWND hwnd,LONG object,LONG child,DWORD,DWORD) {
        if(instance&&kind==EVENT_SYSTEM_DESKTOPSWITCH)instance->visualCaptureInvalid=true;
        if(instance&&instance->target&&hwnd==instance->target&&kind==EVENT_OBJECT_DESTROY&&object==0&&child==0)instance->targetDestroyed=true;
        if(instance&&object==0&&child==0&&hwnd!=instance->target){
            if(kind==EVENT_OBJECT_DESTROY){
                instance->forget_sibling(hwnd);
                instance->windowBindings.erase(hwnd);
            }
            else if(kind==EVENT_OBJECT_SHOW||kind==EVENT_SYSTEM_FOREGROUND){
                try{instance->note_sibling(hwnd);}catch(const std::exception& error){record_error(error.what());}
            }
        }
        if(instance&&hwnd==instance->target&&object==0&&child==0&&
            (kind==EVENT_SYSTEM_MINIMIZESTART||kind==EVENT_OBJECT_HIDE||kind==EVENT_OBJECT_DESTROY))instance->cancelGesturePending=true;
        if (instance && (kind==EVENT_SYSTEM_FOREGROUND || kind==EVENT_SYSTEM_DESKTOPSWITCH || (hwnd==instance->target && object==0 && child==0)) && !instance->pending) {
            instance->pending=true;PostMessageW(instance->host,WM_APP+1,0,0);
        }
    }
    void sample() {
        if (samplePath.empty()) return;
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
        DWORD handles=0;GetProcessHandleCount(GetCurrentProcess(),&handles);
        FILETIME a,b,k,u;GetProcessTimes(GetCurrentProcess(),&a,&b,&k,&u);
        auto seconds=[](FILETIME f){return (double(ULONGLONG(f.dwHighDateTime)<<32)+f.dwLowDateTime)/1e7;};
        json out={{"pid",GetCurrentProcessId()},{"wall_time",std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()},
                  {"working",memory.WorkingSetSize},{"private",memory.PrivateUsage},{"cpu_seconds",seconds(k)+seconds(u)},
                  {"handles",handles},{"gdi",GetGuiResources(GetCurrentProcess(),0)},{"user_objects",GetGuiResources(GetCurrentProcess(),1)},
                  {"editing",editing},{"editor_pid",editorPid},{"regions",surfaces.size()},{"surface_count",normal.plan.size()}};
        if(applications){out["active_application"]=applications->active();out["binding_epoch"]=bindingEpoch;
            out["window_profile"]=windowProfile;
            out["target_hwnd"]=reinterpret_cast<uintptr_t>(target);out["application_pick_active"]=bool(applicationPicker);
            out["awaiting_window_choice"]=awaiting_window_choice();
            out["canvas_input"]=canvas_input();
            out["application_switch_pending"]=application_action_pending();}
        size_t pixels=0;for(auto& s:surfaces)if(IsWindowVisible(s->hwnd))pixels+=s->width*s->height;
        for(auto& window:normal.windows)if(window&&window->valid()&&IsWindowVisible(window->hwnd))pixels+=size_t(window->image.width)*window->image.height;
        if(editCanvas&&editCanvas->valid()&&IsWindowVisible(editCanvas->hwnd))pixels+=editCanvas->pixels();
        out["canvas_pixels"]=pixels;
        unsigned long long frames=0;double draw=0,blend=0,upload=0;
        for(auto& s:surfaces){frames+=s->frames;draw+=s->drawMilliseconds;blend+=s->blendMilliseconds;upload+=s->uploadMilliseconds;}
        out["frames"]=frames;out["draw_ms"]=draw;out["blend_ms"]=blend;out["upload_ms"]=upload;
        out["errors"]=errors;
        out["high_resolution_timer"]=highResolutionTimer;
        out["cache_bytes"]=renderer.cachedBytes;
        out["surface_bytes"]=renderer.surfaceBytes;
        out["gesture_cancel_sequence"]=gestureCancelSequence;
        if(configState){out["selected_id"]=configState->selected;out["state_revision"]=configState->revision;out["pending_save"]=configState->revision>configState->persistedRevision;}
        if(diagnostic&&configState&&visual){
            out["tracking_states"]=configState->tracking_states();out["effective_rects"]=configState->effective_rects();out["visual_error"]=visual->error;
            out["visual_stage"]=visual->stage;out["target_foreground"]=GetForegroundWindow()==target;
            out["visual_regions"]=visual->diagnostics();
            out["visual_observation"]=visual->lastObservation;
            out["calibration_editor_suspended"]=calibrationEditorSuspended;
        }
        out["compositor"]=renderer.cpuComposite?"cached_dib":"d2d_wic";
        out["engine_working"]=memory.WorkingSetSize;out["engine_private"]=memory.PrivateUsage;
        out["editor_working"]=0;out["editor_private"]=0;out["cpu_seconds"]=out["cpu_seconds"].get<double>()+retiredEditorCpu;
        if(editorProcess.value){
            FILETIME created{},ended{},kernel{},user{};
            if(GetProcessTimes(editorProcess.value,&created,&ended,&kernel,&user))
                out["cpu_seconds"]=out["cpu_seconds"].get<double>()+seconds(kernel)+seconds(user);
            if(WaitForSingleObject(editorProcess.value,0)==WAIT_TIMEOUT){
                PROCESS_MEMORY_COUNTERS_EX editorMemory{};editorMemory.cb=sizeof(editorMemory);DWORD editorHandles=0;
                if(GetProcessMemoryInfo(editorProcess.value,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&editorMemory),sizeof(editorMemory))){
                    out["editor_working"]=editorMemory.WorkingSetSize;out["editor_private"]=editorMemory.PrivateUsage;
                    out["working"]=memory.WorkingSetSize+editorMemory.WorkingSetSize;out["private"]=memory.PrivateUsage+editorMemory.PrivateUsage;
                    GetProcessHandleCount(editorProcess.value,&editorHandles);out["handles"]=handles+editorHandles;
                    out["gdi"]=out["gdi"].get<DWORD>()+GetGuiResources(editorProcess.value,0);
                    out["user_objects"]=out["user_objects"].get<DWORD>()+GetGuiResources(editorProcess.value,1);
                }else out["aggregation_complete"]=false;
            }
        }
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(snapshot!=INVALID_HANDLE_VALUE){
            PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
            DWORD threads=0;
            if(Process32FirstW(snapshot,&entry))do{
                if(entry.th32ProcessID==GetCurrentProcessId()||(editorPid&&entry.th32ProcessID==editorPid))threads+=entry.cntThreads;
            }while(Process32NextW(snapshot,&entry));
            out["threads"]=threads;
            CloseHandle(snapshot);
        }
        std::ofstream(samplePath,std::ios::app)<<out.dump()<<'\n';
    }
    void sync() {
        pending=false;
        // Preserve the transition even if Windows has already restored the
        // target by the time coalesced events reach this message loop.
        if(cancelGesturePending){
            cancelGesturePending=false;
            if(editCanvas&&editCanvas->coverDrag&&editCanvas->coverDrag->placing)editorRestoreSequence++;
            set_canvas_mode(CanvasMode::Idle);hide_editing_canvas();
        }
        if(targetDestroyed||(applications&&target&&!targetIdentity.current())){
            if(applications){
                windowBindings.erase(target);
                auto active=applications->active();
                std::vector<WindowIdentity> siblings;
                for(const auto& window:targetSiblings)
                    if(window.current()&&applications->accepts(window.window,true))siblings.push_back(window);
                if(!siblings.empty()){
                    applicationsAwaitingWindowChoice.insert(active);
                    if(regionScopes)reconnectBlockers[active]=std::move(siblings);
                    if(shell)shell->notice(L"目标窗口已关闭，请在应用菜单中选择要绑定的窗口。");
                }
                bind_window({});
            }
            else{
                if(configState){configState->confirmedBoundaries.clear();configState->featureOrigins.clear();configState->invalidate_visual();}
                target=nullptr;targetDestroyed=false;targetPinned=false;
            }
        }
        if(!applications&&target&&targetPid&&process_id(target)!=targetPid){target=nullptr;targetPinned=false;}
        if(applications){
            if(regionScopes&&target&&!available(target,true)&&!applicationPicker&&
                !applicationActionRunning&&!application_action_pending()){
                auto active=applications->active();
                auto candidates=applications->candidates(active,true);
                if(candidates.size()==1&&!IsIconic(candidates.front().window)&&
                    std::none_of(targetSiblings.begin(),targetSiblings.end(),[&](const auto& sibling){
                        return sibling.window==candidates.front().window&&sibling.current();
                    })){
                    // A tray app can leave its old HWND alive and create a new main window.
                    // Treat a pre-existing sibling as a separate window as before.
                    queue_application(active,candidates.front());
                }
            }
            if(!target&&!applicationPicker&&!applicationActionRunning&&!application_action_pending()&&!applications->active().empty()){
                auto active=applications->active();
                auto blockers=reconnectBlockers.find(active);
                if(blockers!=reconnectBlockers.end()&&std::none_of(blockers->second.begin(),blockers->second.end(),[this](const auto& window){
                    return window.current()&&applications->accepts(window.window,true);
                })){
                    reconnectBlockers.erase(blockers);
                    applicationsAwaitingWindowChoice.erase(active);
                }
                auto candidates=awaiting_window_choice()?std::vector<WindowIdentity>{}:applications->candidates(active);
                if(independentWindows){
                    if(candidates.size()==1)queue_application(active,candidates.front());
                    else if(!windowProfile.empty())queue_application(active);
                }else if(candidates.size()==1)bind_window(candidates.front());
            }
        }else if (!diagnostic) {
            HWND fg=GetForegroundWindow();
            if (!targetPinned&&fg!=target && available(fg) && !GetWindow(fg,GW_OWNER) && is_chatgpt(fg)) target=fg;
            if (!IsWindow(target)) target=find_target();
        }
        if(applicationPicker){
            normal.hide();for(auto& surface:surfaces)surface->hide();
            CancelWaitableTimer(frameTimer);animationTimer=false;return;
        }
        targetPid=process_id(target);
        report_issue(targetIssue,available(target)?"":"target_waiting");
        if(configState){
            float oldWidth=configState->viewportWidth,oldHeight=configState->viewportHeight,oldDpi=configState->viewportDpi;
            configState->set_target_geometry(target);
            if(editCanvas&&(editCanvas->dragging()||(editCanvas->coverDrag&&editCanvas->coverDrag->placing))&&
                (oldWidth!=configState->viewportWidth||oldHeight!=configState->viewportHeight||
                oldDpi!=configState->viewportDpi))hide_editing_canvas();
            configState->expire_visual();
            KillTimer(host,10);
            if(!configState->visualPositions.empty()){
                double due=1;for(const auto& result:configState->visualPositions)due=std::min(due,std::max(result.second.expires,result.second.transportHold)-visual_now());
                SetTimer(host,10,UINT(std::max(1.,std::ceil(due*1000))),nullptr);
            }
        }
        visual_tick();
        if(calibrationEditorSuspended||(visual&&visual->holding()))return;
        if (!available(target)) {
            normal.hide();
            for(auto& s:surfaces)s->hide();
            hide_editing_canvas();
            if(editorVisible!=editing){editorVisible=editing;if(configState)configState->stateSequence++;}
            CancelWaitableTimer(frameTimer);animationTimer=false;return;
        }
        if(editing&&!editorVisible){editorVisible=true;configState->stateSequence++;}
        if(canvas_display()){
            normal.clear();
            for(auto& s:surfaces)s->hide();
            bool visible=editing_visible();
            if(visible)edit_frame();
            else hide_editing_canvas();
            bool animate=visible&&!pickWaiting&&!(editCanvas&&editCanvas->picking)&&std::any_of(surfaces.begin(),surfaces.end(),[](auto& s){return s->region.animated();});
            if(animate&&!animationTimer){LARGE_INTEGER due;due.QuadPart=-330000;SetWaitableTimerEx(frameTimer,&due,33,nullptr,nullptr,nullptr,0);}
            if(!animate&&animationTimer)CancelWaitableTimer(frameTimer);
            animationTimer=animate;return;
        }
        if(editCanvas&&editCanvas->valid()&&IsWindowVisible(editCanvas->hwnd))hide_editing_canvas();
        try{normal.sync(surfaces,target,seconds(),configState.get());report_issue(renderIssue,"");}
        catch(const std::exception& error){
            normal.hide();for(auto& surface:surfaces){surface->hide();surface->release();}errors++;record_error(error.what());
            report_issue(renderIssue,"render_failed");
        }
        bool animate=normal.active(surfaces);
        if(animate&&!animationTimer){
            LARGE_INTEGER due;due.QuadPart=-330000;
            if(!SetWaitableTimerEx(frameTimer,&due,33,nullptr,nullptr,nullptr,0))throw std::runtime_error("Cannot start frame timer");
        }
        if(!animate&&animationTimer)CancelWaitableTimer(frameTimer);
        animationTimer=animate;
    }
    static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
        try {
        if(instance&&instance->editCanvas&&hwnd==instance->editCanvas->hwnd)return instance->canvas_message(hwnd,msg,w,l);
        if (msg==WM_NCHITTEST)return HTTRANSPARENT;
        if(instance&&(msg==WM_DISPLAYCHANGE||msg==WM_DPICHANGED||msg==WM_DWMCOMPOSITIONCHANGED||
                      (msg==WM_POWERBROADCAST&&w==PBT_APMRESUMEAUTOMATIC))&&!instance->pending){
            if(msg==WM_DISPLAYCHANGE||msg==WM_DWMCOMPOSITIONCHANGED||msg==WM_POWERBROADCAST)instance->visualCaptureInvalid=true;
            instance->pending=true;PostMessageW(instance->host,WM_APP+1,0,0);
        }
        if (instance && hwnd==instance->host) {
            if(instance->shell){
                auto& shell=*instance->shell;
                if(msg==shell.taskbarCreated){if(!shell.add())SetTimer(hwnd,9,2000,nullptr);return 0;}
                if(msg==NativeShell::callback){
                    UINT event=LOWORD(l);
                    if(event==WM_CONTEXTMENU)instance->system_command(shell.menu(instance->target,instance->editing,instance->applications.get()));
                    else if(event==NIN_SELECT||event==NIN_KEYSELECT)instance->open_or_restore_editor();
                    return 0;
                }
                if(msg==WM_HOTKEY&&w==NativeShell::hotkeyId){instance->open_or_restore_editor();return 0;}
            }
            if(msg==WM_CLOSE||msg==WM_APP+55){instance->request_quit();return 0;}
            if(msg==WM_APP+56){instance->open_or_restore_editor();return 0;}
            if(msg==ApplicationPicker::notification){
                if(instance->applicationPicker){
                    auto identity=instance->applicationPicker->selected;instance->applicationPicker.reset();
                    if(identity.window)instance->queue_application("",identity);
                    else {instance->sync();if(instance->resumeApplicationEditor){instance->resumeApplicationEditor=false;instance->open_or_restore_editor();}}
                }
                return 0;
            }
            if(msg==WM_APP+63){instance->finish_application_action();return 0;}
            // Isolated shell tests exercise the same queued actions as the tray.
            if(instance->diagnostic&&instance->applications&&msg==WM_APP+57){
                instance->queue_application("",WindowIdentity::read(reinterpret_cast<HWND>(w)));return 0;
            }
            if(instance->diagnostic&&instance->applications&&msg==WM_APP+58){
                const auto& entries=instance->applications->document["applications"];
                if(w<entries.size())instance->queue_application(entries[size_t(w)]["id"]);return 0;
            }
            if(instance->diagnostic&&instance->applications&&msg==WM_APP+59){
                if(!instance->applicationPicker)instance->queue_application("",{},true);return 0;
            }
            if(instance->diagnostic&&instance->applications&&msg==WM_APP+64){
                if(!instance->applications->active().empty())instance->queue_application("",{},false,true);return 0;
            }
            if(instance->diagnostic&&instance->applications&&msg==WM_APP+65){
                const auto& entries=instance->applications->document.at("applications");
                if(w<entries.size())instance->queue_application("",{},false,false,entries[w].at("id").get<std::string>());
                return 0;
            }
            if(instance->diagnostic&&instance->independentWindows&&msg==WM_APP+66){
                auto id=instance->applications->active();
                if(!id.empty()&&instance->target){
                    auto windows=instance->applications->app(id).value("windows",json::array());
                    auto profile=w==0?std::string("legacy"):
                        w<=windows.size()?windows[size_t(w-1)].at("id").get<std::string>():std::string();
                    if(!profile.empty())instance->queue_application(id,instance->targetIdentity,false,false,"",profile);
                }
                return 0;
            }
            if(instance->diagnostic&&instance->independentWindows&&msg==WM_APP+67){
                auto id=instance->applications->active();
                if(id.empty())return 0;
                auto windows=instance->applications->app(id).value("windows",json::array());
                if(w>=windows.size())return 0;
                try{instance->delete_archived_window_profile(windows[w].at("id"));return TRUE;}
                catch(const std::exception& error){record_error(error.what());return FALSE;}
            }
            if(instance->diagnostic&&instance->regionScopes&&msg==WM_APP+68){
                try{
                    json payload={{"id",instance->configState->selected}};
                    if(w){
                        instance->editor_operation("list_applications",json::object(),instance->editorPid);
                        const auto& windows=instance->editorApplicationWindows;
                        auto found=std::find_if(windows.begin(),windows.end(),[w](const auto& identity){
                            return identity.window==reinterpret_cast<HWND>(w);
                        });
                        if(found==windows.end())return FALSE;
                        payload["index"]=std::distance(windows.begin(),found);
                    }
                    instance->editor_operation("set_region_scope",payload,instance->editorPid);
                    return TRUE;
                }catch(const std::exception& error){record_error(error.what());return FALSE;}
            }
            if(msg==WM_QUERYENDSESSION){
                if(l&ENDSESSION_CRITICAL)return TRUE;
                if(instance->editing){
                    instance->sessionSavePending=true;
                    ShutdownBlockReasonCreate(hwnd,L"正在保存覆盖文字。保存完成后请重试关机或注销。");
                    instance->request_finish();return FALSE;
                }
                try{if(instance->configState)instance->configState->save();}
                catch(const std::exception& error){
                    instance->sessionSavePending=true;instance->saveError=error.what();record_error(error.what());
                    instance->report_save_issue();
                    ShutdownBlockReasonCreate(hwnd,L"覆盖配置尚未保存，请检查存储位置后重试。");
                    SetTimer(hwnd,8,1000,nullptr);return FALSE;
                }
                return TRUE;
            }
            if(msg==WM_ENDSESSION&&w){
                if(instance->configState)try{instance->configState->save();}catch(const std::exception& error){record_error(error.what());}
                return 0;
            }
            if(msg==WM_APP+54&&instance->diagnostic&&instance->diagnosticTargetPid){
                HWND target=reinterpret_cast<HWND>(w);
                if(available(target)&&process_id(target)==instance->diagnosticTargetPid&&(!instance->applications||instance->applications->accepts(target))){
                    if(instance->independentWindows)
                        instance->queue_application(instance->applications->active(),WindowIdentity::read(target));
                    else if(instance->applications)instance->bind_window(WindowIdentity::read(target));
                    else {instance->hide_editing_canvas();instance->target=target;instance->targetPid=process_id(target);instance->targetDestroyed=false;}
                    if(instance->configState)instance->configState->stateSequence++;
                    instance->sync();
                }
                return 0;
            }
            if(msg==PipeServer::notification){
                if(instance->targetDestroyed||(instance->applications&&instance->target&&!instance->targetIdentity.current()))instance->sync();
                if(instance->pipe&&instance->protocol)instance->pipe->drain([&](const PipeServer::Ticket& ticket){return instance->protocol->dispatch(ticket);});
                if(instance->visual&&instance->visual->pipe)instance->visual->pipe->drain([&](const PipeServer::Ticket& ticket){return instance->visual->dispatch(ticket,instance->target);});
                return 0;
            }
            if(msg==WM_APP+1){instance->sync();return 0;}
            if(msg==WM_TIMER){
                if(w==11){instance->visual_tick();return 0;}
                if(w==10){KillTimer(hwnd,10);instance->sync();return 0;}
                if(w==1){
                    instance->sample();
                    if(instance->shell&&!instance->pendingNotice.empty()){
                        instance->shell->notice(wide(instance->pendingNotice),!instance->saveError.empty());instance->pendingNotice.clear();
                    }
                }
                if(w==2)instance->sync();if(w==3)DestroyWindow(hwnd);
                if(w==9&&instance->shell&&instance->shell->add())KillTimer(hwnd,9);
                if(w==5)instance->editor_tick();if(w==6){KillTimer(hwnd,6);instance->sync();}
                if(w==7){KillTimer(hwnd,7);instance->complete_pick();}
                if(w==8){
                    KillTimer(hwnd,8);
                    if(instance->configState)try{
                        auto previous=instance->configState->persistedRevision;
                        instance->configState->save();
                        if(!instance->saveError.empty()||previous!=instance->configState->persistedRevision)instance->configState->stateSequence++;
                        instance->saveError.clear();
                        instance->report_save_issue();
                        if(instance->sessionSavePending&&!instance->editing){ShutdownBlockReasonDestroy(hwnd);instance->sessionSavePending=false;}
                        if(instance->shutdownRequested&&!instance->editing&&!instance->editorProcess.value)PostMessageW(hwnd,WM_APP+55,0,0);
                    }catch(const std::exception& error){
                        if(instance->saveError!=error.what()){
                            instance->saveError=error.what();instance->configState->stateSequence++;
                            instance->report_save_issue();
                            record_error("autosave: "+std::string(error.what()));
                            instance->pendingNotice="自动保存暂未完成，修改仍被保留，正在重试。";
                        }
                        SetTimer(hwnd,8,1000,nullptr);
                    }
                }
                return 0;
            }
            if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
        }
        return DefWindowProcW(hwnd,msg,w,l);
        }catch(const std::exception& error){
            if(instance){for(auto& s:instance->surfaces)s->hide();instance->errors++;}
            record_error(error.what());
            PostQuitMessage(1);return 0;
        }catch(...){PostQuitMessage(1);return 0;}
    }
    Engine() {
        frameTimer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        highResolutionTimer=frameTimer!=nullptr;
        if(!frameTimer)frameTimer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);
        if(!frameTimer)throw std::runtime_error("Cannot create frame timer");
        instance=this;
    }
    ~Engine(){visual.reset();pipe.reset();for(auto h:hooks)UnhookWinEvent(h);instance=nullptr;if(frameTimer)CloseHandle(frameTimer);}
};
Engine* Engine::instance=nullptr;
#include "editing_host.hpp"
#include "system_host.hpp"
#include "lifecycle_tests.hpp"

int WINAPI wWinMain(HINSTANCE h,HINSTANCE,LPWSTR,int) {
    bool diagnosticRun=false;
    fs::path config;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        fs::path ready,samples,renderTests,lifecycleTests,referenceTests,visualTests,configTests,editorScript,pythonExecutable;
        fs::path engineVisualPython,engineVisualScript,engineVisualDeps;
        std::wstring profileTestPrefix;
        HWND target=nullptr;double duration=1.5;bool checkMode=false,cpuComposite=true,ipcTest=false,editorHost=false,simulateFocus=false,shellTest=false,windowProfiles=false,regionScopes=false;DWORD clientPid=0;
        int sampleInterval=1000;
        for(int i=1;i<argc;i++){
            std::wstring key=argv[i];
            if(key==L"--check")checkMode=true;
            else if(key==L"--d2d-composite")cpuComposite=false;
            else if(key==L"--ipc-test-server")ipcTest=true;
            else if(key==L"--editor-host")editorHost=true;
            else if(key==L"--simulate-focus")simulateFocus=true;
            else if(key==L"--shell-test")shellTest=true;
            else if(key==L"--window-profiles")windowProfiles=true;
            else if(key==L"--region-scopes")regionScopes=true;
            else {
                if(i+1>=argc)throw std::runtime_error("Missing command argument");
                std::wstring value=argv[++i];
                if(key==L"--config")config=value;
                else if(key==L"--ready-file")ready=value;
                else if(key==L"--sample-file")samples=value;
                else if(key==L"--sample-interval-ms")sampleInterval=std::stoi(value);
                else if(key==L"--check-seconds")duration=std::stod(value);
                else if(key==L"--render-tests")renderTests=value;
                else if(key==L"--lifecycle-tests")lifecycleTests=value;
                else if(key==L"--reference-picker-tests")referenceTests=value;
                else if(key==L"--visual-tests")visualTests=value;
                else if(key==L"--config-tests")configTests=value;
                else if(key==L"--editor-script")editorScript=fs::absolute(value);
                else if(key==L"--python")pythonExecutable=fs::absolute(value);
                else if(key==L"--visual-python")engineVisualPython=fs::absolute(value);
                else if(key==L"--visual-script")engineVisualScript=fs::absolute(value);
                else if(key==L"--visual-deps")engineVisualDeps=fs::absolute(value);
                else if(key==L"--client-pid")clientPid=DWORD(std::stoul(value));
                else if(key==L"--target-hwnd")target=reinterpret_cast<HWND>(std::stoull(value,nullptr,0));
                else if(key==L"--application-profiles"){
                    if(value.empty()||value.size()>200)throw std::runtime_error("Application profile diagnostics require a test window title prefix");
                    profileTestPrefix=value;
                }
                else throw std::runtime_error("Unknown command argument");
            }
        }
        LocalFree(argv);
        diagnosticRun=checkMode||!renderTests.empty()||!lifecycleTests.empty()||!referenceTests.empty()||!visualTests.empty()||!configTests.empty();
        if(config.empty()&&!diagnosticRun)config=application_data()/L"config.json";
        if(config.empty()&&renderTests.empty()&&lifecycleTests.empty()&&referenceTests.empty()&&visualTests.empty()&&configTests.empty())throw std::runtime_error("Diagnostics require an isolated --config file");
        if(target && !checkMode)throw std::runtime_error("Explicit test HWND requires --check");
        if((editorHost||simulateFocus)&&!checkMode)throw std::runtime_error("Prototype editor flags require --check");
        if(shellTest&&!checkMode)throw std::runtime_error("Shell test requires --check");
        if(!profileTestPrefix.empty()&&(!shellTest||ipcTest))throw std::runtime_error("Application profile diagnostics require --shell-test without IPC test server");
        if(windowProfiles&&profileTestPrefix.empty())throw std::runtime_error("Window profile diagnostics require --application-profiles");
        if(regionScopes&&!windowProfiles)throw std::runtime_error("Region scope diagnostics require --window-profiles");
        if(sampleInterval<25||sampleInterval>1000||sampleInterval!=1000&&!checkMode)throw std::runtime_error("Fast samples require check mode and 25..1000 ms");
        if(!std::isfinite(duration)||duration<=0||duration>=86400)throw std::runtime_error("Invalid check duration");
        WinHandle singleInstance;
        if(!diagnosticRun||shellTest){
            std::wstring name=shellTest?L"Local\\ChatGPTOverlay.ShellTest."+std::to_wstring(std::hash<std::wstring>{}(fs::absolute(config).wstring())):
                L"Local\\ChatGPTOverlay.SingleInstance";
            HANDLE mutex=CreateMutexW(nullptr,FALSE,name.c_str());DWORD result=GetLastError();singleInstance.reset(mutex);
            if(!mutex)throw std::runtime_error("Cannot create single-instance lock");
            if(result==ERROR_ALREADY_EXISTS){
                if(!shellTest){HWND existing=FindWindowW(L"OverlayNativeSurface",L"ChatGPT Overlay host");
                    if(existing){AllowSetForegroundWindow(process_id(existing));PostMessageW(existing,WM_APP+56,0,0);}
                    else MessageBoxW(nullptr,L"工具已在运行，请使用系统托盘图标。",L"ChatGPT Overlay",MB_OK|MB_ICONINFORMATION);}
                return 0;
            }
        }
        Engine engine;engine.target=target;engine.diagnostic=checkMode;engine.samplePath=samples;
        engine.independentWindows=!diagnosticRun||windowProfiles;
        engine.regionScopes=!diagnosticRun||regionScopes;
        if(checkMode&&target)engine.diagnosticTargetPid=process_id(target);
        engine.renderer.cpuComposite=cpuComposite;
        engine.editorScript=editorScript;engine.pythonExecutable=pythonExecutable;engine.simulateFocus=simulateFocus;
        engine.editorExecutable=application_file().parent_path()/L"editor"/L"OverlayEditor.exe";
        if(!configTests.empty()){config_tests(configTests);visual_preview_tests(engine.renderer,configTests/L"preview");visual_retirement_tests(configTests/L"retirement");return 0;}
        if(!visualTests.empty()){visual_config_tests(visualTests/L"config");visual_preview_tests(engine.renderer,visualTests/L"preview");visual_search_tests(visualTests/L"search");return 0;}
        WNDCLASSW wc{};wc.lpfnWndProc=Engine::proc;wc.hInstance=h;wc.style=CS_DBLCLKS;wc.lpszClassName=L"OverlayNativeSurface";RegisterClassW(&wc);
        if(!renderTests.empty()){render_tests(engine.renderer,renderTests);return 0;}
        engine.host=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"ChatGPT Overlay host",WS_POPUP,0,0,0,0,nullptr,nullptr,h,nullptr);
        if(!engine.host)throw std::runtime_error("Cannot create engine host");
        if(!lifecycleTests.empty()){lifecycle_tests(engine,lifecycleTests);return 0;}
        if(!referenceTests.empty()){reference_picker_tests(engine,referenceTests);return 0;}
        if(!diagnosticRun||!profileTestPrefix.empty()){
            engine.applications=std::make_unique<ApplicationProfiles>(config,profileTestPrefix);
            if(target&&!engine.applications->accepts(target))throw std::runtime_error("Target does not match the test window prefix");
            engine.targetIdentity=WindowIdentity::read(target);
            auto active=engine.applications->active();
            if(active.empty()||engine.independentWindows){engine.target=nullptr;engine.targetIdentity={};}
            if(!active.empty())config=engine.applications->config_path(active);
        }
        if(ipcTest||editorHost||!diagnosticRun||shellTest)
            engine.configState=std::make_unique<ConfigState>(config,!engine.applications||
                (!engine.independentWindows&&!engine.applications->active().empty()));
        engine.visualPython=engineVisualPython;engine.visualScript=engineVisualScript;engine.visualDeps=engineVisualDeps;
        if(engine.configState){
            if(engine.applications){
                engine.configState->applicationId=engine.applications->active();
                if(engine.configState->applicationId=="chatgpt")engine.configState->otherApplicationDirectory=engine.applications->application_directory();
            }
            engine.visual=engine.make_visual(*engine.configState);
        }
        if(ipcTest){
            if(!checkMode||!clientPid)throw std::runtime_error("IPC test server requires --check and --client-pid");
            engine.pipe=std::make_unique<PipeServer>(engine.host,clientPid);
            engine.protocol=std::make_unique<SessionProtocol>(*engine.configState,engine.pipe->nonce);
            bool visualComponent=!engineVisualScript.empty()||engine.visual->component_available();
            if(visualComponent)engine.protocol->extension=[&engine](const std::string& op,const json& payload,DWORD pid){
                if(op=="hello"||op=="select"||op=="commit_close")return json::object();
                return engine.editor_operation(op,payload,pid);
            };
            if(visualComponent)engine.protocol->extraState=[&engine](){
                HWND surface=nullptr;for(const auto& s:engine.surfaces)if(s->region.source.value("id",std::string())==engine.visual->id)surface=s->hwnd;
                return json{
                {"visual_worker_pid",engine.visual->workerPid},{"visual_error",engine.visual->error},{"visual_stage",engine.visual->stage},
                {"visual_observation",engine.visual->lastObservation},{"target_hwnd",reinterpret_cast<uintptr_t>(engine.target)},
                {"visual_regions",engine.visual->diagnostics()},
                {"visual_blocked_hwnd",reinterpret_cast<uintptr_t>(engine.visual->blockedWindow)},
                {"visual_blocked_pid",process_id(engine.visual->blockedWindow)},
                {"calibration_preview",engine.visual->preview_state()},
                {"visual_capabilities",{"visual_geometry_v1","visual_tracking_v1","visual_calibration_preview_v1"}},
                {"visual_worker_available",engine.visual->component_available()},{"visual_surface_hwnd",reinterpret_cast<uintptr_t>(surface)}};};
        }
        json document;if(engine.configState)document=engine.configState->document;else std::ifstream(config)>>document;
        if((document.value("version",0)!=1&&document.value("version",0)!=2)||!document["regions"].is_array())throw std::runtime_error("Unsupported configuration");
        for(auto& entry:document["regions"])engine.surfaces.emplace_back(std::make_unique<Surface>(engine.renderer,Region(entry),false));
        if(!diagnosticRun||shellTest){engine.shell=std::make_unique<NativeShell>();engine.shell->start(engine.host,shellTest);}
        if(!samples.empty()){fs::create_directories(samples.parent_path());std::ofstream(samples).close();}
        if(engine.independentWindows){
            auto active=engine.applications->active();
            for(const auto& app:engine.applications->document.at("applications")){
                if(app.value("deleting",false))continue;
                auto id=app.at("id").get<std::string>();
                for(const auto& binding:app.value("window_bindings",json::object())){
                    std::string profile;
                    auto identity=engine.applications->restore_window(id,binding,profile);
                    if(identity.window)engine.windowBindings[identity.window]={identity,id,profile};
                }
            }
            if(target&&!active.empty())engine.queue_application(active,WindowIdentity::read(target));
            else if(!active.empty()&&engine.applications->app(active).contains("last_window")){
                std::string profile;
                auto identity=engine.applications->restore_window(active,engine.applications->app(active).at("last_window"),profile);
                // A new unique window can use app-wide regions. Other already-bound
                // windows retain their own profile until the user selects one.
                auto candidates=engine.applications->candidates(active,true);
                if(identity.window||!engine.regionScopes)engine.applicationsAwaitingWindowChoice.insert(active);
                if(engine.regionScopes){
                    std::vector<WindowIdentity> siblings;
                    for(const auto& candidate:candidates){
                        if(candidate.window==identity.window)continue;
                        if(identity.window||engine.windowBindings.count(candidate.window))siblings.push_back(candidate);
                    }
                    if(!siblings.empty()){
                        engine.applicationsAwaitingWindowChoice.insert(active);
                        engine.reconnectBlockers[active]=std::move(siblings);
                    }else if(identity.window)engine.reconnectBlockers[active]={};
                }
                if(identity.window)engine.queue_application(active,identity,false,false,"",profile);
            }
        }
        engine.sync();engine.sample();
        if(editorHost){
            if(engine.application_action_pending())engine.resumeApplicationEditor=true;
            else engine.begin_editing();
        }
        if(!ready.empty()){fs::create_directories(ready.parent_path());json readyState={{"pid",GetCurrentProcessId()},
            {"first_frame_visible",std::any_of(engine.normal.plan.begin(),engine.normal.plan.end(),[&](const auto& group){return IsWindowVisible(engine.normal.window(size_t(&group-engine.normal.plan.data()),engine.surfaces))!=0;})},
            {"hotkey_registered",engine.shell&&engine.shell->hotkey},{"tray_visible",engine.shell&&engine.shell->added},{"host_hwnd",reinterpret_cast<uintptr_t>(engine.host)},
            {"editor_pid",engine.editorPid},{"canvas_hwnd",engine.editCanvas?reinterpret_cast<uintptr_t>(engine.editCanvas->hwnd):0}};
            if(engine.pipe){readyState["pipe"]=utf8(engine.pipe->name);readyState["session"]=engine.pipe->nonce;}
            std::ofstream(ready)<<readyState;}
        for(auto range: {std::pair<DWORD,DWORD>{EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND},{EVENT_SYSTEM_DESKTOPSWITCH,EVENT_SYSTEM_DESKTOPSWITCH},
            {EVENT_SYSTEM_MINIMIZESTART,EVENT_SYSTEM_MINIMIZEEND},{EVENT_OBJECT_DESTROY,EVENT_OBJECT_HIDE},
            {EVENT_OBJECT_CLOAKED,EVENT_OBJECT_UNCLOAKED},{EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE}}) {
            auto hook=SetWinEventHook(range.first,range.second,nullptr,Engine::event,0,0,WINEVENT_OUTOFCONTEXT);
            if(!hook)throw std::runtime_error("Cannot install window hook");engine.hooks.push_back(hook);
        }
        SetTimer(engine.host,1,UINT(sampleInterval),nullptr);SetTimer(engine.host,2,2000,nullptr);
        if(checkMode)SetTimer(engine.host,3,UINT(std::clamp(duration*1000,1.,86400000.)),nullptr);
        for(;;){
            DWORD result=MsgWaitForMultipleObjectsEx(1,&engine.frameTimer,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(result==WAIT_FAILED)throw std::runtime_error("Message wait failed");
            if(result==WAIT_OBJECT_0)engine.animate();
            MSG msg;
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
                if(msg.message==WM_QUIT)return int(msg.wParam);
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
        }
    }catch(const std::exception& ex){
        record_error(ex.what());
        if(!diagnosticRun){
            bool invalidConfig=dynamic_cast<const json::parse_error*>(&ex)!=nullptr;
            const char* code=invalidConfig?"configuration_invalid":"startup_failed";
            try{record_native_issue((config.empty()?application_data():config.parent_path())/L"diagnostics",code,"failed");}catch(...){}
            const wchar_t* advice=invalidConfig?L"请保留并备份原配置，再尝试恢复上次可用的配置。":L"请检查是否完整解压当前版，然后重试。";
            MessageBoxW(nullptr,(std::wstring(L"无法启动 ChatGPT Overlay。\n")+advice+L"\n\n问题编号："+wide(code)).c_str(),L"ChatGPT Overlay",MB_OK|MB_ICONERROR);
        }
        return 1;
    }
}
