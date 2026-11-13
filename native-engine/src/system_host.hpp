#pragma once

inline std::unique_ptr<VisualRuntime> Engine::make_visual(ConfigState& state){
    state.runtimeChanged=[this](){PostMessageW(host,WM_APP+1,0,0);};
    auto runtime=std::make_unique<VisualRuntime>(state,normal,surfaces,host);
    runtime->python=visualPython;runtime->script=visualScript;runtime->deps=visualDeps;
    if(visualPython.empty()&&visualScript.empty())runtime->executable=application_file().parent_path()/L"vision"/L"OverlayVision.exe";
    runtime->reconcile=[this](){reconcile(false);PostMessageW(host,WM_APP+1,0,0);};
    return runtime;
}

inline void Engine::bind_window(const WindowIdentity& identity){
    set_canvas_mode(CanvasMode::Idle);
    hide_editing_canvas();
    if(visual){
        if(visual->stage=="preview")visual->cancel_preview();
        else if(!visual->stage.empty())visual->abort_calibration();
        visual->windowPermitted=false;visual->stop();visual->stage.clear();
    }
    if(calibrationEditorSuspended){calibrationEditorSuspended=false;calibrationResult="failed";if(shell)shell->calibration_status(L"");}
    KillTimer(host,7);pickWaiting=false;pickId.clear();pickSession.clear();
    if(editCanvas)editCanvas->picking=false;
    normal.clear();for(auto& surface:surfaces)surface->hide();
    targetIdentity=identity;target=identity.window;targetPid=identity.pid;targetDestroyed=false;
    targetSiblings.clear();
    if(target&&applications){
        applicationsAwaitingWindowChoice.erase(applications->active());
        reconnectBlockers.erase(applications->active());
        targetSiblings=applications->candidates(applications->active(),true);
        forget_sibling(target);
    }
    configState->confirmedBoundaries.clear();configState->featureOrigins.clear();
    configState->bindingEpoch=++bindingEpoch;
    configState->invalidate_visual();
}

inline void Engine::queue_application(const std::string& id,const WindowIdentity& identity,bool pick,bool remove,
                                      const std::string& deletion,const std::string& profile){
    if(!applications||shutdownRequested)return;
    if(!pick&&!remove&&deletion.empty()&&id.empty()&&(!identity.window||!identifiable_application(identity))){
        if(shell)shell->notice(L"无法识别该应用窗口，请重新点选。",true);
        sync();if(std::exchange(resumeApplicationEditor,false))open_or_restore_editor();return;
    }
    pendingApplication=id;pendingApplicationWindow=identity;pendingApplicationPick=pick;pendingApplicationRemove=remove;
    pendingDeletion=deletion;
    pendingWindowProfile=profile;
    resumeApplicationEditor=resumeApplicationEditor||editing||reopenEditor;
    reopenEditor=false;
    if(editing){
        // A drag may already have changed the in-memory preview. Cancel it before commit_close.
        set_canvas_mode(CanvasMode::Idle);hide_editing_canvas();
        if(visual&&visual->stage=="preview")visual->cancel_preview();
        if(calibrationEditorSuspended){visual->abort_calibration();calibrationEditorSuspended=false;}
        KillTimer(host,7);pickWaiting=false;pickId.clear();pickSession.clear();
        if(editCanvas)editCanvas->picking=false;
        request_finish();sync();
    }else PostMessageW(host,WM_APP+63,0,0);
}

