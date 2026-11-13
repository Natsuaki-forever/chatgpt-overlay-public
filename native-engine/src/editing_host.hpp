#pragma once

inline std::wstring command_arg(const std::wstring& value){
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t ch:value){
        if(ch==L'\\'){slashes++;continue;}
        out.append(slashes*(ch==L'"'?2:1),L'\\');slashes=0;
        if(ch==L'"')out+=L'\\';out+=ch;
    }
    out.append(slashes*2,L'\\');return out+L"\"";
}

inline bool Engine::editing_visible(){
    if(simulateFocus)return true;
    HWND foreground=GetForegroundWindow();DWORD pid=process_id(foreground);
    bool related=foreground==target||GetAncestor(foreground,GA_ROOTOWNER)==target||
        (pid&&pid==GetCurrentProcessId())||(pid&&pid==editorPid);
    if(related){lastRelated=GetTickCount64();return true;}
    if(!foreground){SetTimer(host,6,120,nullptr);return editorVisible&&GetTickCount64()-lastRelated<=120;}
    return false;
}

inline void Engine::edit_frame(){
    if(!canvas_display()||calibrationEditorSuspended||!editorVisible||!available(target)||!editCanvas||!configState||pickWaiting)return;
    try{
        editCanvas->preview=visual?visual->render_preview():json(nullptr);
        editCanvas->referencePicking=visual&&visual->stage=="preview"&&visual->referencePicking;
        editCanvas->frame(*configState,surfaces,target,seconds());
        auto style=GetWindowLongPtrW(editCanvas->hwnd,GWL_EXSTYLE);
        auto inputStyle=canvas_input()?style&~WS_EX_TRANSPARENT:style|WS_EX_TRANSPARENT;
        if(style!=inputStyle)SetWindowLongPtrW(editCanvas->hwnd,GWL_EXSTYLE,inputStyle);
        // The panel has a separate owner. When the target comes back to the
        // foreground, restore the panel's position without activating it.
        HWND active=GetForegroundWindow();
        bool editingForeground=simulateFocus||active==target||active==editorHwnd||process_id(active)==GetCurrentProcessId();
        if(editingForeground&&editor_window_valid()&&IsWindowVisible(editorHwnd)&&!IsIconic(editorHwnd)&&editCanvas->valid()){
            HWND above=GetWindow(editCanvas->top_window(),GW_HWNDPREV);
            while(above&&above!=editorHwnd)above=GetWindow(above,GW_HWNDPREV);
            if(!above)SetWindowPos(editorHwnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        }
        if(!renderError.empty()){renderError.clear();configState->stateSequence++;}
        report_issue(renderIssue,"");
    }catch(const std::exception& error){
        editCanvas->hide();
        report_issue(renderIssue,"render_failed");
        if(renderError!=error.what()){
            renderError=error.what();configState->stateSequence++;
            record_error("edit canvas: "+std::string(error.what()));
        }
    }
}

inline void Engine::set_canvas_mode(CanvasMode mode){
    if(canvasMode==mode&&!(editCanvas&&editCanvas->coverDrag))return;
    if(editCanvas)editCanvas->coverDrag.reset();
    hide_editing_canvas();
    canvasMode=mode;
    if(configState)configState->stateSequence++;
    if(mode!=CanvasMode::Idle&&visual)visual->begin_edit();
}

inline void Engine::reconcile(bool synchronize){
    if(!configState)return;
    normal.clear();
    std::vector<std::unique_ptr<Surface>> updated;
    for(const auto& entry:configState->document["regions"]){
        std::unique_ptr<Surface> surface;
        for(auto& candidate:surfaces)if(candidate&&candidate->region.source.value("id",std::string())==entry["id"]){surface=std::move(candidate);break;}
        if(!surface)surface=std::make_unique<Surface>(renderer,Region(entry),false);
        else if(surface->region.source!=entry){
            json before=surface->region.source,after=entry;
            for(auto key:{"x","y","anchor_x","anchor_y","reference_width","reference_height"}){before.erase(key);after.erase(key);}
            if(before!=after)surface->release();
            surface->region=Region(entry);
        }
        updated.push_back(std::move(surface));
    }
    surfaces=std::move(updated);
    SetTimer(host,8,350,nullptr);
    if(synchronize)sync();
}

inline void Engine::hide_editing_canvas(){
    if(!editCanvas)return;
    if(editCanvas->coverDrag&&editCanvas->coverDrag->placing){
        editCanvas->coverDrag.reset();canvasMode=CanvasMode::Idle;
        editorRestoreSequence++;
        if(configState)configState->stateSequence++;
    }
    if(editCanvas->dragging()&&configState){editCanvas->cancel(*configState);gestureCancelSequence++;reconcile(false);}
    if(visual)visual->interrupt_reference();
    editCanvas->referencePicking=false;
    if(editCanvas->valid()&&GetCapture()==editCanvas->hwnd)ReleaseCapture();
    editCanvas->hide();
}

inline json Engine::editor_operation(const std::string& operation,const json& payload,DWORD pid){
    auto suspendCalibration=[this](){
        if(editing){
            calibrationResult.clear();
            HWND foreground=GetForegroundWindow();DWORD foregroundPid=process_id(foreground);
            bool handoff=foreground==target||foregroundPid==editorPid||foregroundPid==GetCurrentProcessId();
            calibrationEditorSuspended=true;if(visual)visual->exclusive=true;hide_editing_canvas();
            if(editor_window_valid())ShowWindow(editorHwnd,SW_HIDE);
            // This follows the user's confirm/retry click, never a background
            // timer. The editor grants foreground permission before sending it.
            if(handoff&&available(target))SetForegroundWindow(target);
            if(shell)shell->calibration_status(L"正在校准，请保持目标窗口在前台…");
            CancelWaitableTimer(frameTimer);animationTimer=false;configState->stateSequence++;
        }
        SetTimer(host,11,50,nullptr);
    };
    if(operation=="set_visual_check_interval"){
        if(!payload.at("seconds").is_number_integer())throw std::runtime_error("invalid_visual_check_interval");
        int before=configState->visual_check_interval(),seconds=payload.at("seconds").get<int>();
        configState->set_visual_check_interval(seconds);configState->save();
        if(visual&&before!=seconds)visual->each_region([&](auto& r){if(!r.refresh.active&&!r.refresh.requested)r.refresh.finish(visual_now(),seconds);});
    }else if(operation=="refresh_visual"){
        if(!visual||!available(target))throw std::runtime_error("target_unavailable");
        auto r=visual->region(payload.at("id").get<std::string>());
        if(!r||r->policy.locked())throw std::runtime_error("visual_position_not_ready");
        visual->request_refresh(*r,visual_now(),0);
        SetTimer(host,11,50,nullptr);
        if(process_id(GetForegroundWindow())==editorPid)SetForegroundWindow(target);
    }else if(operation=="set_position_mode"){
        std::string id=payload.at("id"),mode=payload.at("mode");
        if(mode=="anchor"){
            configState->switch_to_anchor(id,configState->revision,payload.value("use_saved_position",false));configState->save();
            if(visual){visual->deactivate_region(id);calibrationEditorSuspended=false;}
            reconcile();
        }else if(mode=="visual"){
            if(!available(target))throw std::runtime_error("target_unavailable");
            if(!visual)throw std::runtime_error("visual_worker_missing");
            int index=configState->index(id);if(index<0)throw std::runtime_error("region_not_found");
            if(configState->document["regions"][index]["position_mode"]=="visual")return json::object();
            auto document=configState->document;auto revision=configState->revision,persisted=configState->persistedRevision;
            visual->queue(id,false);
            try{configState->activate_visual(id,configState->revision);configState->save();}
            catch(...){
                configState->document=document;configState->revision=revision;configState->persistedRevision=persisted;
                visual->deactivate_region(id);throw;
            }
            reconcile(false);suspendCalibration();return {{"calibration_queued",true}};
        }else throw std::runtime_error("invalid_position_mode");
    }else if(operation=="retry_calibration"){
        if(!visual)throw std::runtime_error("visual_worker_missing");
        visual->retry_calibration();suspendCalibration();return {{"calibration_queued",true}};
    }else if(operation=="begin_cover_drag"){
        if(!editing||!editCanvas||!visual||!available(target)||!visual->stage.empty()||
            calibrationEditorSuspended||pickWaiting||editCanvas->picking)
            throw std::runtime_error("target_unavailable");
        auto id=payload.at("id").get<std::string>();
        int index=configState->index(id);
        if(index<0||configState->selected!=id)throw std::runtime_error("region_not_found");
        const auto& entry=configState->document["regions"][index];
        if(entry["position_mode"]!="visual")throw std::runtime_error("visual_mode_required");
        auto relation=entry.at("visual_binding").at("relation");
        auto previous=editCanvas->coverDrag;
        set_canvas_mode(CanvasMode::Cover);
        auto shown=visual->render_preview();
        const auto& r=shown.at("reference");
        VisualRect reference{r[0],r[1],r[2],r[3]};
        if(previous&&previous->id==id){reference=previous->reference;relation=previous->relation;}
        auto cover=visual_nominal(reference,relation);
        editCanvas->coverDrag=EditCanvas::CoverDrag{id,reference,cover,relation,true,false,0,0,cover};
        configState->stateSequence++;
        if(editor_window_valid())ShowWindow(editorHwnd,SW_HIDE);
        sync();
        if(editCanvas->valid()){SetForegroundWindow(editCanvas->hwnd);SetFocus(editCanvas->hwnd);}
    }else if(operation=="recalibrate_reference"){
        if(!available(target))throw std::runtime_error("target_unavailable");
        if(!visual||pickWaiting||(editCanvas&&editCanvas->picking))throw std::runtime_error("calibration_unavailable");
        visual->recalibrate_reference(payload.at("id"),payload.at("rect"));
        set_canvas_mode(CanvasMode::Idle);
        suspendCalibration();return {{"calibration_queued",true}};
    }else if(operation=="begin_calibration"){
        if(!available(target))throw std::runtime_error("target_unavailable");
        if(!visual||pickWaiting||(editCanvas&&editCanvas->picking))throw std::runtime_error("calibration_unavailable");
        set_canvas_mode(CanvasMode::Idle);
        visual->begin_edit();
        visual->begin_preview(payload.at("id"),payload.value("tracking_kind",std::string("boundary")),payload.value("search_limit_dip",500.));calibrationResult.clear();configState->selected=payload.at("id");
        if(editCanvas){editCanvas->referencePicking=true;if(editor_window_valid())ShowWindow(editorHwnd,SW_HIDE);}
        sync();
    }else if(operation=="set_calibration_reference"||operation=="update_calibration_preview"||
             operation=="confirm_calibration"||operation=="cancel_calibration"||operation=="pick_calibration_reference"){
        if(!visual)throw std::runtime_error("visual_worker_missing");
        auto token=payload.at("token").get<std::string>();visual->expect_preview(token);
        if(operation=="set_calibration_reference")visual->set_reference(token,payload.at("rect"));
        else if(operation=="update_calibration_preview")visual->set_preview_relation(token,payload.at("relation"));
        else if(operation=="confirm_calibration"){
            visual->confirm_preview(token);
            suspendCalibration();return {{"calibration_queued",true}};
        }
        else if(operation=="cancel_calibration")visual->cancel_preview();
        else {visual->referencePicking=true;visual->error.clear();configState->stateSequence++;}
        sync();
    }else if(operation=="set_visual_search_limit"){
        auto id=payload.at("id").get<std::string>();double limit=visual_number(payload.at("limit"),32,500);
        if(visual&&visual->stage=="preview")throw std::runtime_error("calibration_in_progress");
        configState->update_search_limit(id,limit,configState->revision);
        if(visual)if(auto r=visual->region(id)){r->policy.searchLimit=limit;r->policy.pause("search_settings_changed");r->search.reset();}
        reconcile();
    }else if(operation=="update_cover_relation"){
        if(visual&&visual->stage=="preview")throw std::runtime_error("calibration_in_progress");
        auto id=payload.at("id").get<std::string>();
        configState->update_cover_relation(id,payload.at("relation"),configState->revision);
        if(editCanvas&&editCanvas->coverDrag)set_canvas_mode(CanvasMode::Idle);
        if(visual)visual->pause_region(id,"relation_changed");
        reconcile();
    }else if(operation=="visual_calibrate"||operation=="visual_retry"){
        if(!visual)throw std::runtime_error("visual_worker_missing");
        visual->queue(payload.at("id"),operation=="visual_calibrate");
        SetTimer(host,11,50,nullptr);return {{"calibration_queued",true}};
    }else if(operation=="visual_anchor"){
        std::string id=payload.at("id");
        configState->switch_to_anchor(id,configState->revision,payload.value("use_saved_position",false));
        configState->save();
        if(visual){visual->deactivate_region(id);calibrationEditorSuspended=false;}
        reconcile();
    }else if(operation=="hello"){
        HWND window=reinterpret_cast<HWND>(payload.at("hwnd").get<uint64_t>());
        if(!IsWindow(window)||process_id(window)!=pid||pid!=editorPid)throw std::runtime_error("invalid_editor_window");
        editorHwnd=window;
        auto capabilities=payload.value("capabilities",json::array());
        editorOwnsShow=capabilities.is_array()&&
            std::find(capabilities.begin(),capabilities.end(),"editor_owns_show_v1")!=capabilities.end();
        report_issue(editorIssue,"");
        SetWindowLongPtrW(window,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(host));
    }else if(operation=="changed")reconcile();
    else if(operation=="select"){
        if(editCanvas&&editCanvas->coverDrag&&configState->selected!=editCanvas->coverDrag->id){
            set_canvas_mode(CanvasMode::Idle);sync();
        }
        if(visual&&visual->stage=="preview"&&configState->selected!=visual->id)visual->cancel_preview();
        edit_frame();
    }
    else if(operation=="commit_close"){
        if(visual&&visual->stage=="preview")visual->cancel_preview();
        saveError.clear();
        report_save_issue();
        if(payload.contains("editor_state")){
            const auto& value=payload["editor_state"];
            int page=value.value("page",0);std::string geometry=value.value("geometry",std::string());
            if(page<0||page>5||geometry.size()>16384)throw std::runtime_error("invalid_editor_state");
            editorState={{"page",page},{"geometry",geometry}};
        }
        end_editing();
    }else if(operation=="list_applications"){
        if(!editing||!applications)throw std::runtime_error("operation_unavailable");
        editorApplicationWindows.clear();
        auto entries=json::array();
        for(const auto& app:applications->document.at("applications"))if(ApplicationProfiles::listed(app))
            entries.push_back({{"id",app.at("id")},{"name",app.at("name")},{"windows",json::array()}});
        struct MenuSearch {Engine* engine;json* entries;};
        MenuSearch search{this,&entries};
        EnumWindows([](HWND window,LPARAM raw)->BOOL{
            auto& search=*reinterpret_cast<MenuSearch*>(raw);
            auto& engine=*search.engine;
            if(!engine.applications->accepts(window))return TRUE;
            auto identity=WindowIdentity::read(window);
            for(auto& entry:*search.entries){
                auto id=entry.at("id").get<std::string>();
                if(!engine.applications->matches(engine.applications->app(id),identity))continue;
                wchar_t title[101]{};GetWindowTextW(window,title,101);
                entry["windows"].push_back({{"index",engine.editorApplicationWindows.size()},
                    {"title",utf8(title) },
                    {"pid",identity.pid},{"bound",window==engine.target}});
                engine.editorApplicationWindows.push_back(std::move(identity));
                break;
            }
            return engine.editorApplicationWindows.size()<128;
        },reinterpret_cast<LPARAM>(&search));
        auto deletable=json::array();
        for(const auto& app:applications->document.at("applications"))
            deletable.push_back({{"id",app.at("id")},{"name",app.at("name")},{"executable",app.at("executable")},{"listed",ApplicationProfiles::listed(app)},
                                 {"deleting",app.value("deleting",false)}});
        auto active=applications->active();
        for(auto& entry:entries){
            const auto id=entry.at("id").get<std::string>();
            auto& profiles=entry["profiles"]=json::array();
            if(!independentWindows)continue;
            std::set<std::string> scopedProfiles;
            auto collect=[&](const json& regions){
                for(const auto& region:regions)if(region.is_object()){
                    auto profile=region.value("window_profile",std::string());
                    if(!profile.empty())scopedProfiles.insert(std::move(profile));
                }
            };
            bool scoped=regionScopes&&fs::exists(applications->scoped_config_path(id));
            if(scoped){
                if(id==active&&configState->persistent){
                    collect(configState->document.at("regions"));collect(configState->hiddenRegions);
                }else{
                    std::ifstream file(applications->scoped_config_path(id),std::ios::binary);
                    if(file){
                        auto saved=json::parse(file,nullptr,false);
                        if(saved.is_object()&&saved.contains("regions")&&saved["regions"].is_array())collect(saved["regions"]);
                    }
                }
            }
            if(id==active&&available(target)&&archived_window_profile(id,"legacy")){
                bool legacy=scopedProfiles.count("legacy")!=0;
                if(!scoped){
                    std::ifstream file(applications->window_config_path(id,"legacy"),std::ios::binary);
                    if(file){
                        auto saved=json::parse(file,nullptr,false);
                        legacy=saved.is_object()&&saved.contains("regions")&&saved["regions"].is_array()&&!saved["regions"].empty();
                    }
                }
                if(legacy)profiles.push_back({{"id","legacy"},{"title",u8"原有应用配置"},{"deleting",false}});
            }
            for(const auto& profile:applications->app(id).value("windows",json::array())){
                const auto key=profile.at("id").get<std::string>();
                bool deleting=profile.value("deleting",false);
                bool hasRegions=scopedProfiles.count(key)!=0;
                if(!scoped&&!deleting){
                    std::ifstream file(applications->window_config_path(id,key),std::ios::binary);
                    if(file){
                        auto saved=json::parse(file,nullptr,false);
                        hasRegions=saved.is_object()&&saved.contains("regions")&&saved["regions"].is_array()&&!saved["regions"].empty();
                    }
                }
                if(!hasRegions&&!deleting)continue;
                bool live=false;
                for(auto& window:entry.at("windows")){
                    const auto& identity=editorApplicationWindows[window.at("index").get<size_t>()];
                    auto binding=windowBindings.find(identity.window);
                    if(binding!=windowBindings.end()&&binding->second.application==id&&binding->second.profile==key&&
                        binding->second.identity.current()){
                        if(regionScopes||id!=active||key!=windowProfile)window["profile"]=key;
                        live=true;break;
                    }
                }
                if(live||profile_bound_elsewhere(id,key,nullptr))continue;
                profiles.push_back({{"id",key},{"title",profile.at("title")},{"deleting",deleting}});
            }
        }
        return {{"applications",std::move(entries)},{"active_application",active},
                {"deletable_applications",std::move(deletable)}};
    }else if(operation=="set_region_scope"){
        if(!regionScopes||!editing||!applications||!configState->persistent||!available(target)||
            (visual&&visual->stage=="preview"))throw std::runtime_error("operation_unavailable");
        auto id=payload.at("id").get<std::string>();
        if(configState->index(id)<0)throw std::runtime_error("region_not_found");
        std::string profile;
        if(payload.contains("index")){
            auto index=payload.at("index").get<size_t>();
            if(index>=editorApplicationWindows.size())throw std::runtime_error("invalid_target");
            auto identity=editorApplicationWindows[index];
            auto app=applications->active();
            if(!identity.current()||!applications->accepts(identity.window,true)||
                !applications->matches(applications->app(app),identity))throw std::runtime_error("invalid_target");
            auto binding=windowBindings.find(identity.window);
            if(binding!=windowBindings.end()&&binding->second.application==app&&binding->second.identity.current())
                profile=binding->second.profile;
            else{
                auto next=*applications;
                profile=next.create_window_profile(app,identity.window);
                identity.mark();
                if(!identity.current())throw std::runtime_error("invalid_target");
                next.remember_window(app,identity,profile,false);
                next.save();
                *applications=std::move(next);
                windowBindings[identity.window]={identity,app,profile};
            }
        }
        configState->set_region_scope(id,profile);
        if(visual&&configState->index(id)<0)visual->deactivate_region(id);
        reconcile();
    }else if(operation=="switch_application"){
        if(!editing||!applications)throw std::runtime_error("operation_unavailable");
        if(payload.contains("delete_window_profile")){
            if(!independentWindows)throw std::runtime_error("invalid_target");
            auto app=payload.at("id").get<std::string>();
            auto profile=payload.at("delete_window_profile").get<std::string>();
            if(profile=="legacy")throw std::runtime_error("invalid_target");
            try{
                if(payload.contains("index")){
                    auto index=payload.at("index").get<size_t>();
                    if(index>=editorApplicationWindows.size())throw std::runtime_error("invalid_target");
                    const auto& identity=editorApplicationWindows[index];
                    auto binding=windowBindings.find(identity.window);
                    if(!identity.current()||binding==windowBindings.end()||binding->second.application!=app||
                        binding->second.profile!=profile||!binding->second.identity.current())
                        throw std::runtime_error("invalid_target");
                    if(regionScopes){
                        bool current=app==applications->active()&&configState->persistent;
                        auto path=applications->scoped_config_path(app);
                        auto remove=[&](ConfigState& state){
                            auto candidate=state;
                            std::vector<std::string> ids;
                            for(const auto& region:candidate.document.at("regions"))
                                if(region.value("window_profile",std::string())==profile)ids.push_back(region.at("id"));
                            for(const auto& id:ids)candidate.erase(id,candidate.revision);
                            candidate.remove_region_scope(profile);
                            candidate.save();
                            state=std::move(candidate);
                        };
                        if(current){remove(*configState);reconcile();}
                        else if(fs::exists(path)){ConfigState scoped(path,true,"archive");remove(scoped);}
                    }else if(app==applications->active()&&profile==windowProfile){
                        throw std::runtime_error("invalid_target");
                    }else applications->delete_window_files(app,profile);
                }else delete_archived_window_profile(app,profile);
            }
            catch(const std::exception& error){
                record_error("window profile deletion: "+std::string(error.what()));
                throw std::runtime_error("window_profile_delete_failed");
            }
        }else if(payload.contains("window_profile")){
            auto active=payload.value("id",applications->active());
            auto identity=targetIdentity;
            if(payload.contains("index")){
                auto index=payload.at("index").get<size_t>();
                if(index>=editorApplicationWindows.size())throw std::runtime_error("invalid_target");
                identity=editorApplicationWindows[index];
            }else if(active!=applications->active())throw std::runtime_error("invalid_target");
            if(!independentWindows||active.empty()||!available(identity.window)||!identity.current()||
                !applications->matches(applications->app(active),identity))
                throw std::runtime_error("invalid_target");
            auto profile=payload.at("window_profile").get<std::string>();
            applications->window_config_path(active,profile);
            if(!archived_window_profile(active,profile))throw std::runtime_error("invalid_target");
            queue_application(active,identity,false,false,"",profile);
        }else if(payload.contains("delete_id")){
            auto id=payload.at("delete_id").get<std::string>();
            applications->app(id);
            queue_application("",{},false,false,id);
        }else if(payload.value("remove_current",false)){
            if(applications->active().empty())throw std::runtime_error("No active application");
            queue_application("",{},false,true);
        }else if(payload.value("pick",false))queue_application("",{},true);
        else{
            auto id=payload.at("id").get<std::string>();
            const auto& app=applications->app(id);
            if(!ApplicationProfiles::listed(app))throw std::runtime_error("Application is no longer listed");
            WindowIdentity identity;
            if(payload.contains("index")){
                auto index=payload.at("index").get<size_t>();
                if(index>=editorApplicationWindows.size())throw std::runtime_error("invalid_target");
                identity=editorApplicationWindows[index];
                if(!applications->accepts(identity.window)||!identity.current()||!applications->matches(app,identity))
                    throw std::runtime_error("invalid_target");
            }
            queue_application(id,identity);
        }
    }else if(operation=="select_target"){
        HWND window=reinterpret_cast<HWND>(payload.at("hwnd").get<uint64_t>());
        if(applications){
            if(applications->active().empty())throw std::runtime_error("invalid_target");
            auto identity=WindowIdentity::read(window);
            if(!applications->accepts(window)||!identity.current()||!applications->matches(applications->app(applications->active()),identity))
                throw std::runtime_error("invalid_target");
            if(independentWindows)queue_application(applications->active(),identity);
            else bind_window(identity);
        }else{
            if(!available(window)||(!diagnostic&&(!is_chatgpt(window)||GetWindow(window,GW_OWNER))))throw std::runtime_error("invalid_target");
            hide_editing_canvas();target=window;targetPinned=true;targetPid=process_id(window);configState->stateSequence++;
        }
        sync();
    }else if(operation=="set_canvas_mode"){
        if(!editing||(!applications&&!(editCanvas&&editCanvas->coverDrag)))throw std::runtime_error("operation_unavailable");
        auto mode=payload.at("mode").get<std::string>();
        if(mode=="idle"){
            if(editCanvas&&editCanvas->coverDrag&&editCanvas->coverDrag->placing)editorRestoreSequence++;
            set_canvas_mode(CanvasMode::Idle);
        }
        else{
            if(mode!="once"&&mode!="free")throw std::runtime_error("invalid_canvas_mode");
            if(!available(target)||calibrationEditorSuspended||pickWaiting||
                (editCanvas&&editCanvas->picking)||(visual&&visual->stage=="preview"))
                throw std::runtime_error("target_unavailable");
            if(!simulateFocus)SetForegroundWindow(target);
            set_canvas_mode(mode=="once"?CanvasMode::Once:CanvasMode::Free);
        }
        sync();
    }else if(operation=="pick_color"){
        std::string id=payload.at("id").get<std::string>();
        auto field=payload.value("field",std::string("background"));
        if(field!="color"&&field!="background")throw std::runtime_error("invalid_pick_field");
        if(!editing||!editCanvas||configState->index(id)<0)throw std::runtime_error("region_not_found");
        if(!available(target)||calibrationEditorSuspended||(visual&&visual->stage=="preview"))
            throw std::runtime_error("target_unavailable");
        set_canvas_mode(CanvasMode::Idle);
        pickField=std::move(field);
        pickId=id;pickSession=pipe->nonce;pickWaiting=false;pickError.clear();editCanvas->picking=true;
        configState->stateSequence++;
        if(!simulateFocus)SetForegroundWindow(target);
        if(editor_window_valid())ShowWindow(editorHwnd,SW_HIDE);
        sync();
        if(editCanvas->valid()&&IsWindowVisible(editCanvas->hwnd)){
            SetForegroundWindow(editCanvas->hwnd);SetFocus(editCanvas->hwnd);
        }
    }
    return json::object();
}

inline void Engine::begin_editing(){
    if(editorProcess.value&&WaitForSingleObject(editorProcess.value,0)==WAIT_TIMEOUT){
        if(IsWindow(editorHwnd)&&process_id(editorHwnd)==editorPid)SetForegroundWindow(editorHwnd);
        return;
    }
    editor_tick();
    if(!configState)throw std::runtime_error("Configuration is unavailable");
    if(configState->document.dump().size()>900000)throw std::runtime_error("Configuration is too large for an editing session (900 KB limit)");
    pipe=std::make_unique<PipeServer>(host,0);
    editorApplicationWindows.clear();
    protocol=std::make_unique<SessionProtocol>(*configState,pipe->nonce);
    protocol->extraState=[this](){bool picking=pickWaiting||(editCanvas&&editCanvas->picking)||(visual&&visual->stage=="preview"&&visual->referencePicking);
        auto drag=editCanvas&&editCanvas->coverDrag?&*editCanvas->coverDrag:nullptr;
        return json{{"visible",editorVisible&&!picking&&!calibrationEditorSuspended&&!(drag&&drag->placing)},{"picking",picking},{"target_hwnd",reinterpret_cast<uintptr_t>(target)},
        {"engine_pid",GetCurrentProcessId()},{"owner_hwnd",reinterpret_cast<uintptr_t>(host)},{"editor_state",editorState},{"finish_sequence",finishSequence},
        {"application_id",configState->applicationId},{"binding_epoch",configState->bindingEpoch},
        {"application_name",applications?(applications->active().empty()?json("未选择应用"):
            applications->app(applications->active()).at("name")):json("ChatGPT")},
        {"canvas_modes",bool(applications)},
        {"region_scopes",regionScopes},
        {"canvas_mode",drag?(drag->placing?"cover":"cover_draft"):canvasMode==CanvasMode::Free?"free":canvasMode==CanvasMode::Once?"once":"idle"},
        {"cover_drag_relation",drag&&!drag->placing?drag->relation:json(nullptr)},
        {"diagnostics_dir",utf8((configState->path.parent_path()/L"diagnostics").wstring())},
        {"editor_restore_sequence",editorRestoreSequence},{"waiting_for_target",!available(target)},
        {"awaiting_window_choice",awaiting_window_choice()},
        {"save_error",saveError},{"render_error",renderError},{"pick_error",pickError},
        {"visual_error",visual?visual->selected_error():""},{"visual_worker_available",visual&&visual->component_available()},
        {"calibration_preview",visual?visual->preview_state():json(nullptr)},
        {"visual_capabilities",{"visual_geometry_v1","visual_tracking_v1","visual_calibration_preview_v1","feature_position_v1"}},
        {"calibration_retry_available",visual&&visual->can_retry_calibration()},
        {"calibration_busy",calibrationEditorSuspended},
        {"calibration_result",configState->selected==calibrationResultOwner?calibrationResult:""},
        {"editing_preview",visual?visual->render_preview():json(nullptr)},
        {"focus_text_sequence",focusTextSequence},{"canvas_hwnd",editCanvas?reinterpret_cast<uintptr_t>(editCanvas->hwnd):0}};};
    protocol->extension=[this](const std::string& op,const json& payload,DWORD pid){return editor_operation(op,payload,pid);};
    std::wstring executable,command;
    if(!editorScript.empty()){
        if(pythonExecutable.empty()||!fs::exists(pythonExecutable)||!fs::exists(editorScript))throw std::runtime_error("Development editor paths are missing");
        executable=pythonExecutable.wstring();command=command_arg(executable)+L" "+command_arg(editorScript.wstring());
    }else{
        if(!fs::exists(editorExecutable))throw std::runtime_error("The packaged editor is missing");
        executable=editorExecutable.wstring();command=command_arg(executable);
    }
    command+=L" --pipe "+command_arg(pipe->name)+L" --session "+command_arg(wide(pipe->nonce));
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,
        nullptr,editorScript.empty()?editorExecutable.parent_path().c_str():editorScript.parent_path().c_str(),&startup,&process))
        throw std::runtime_error("Cannot launch the property editor");
    editorProcess.reset(process.hProcess);WinHandle thread(process.hThread);editorPid=process.dwProcessId;
    pipe->expectedPid=editorPid;
    // These counters belong to the new pipe session. Carrying an old finish
    // sequence into hello would immediately close the newly launched editor.
    finishSequence=0;focusTextSequence=0;editorRestoreSequence=0;editorOwnsShow=false;calibrationResult.clear();
    if(visual)visual->begin_edit();
    editing=true;expectedEditorExit=false;editorVisible=true;lastRelated=GetTickCount64();
    editCanvas=std::make_unique<EditCanvas>();
    if(configState->selected.empty()&&!configState->document["regions"].empty())configState->selected=configState->document["regions"][0]["id"];
    configState->stateSequence++;
    AllowSetForegroundWindow(editorPid);
    if(ResumeThread(thread.value)==DWORD(-1)){TerminateProcess(editorProcess.value,1);throw std::runtime_error("Cannot resume the editor");}
    SetTimer(host,5,100,nullptr);sync();
}

inline void Engine::request_finish(){
    if(!editing)return;
    finishSequence++;configState->stateSequence++;
}

inline void Engine::end_editing(){
    set_canvas_mode(CanvasMode::Idle);
    if(calibrationEditorSuspended){if(visual)visual->abort_calibration();calibrationEditorSuspended=false;}
    editing=false;expectedEditorExit=true;editorVisible=false;configState->stateSequence++;
    if(sessionSavePending){ShutdownBlockReasonDestroy(host);sessionSavePending=false;pendingNotice="覆盖修改已保存，可以重新执行关机或注销。";}
    KillTimer(host,7);pickWaiting=false;pickId.clear();pickSession.clear();pickError.clear();
    editCanvas.reset();sync();
}

inline void Engine::editor_tick(){
    if(!editorProcess.value||WaitForSingleObject(editorProcess.value,0)==WAIT_TIMEOUT)return;
    if(editing){
        set_canvas_mode(CanvasMode::Idle);
        if(calibrationEditorSuspended){if(visual)visual->abort_calibration();calibrationEditorSuspended=false;}
        if(visual&&visual->stage=="preview")visual->cancel_preview();
        hide_editing_canvas();
        try{configState->save();}
        catch(const std::exception& error){
            saveError=error.what();configState->stateSequence++;SetTimer(host,8,1000,nullptr);
            report_save_issue();
            record_error("editor recovery save: "+std::string(error.what()));
        }
        pendingNotice=saveError.empty()?"编辑器已退出，覆盖恢复鼠标穿透。":"编辑器已退出；未保存的修改仍在后台，将继续重试保存。";
        editing=false;editorVisible=false;editCanvas.reset();
    }
    FILETIME created{},ended{},kernel{},user{};
    if(GetProcessTimes(editorProcess.value,&created,&ended,&kernel,&user)){
        auto duration=[](FILETIME value){return double((uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime)/1e7;};
        retiredEditorCpu+=duration(kernel)+duration(user);
    }
    pipe.reset();protocol.reset();editorProcess.reset();editorPid=0;editorHwnd=nullptr;
    KillTimer(host,5);
    if(!shutdownRequested&&application_action_pending()){
        finish_application_action();return;
    }
    sync();
    if(shutdownRequested)PostMessageW(host,WM_APP+55,0,0);
    else if(reopenEditor){reopenEditor=false;PostMessageW(host,WM_APP+56,0,0);}
}

inline LRESULT Engine::canvas_message(HWND window,UINT message,WPARAM w,LPARAM l){
    if(cancelGesturePending&&(message==WM_MOUSEMOVE||message==WM_LBUTTONUP||message==WM_LBUTTONDOWN)){
        cancelGesturePending=false;hide_editing_canvas();return 0;
    }
    auto& canvas=*editCanvas;
    if(message==WM_NCHITTEST)return canvas_input()?HTCLIENT:HTTRANSPARENT;
    if(message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL){
        POINT screen{SHORT(LOWORD(l)),SHORT(HIWORD(l))};
        auto bounds=client_box(target);
        if(canvas_input()&&available(target)&&PtInRect(&bounds,screen)){
            HWND recipient=target;
            GUITHREADINFO info{sizeof(info)};
            if(GetGUIThreadInfo(GetWindowThreadProcessId(target,nullptr),&info)&&IsChild(target,info.hwndFocus))
                recipient=info.hwndFocus;
            PostMessageW(recipient,message,w,l);
        }
        return 0;
    }
    if(message==WM_PAINT){
        PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);
        if(dc)FillRect(dc,&paint.rcPaint,HBRUSH(GetStockObject(BLACK_BRUSH)));
        EndPaint(window,&paint);return 0;
    }
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_SETCURSOR){
        SetCursor(LoadCursorW(nullptr,canvasMode==CanvasMode::Cover?IDC_SIZEALL:
            (canvas.picking||canvas.referencePicking)?IDC_CROSS:IDC_ARROW));return TRUE;
    }
    if(!configState)return DefWindowProcW(window,message,w,l);
    float x=SHORT(LOWORD(l))/canvas.scale,y=SHORT(HIWORD(l))/canvas.scale;
    if(canvasMode==CanvasMode::Cover&&canvas.coverDrag){
        auto& drag=*canvas.coverDrag;
        if(message==WM_LBUTTONDOWN){
            const auto& box=drag.cover;
            if(x>=box.x-6&&x<=box.x+box.width+6&&y>=box.y-6&&y<=box.y+box.height+6){
                drag.startX=x;drag.startY=y;drag.held=true;
                SetFocus(window);SetCapture(window);
            }
            return 0;
        }
        if(drag.held&&(message==WM_MOUSEMOVE||message==WM_LBUTTONUP)){
            drag.cover.x=std::clamp(drag.original.x+x-drag.startX,0.,std::max(0.,configState->viewportWidth-drag.cover.width));
            drag.cover.y=std::clamp(drag.original.y+y-drag.startY,0.,std::max(0.,configState->viewportHeight-drag.cover.height));
            if(message==WM_MOUSEMOVE){edit_frame();return 0;}
            try{drag.relation=visual_relation_at_cover(drag.relation,drag.reference,drag.cover);}
            catch(const std::exception&){
                set_canvas_mode(CanvasMode::Idle);editorRestoreSequence++;configState->stateSequence++;sync();return 0;
            }
            drag.held=false;drag.placing=false;canvasMode=CanvasMode::Idle;
            if(GetCapture()==window)ReleaseCapture();
            editorRestoreSequence++;configState->stateSequence++;sync();return 0;
        }
        if(message==WM_CANCELMODE||(message==WM_CAPTURECHANGED&&drag.held)||
            (message==WM_KEYDOWN&&w==VK_ESCAPE)){
            set_canvas_mode(CanvasMode::Idle);editorRestoreSequence++;configState->stateSequence++;sync();return 0;
        }
        if(message==WM_LBUTTONUP||message==WM_LBUTTONDBLCLK||message==WM_KEYDOWN)return 0;
    }
    if(message==WM_LBUTTONDOWN){
        if(!canvas_input())return 0;
        if(canvas.picking){
            pickPoint={SHORT(LOWORD(l)),SHORT(HIWORD(l))};ClientToScreen(window,&pickPoint);
            canvas.picking=false;pickWaiting=true;canvas.hide();
            if(editor_window_valid())ShowWindow(editorHwnd,SW_HIDE);
            CancelWaitableTimer(frameTimer);animationTimer=false;
            configState->stateSequence++;SetTimer(host,7,120,nullptr);return 0;
        }
        SetFocus(window);SetCapture(window);canvas.begin(*configState,x,y,canvasMode==CanvasMode::Once);edit_frame();return 0;
    }
    if((message==WM_MOUSEMOVE||message==WM_LBUTTONUP)&&canvas.dragging()){
        canvas.motion(*configState,x,y,(canvas.inputBox.right-canvas.inputBox.left)/canvas.scale,
            (canvas.inputBox.bottom-canvas.inputBox.top)/canvas.scale);
        if(message==WM_MOUSEMOVE){reconcile();return 0;}
        if(canvas.mode==EditCanvas::Reference&&visual&&visual->stage=="preview"){
            json rect={std::min(canvas.startX,canvas.currentX),std::min(canvas.startY,canvas.currentY),
                std::abs(canvas.currentX-canvas.startX),std::abs(canvas.currentY-canvas.startY)};
            try{visual->set_reference(visual->previewToken,rect);}
            catch(const std::exception& e){visual->error=e.what();visual->referencePicking=false;}
            canvas.end(*configState);canvas.referencePicking=false;
            if(GetCapture()==window)ReleaseCapture();sync();return 0;
        }
        auto revision=configState->revision;
        canvas.end(*configState);if(GetCapture()==window)ReleaseCapture();
        if(canvasMode==CanvasMode::Once&&configState->revision!=revision)set_canvas_mode(CanvasMode::Idle);
        reconcile();return 0;
    }
    if((message==WM_CAPTURECHANGED&&canvas.dragging())||message==WM_CANCELMODE){
        hide_editing_canvas();sync();return 0;
    }
    if(message==WM_LBUTTONDBLCLK){
        if(canvas_input()){focusTextSequence++;configState->stateSequence++;if(IsWindow(editorHwnd))SetForegroundWindow(editorHwnd);}
        return 0;
    }
    if(message==WM_KEYDOWN){
        if(!canvas_input())return 0;
        if(w==VK_ESCAPE){
            if(visual&&visual->stage=="preview"){
                if(canvas.dragging())canvas.cancel(*configState);
                if(GetCapture()==window)ReleaseCapture();visual->cancel_preview();canvas.referencePicking=false;sync();return 0;
            }
            if(canvas.picking||pickWaiting){
                KillTimer(host,7);canvas.picking=false;pickWaiting=false;pickId.clear();pickSession.clear();configState->stateSequence++;sync();
            }else if(canvasMode!=CanvasMode::Idle){set_canvas_mode(CanvasMode::Idle);sync();}
            else if(canvas.dragging()){canvas.cancel(*configState);ReleaseCapture();reconcile();}
            else request_finish();
            return 0;
        }
        int index=configState->index(configState->selected);
        if(index>=0&&w==VK_DELETE){configState->erase(configState->selected,configState->revision);reconcile();return 0;}
        if(index>=0&&(w==VK_LEFT||w==VK_RIGHT||w==VK_UP||w==VK_DOWN)){
            Region region=configState->positioned(index);float step=GetKeyState(VK_SHIFT)<0?10.f:1.f;
            if(region.visual)return 0;
            configState->update_positioned(configState->selected,{{"x",std::max(0.f,region.x+step*(int(w==VK_RIGHT)-int(w==VK_LEFT)))},
                {"y",std::max(0.f,region.y+step*(int(w==VK_DOWN)-int(w==VK_UP)))}},configState->revision);
            reconcile();return 0;
        }
    }
    return DefWindowProcW(window,message,w,l);
}

inline void Engine::complete_pick(){
    if(!pickWaiting)return;
    pickWaiting=false;
    if(!editing||!editCanvas||!pipe||pipe->nonce!=pickSession||configState->index(pickId)<0){pickId.clear();pickSession.clear();sync();return;}
    auto bounds=client_box(target);
    bool valid=available(target)&&PtInRect(&bounds,pickPoint)&&GetAncestor(WindowFromPoint(pickPoint),GA_ROOT)==target;
    HDC desktop=valid?GetDC(nullptr):nullptr;
    COLORREF color=desktop?GetPixel(desktop,pickPoint.x,pickPoint.y):CLR_INVALID;
    if(desktop)ReleaseDC(nullptr,desktop);
    if(color!=CLR_INVALID){
        char value[8];sprintf_s(value,"#%02x%02x%02x",GetRValue(color),GetGValue(color),GetBValue(color));
        configState->update(pickId,{{pickField,std::string(value)}},configState->revision);
    }else pickError="target_pixel_unavailable";
    report_issue(pickIssue,color==CLR_INVALID?"pick_failed":"");
    pickId.clear();pickSession.clear();configState->stateSequence++;reconcile();
}