inline void Engine::finish_application_action(){
    if(shutdownRequested||editorProcess.value||editing)return;
    if(!application_action_pending())return;
    auto requested=std::move(pendingApplication);pendingApplication.clear();
    auto identity=std::exchange(pendingApplicationWindow,WindowIdentity{});
    bool restoringWindow=identity.propertyToken!=0;
    bool pick=std::exchange(pendingApplicationPick,false);
    bool remove=std::exchange(pendingApplicationRemove,false);
    auto deletion=std::exchange(pendingDeletion,std::string());
    auto requestedProfile=std::exchange(pendingWindowProfile,std::string());
    applicationActionRunning=true;
    try{
        configState->save();saveError.clear();report_save_issue();
        if(pick){
            if(visual){visual->windowPermitted=false;visual->stop();}
            applicationPicker=std::make_unique<ApplicationPicker>(host);
            if(shell)shell->notice(L"点击要保存的应用窗口，按 Esc 取消。");
            sync();applicationActionRunning=false;return;
        }
        auto next=*applications;
        auto savedCount=next.document.at("applications").size();
        if(!deletion.empty()){
            next.begin_deletion(deletion);
            requested=next.active();
        }else if(remove){
            next.remove_current();
            requested.clear();
        }else if(identity.window){
            if(!next.accepts(identity.window,restoringWindow)||!identity.current())throw std::runtime_error("Target window is no longer available");
            if(requested.empty())requested=next.remember(identity);
            else if(!next.matches(next.app(requested),identity))throw std::runtime_error("Target window does not match the application");
        }else if(!ApplicationProfiles::listed(next.app(requested))){
            throw std::runtime_error("Application is no longer listed");
        }else if(!applicationsAwaitingWindowChoice.count(requested)){
            if(requested!=next.active()||!target){
                auto candidates=next.candidates(requested);
                if(candidates.size()==1)identity=candidates.front();
            }else identity=targetIdentity;
        }
        if(independentWindows){
            if(!deletion.empty()&&requested==applications->active())requestedProfile=windowProfile;
            else if(!identity.window)requestedProfile.clear();
            else if(requestedProfile.empty()){
                auto binding=windowBindings.find(identity.window);
                if(binding!=windowBindings.end()&&binding->second.application==requested&&binding->second.identity.current())
                    requestedProfile=binding->second.profile;
                else requestedProfile=next.create_window_profile(requested,identity.window);
            }else if(profile_bound_elsewhere(requested,requestedProfile,identity.window)){
                throw std::runtime_error("该配置已关联到另一个打开的窗口。");
            }else if(requestedProfile!="legacy"&&next.window_profile(requested,requestedProfile).value("deleting",false)){
                throw std::runtime_error("请先完成该窗口配置的删除。");
            }
        }
        bool different=requested!=applications->active()||requestedProfile!=windowProfile;
        std::unique_ptr<ConfigState> nextConfig;
        std::vector<std::unique_ptr<Surface>> nextSurfaces;
        std::unique_ptr<VisualRuntime> nextVisual;
        if(different){
            bool persistent=!requested.empty()&&(!independentWindows||!requestedProfile.empty());
            auto configPath=next.legacyConfig;
            if(persistent)configPath=independentWindows?next.window_config_path(requested,requestedProfile):next.config_path(requested);
            if(persistent&&regionScopes)configPath=next.ensure_scoped_config(requested);
            nextConfig=std::make_unique<ConfigState>(configPath,persistent,regionScopes&&persistent?requestedProfile:"");
            if(persistent&&!regionScopes&&(!independentWindows||requestedProfile=="legacy"))
                nextConfig->otherApplicationDirectory=requested=="chatgpt"?next.application_directory():next.window_directory(requested);
            for(const auto& region:nextConfig->document["regions"])
                nextSurfaces.emplace_back(std::make_unique<Surface>(renderer,Region(region),false));
            nextVisual=make_visual(*nextConfig);
            if(auto saved=applicationManualLocks.find(requested+"/"+requestedProfile);saved!=applicationManualLocks.end())
                nextVisual->manualLocks=saved->second;
            if(visual) applicationManualLocks[applications->active()+"/"+windowProfile]=visual->manualLocks;
            // A newly saved application gets its own configuration immediately.
            if(nextConfig->persistent&&!fs::exists(nextConfig->path))nextConfig->save();
        }
        if(independentWindows&&identity.window){
            identity.mark();
            if(!identity.current())throw std::runtime_error("Target window is no longer available");
            next.remember_window(requested,identity,requestedProfile);
        }
        next.document["active"]=requested;
        if(next.document!=applications->document||!fs::exists(next.path))next.save();
        if(different){
            KillTimer(host,8);KillTimer(host,10);KillTimer(host,11);
            visual.reset();normal.clear();normal.separate=false;
            surfaces=std::move(nextSurfaces);
            nextConfig->visualGeneration=configState->visualGeneration;
            configState=std::move(nextConfig);
            calibrationResult.clear();calibrationResultOwner.clear();
            targetIssue.clear();saveIssue.clear();renderIssue.clear();pickIssue.clear();editorIssue.clear();
            visual=std::move(nextVisual);
        }
        *applications=std::move(next);
        configState->applicationId=requested;
        windowProfile=requestedProfile;
        // A window may have closed while the old editor or worker was shutting down.
        if(identity.window&&!identity.current())identity={};
        if(independentWindows&&identity.window)windowBindings[identity.window]={identity,requested,windowProfile};
        if(deletion.empty()||different)bind_window(identity);
        if(!deletion.empty()){
            for(auto it=windowBindings.begin();it!=windowBindings.end();){
                if(it->second.application==deletion)it=windowBindings.erase(it);
                else ++it;
            }
            applications->finish_deletion(deletion);
            auto prefix=deletion+"/";
            for(auto it=applicationManualLocks.lower_bound(prefix);it!=applicationManualLocks.end()&&
                it->first.compare(0,prefix.size(),prefix)==0;)it=applicationManualLocks.erase(it);
            applicationsAwaitingWindowChoice.erase(deletion);
            reconnectBlockers.erase(deletion);
        }
        if(target&&applications->document.at("applications").size()>savedCount){
            set_canvas_mode(CanvasMode::Free);
            resumeApplicationEditor=true;
        }
        sync();
        if(deletion.empty()&&!diagnostic&&!restoringWindow&&available(target))SetForegroundWindow(target);
        if(shell&&!deletion.empty())shell->notice(L"已删除应用的遮盖和配置。");
        else if(shell&&!target)shell->notice(remove?L"已从列表移除。遮盖和视觉模板已保留，请从面板选择下一个应用。":
            awaiting_window_choice()?L"请从面板的应用菜单选择要绑定的窗口。":
            L"等待应用窗口。若有多个窗口，请从面板选择要绑定的窗口。");
    }catch(const std::exception& error){
        record_error("application switch: "+std::string(error.what()));
        if(shell)shell->notice((deletion.empty()?L"应用切换未完成：":L"删除未完成，可在应用菜单重试：")+wide(error.what()),true);
        sync();
    }
    applicationActionRunning=false;
    if(std::exchange(resumeApplicationEditor,false))open_or_restore_editor();
}

inline void Engine::request_quit(){
    shutdownRequested=true;
    applicationPicker.reset();pendingApplication.clear();pendingDeletion.clear();pendingWindowProfile.clear();pendingApplicationWindow={};
    pendingApplicationPick=false;pendingApplicationRemove=false;resumeApplicationEditor=false;
    if(editing){request_finish();return;}
    // Keep IPC alive until the editor receives the durable ACK and exits.
    if(editorProcess.value&&WaitForSingleObject(editorProcess.value,0)==WAIT_TIMEOUT)return;
    try{if(configState)configState->save();}
    catch(const std::exception& error){
        saveError=error.what();record_error("quit save: "+saveError);
        report_save_issue();
        SetTimer(host,8,1000,nullptr);
        if(shell)shell->notice(L"保存失败，程序暂不退出；修改仍被保留，将继续重试。",true);
        return;
    }
    DestroyWindow(host);
}

inline void Engine::open_or_restore_editor(){
    try{
        if(applicationPicker||application_action_pending()){resumeApplicationEditor=true;return;}
        if(editing){
            if(calibrationEditorSuspended){
                if(visual){visual->abort_calibration();visual->exclusive=false;visual->operationError="calibration_cancelled";}
                calibrationEditorSuspended=false;calibrationResult="failed";
            }
            if(pickWaiting||(editCanvas&&(editCanvas->dragging()||editCanvas->picking||editCanvas->referencePicking))||
                (visual&&visual->referencePicking))hide_editing_canvas();
            KillTimer(host,7);pickWaiting=false;pickId.clear();pickSession.clear();
            if(editCanvas)editCanvas->picking=false;
            editorVisible=true;editorRestoreSequence++;configState->stateSequence++;
            AllowSetForegroundWindow(editorPid);
            if(editor_window_valid()){
                if(!editorOwnsShow)ShowWindow(editorHwnd,IsIconic(editorHwnd)?SW_RESTORE:SW_SHOWNOACTIVATE);
                DWORD cloak=0;
                bool preparing=editorOwnsShow&&SUCCEEDED(DwmGetWindowAttribute(editorHwnd,DWMWA_CLOAKED,&cloak,sizeof(cloak)))&&cloak;
                if(!preparing&&IsWindowVisible(editorHwnd)&&!IsIconic(editorHwnd))SetForegroundWindow(editorHwnd);
            }
            sync();return;
        }
        if(editorProcess.value&&WaitForSingleObject(editorProcess.value,0)==WAIT_TIMEOUT){reopenEditor=true;return;}
        shutdownRequested=false;sync();
        if(IsIconic(target))ShowWindow(target,SW_RESTORE);
        if(!diagnostic&&available(target))SetForegroundWindow(target);
        begin_editing();
    }catch(const std::exception& error){
        record_error("start editor: "+std::string(error.what()));
        report_issue(editorIssue,"editor_start_failed");
        if(shell)shell->notice(L"无法打开编辑器："+wide(error.what()),true);
    }
}

inline void Engine::system_command(UINT command){
    if(!shell||!command)return;
    try{
        if(command==NativeShell::Edit)open_or_restore_editor();
        else if(command==NativeShell::Quit)request_quit();
        else if(applications&&command==NativeShell::PickApplication){
            if(!applicationPicker)queue_application("",{},true);
        }else if(applications&&command>=NativeShell::ApplicationFirst&&command-NativeShell::ApplicationFirst<shell->applicationIds.size()){
            queue_application(shell->applicationIds[command-NativeShell::ApplicationFirst]);
        }else if(applications&&command>=NativeShell::TargetFirst&&command-NativeShell::TargetFirst<shell->applicationWindows.size()){
            queue_application(applications->active(),shell->applicationWindows[command-NativeShell::TargetFirst]);
        }
        else if(command==NativeShell::Rediscover){
            hide_editing_canvas();targetPinned=false;target=find_target();targetPid=process_id(target);
            if(configState)configState->stateSequence++;sync();
            if(!available(target))shell->notice(L"尚未找到可见的 ChatGPT 窗口，将在后台继续等待。");
        }else if(command==NativeShell::OpenConfig){
            auto directory=configState->path.parent_path();fs::create_directories(directory);
            if(reinterpret_cast<INT_PTR>(ShellExecuteW(host,L"open",directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
                throw std::runtime_error("Cannot open configuration directory");
        }else if(command==NativeShell::Startup)shell->toggle_startup();
        else if(command==NativeShell::Help){
            MessageBoxW(host,L"从托盘点选并保存应用窗口，再按 Ctrl+Shift+E 打开或恢复编辑器。\n\n在空白处拖动创建矩形；拖动矩形移动，拖动右下角缩放。\n属性面板调整文字、背景和动画。Delete 删除，方向键微调。\nEsc 取消当前手势，或保存并结束编辑。\n\n各应用单独保存遮盖和视觉模板。目标关闭后，只有一个可用候选时自动重连。",L"ChatGPT Overlay · 使用说明",MB_OK|MB_ICONINFORMATION);
        }else if(command>=NativeShell::TargetFirst&&command-NativeShell::TargetFirst<shell->targets.size()){
            HWND candidate=shell->targets[command-NativeShell::TargetFirst];
            if(!available(candidate)||!is_chatgpt(candidate)||GetWindow(candidate,GW_OWNER))throw std::runtime_error("Target is no longer available");
            hide_editing_canvas();target=candidate;targetPinned=true;targetPid=process_id(candidate);configState->stateSequence++;sync();
        }
    }catch(const std::exception& error){record_error(error.what());shell->notice(L"操作未完成："+wide(error.what()),true);}
}
