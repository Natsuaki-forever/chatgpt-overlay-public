#pragma once

inline void reference_picker_tests(Engine& engine,const fs::path& output){
    fs::create_directories(output);
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"OverlayReferenceCancelTarget";
    RegisterClassW(&wc);
    engine.target=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Overlay reference interruption test",WS_OVERLAPPEDWINDOW,
        100,100,700,450,nullptr,nullptr,wc.hInstance,nullptr);
    require(engine.target!=nullptr,"cannot create reference test target");ShowWindow(engine.target,SW_SHOWNOACTIVATE);
    engine.diagnostic=true;engine.simulateFocus=true;engine.editing=true;engine.editorVisible=true;
    engine.configState=std::make_unique<ConfigState>(output/L"config.json");
    auto& state=*engine.configState;
    auto id=state.create({{"x",40},{"y",40},{"width",100},{"height",30}},state.revision);state.selected=id;state.save();
    engine.visual=std::make_unique<VisualRuntime>(state,engine.normal,engine.surfaces,engine.host);
    engine.editCanvas=std::make_unique<EditCanvas>();engine.reconcile();
    const auto targetBox=client_box(engine.target),inputBox=client_box(engine.editCanvas->hwnd);
    const auto previewPixels=engine.editCanvas->pixels();
    require(EqualRect(&targetBox,&inputBox),"edit input does not cover the full target client area");
    require(previewPixels>0&&previewPixels<size_t(targetBox.right-targetBox.left)*(targetBox.bottom-targetBox.top),"small edit preview allocated a full-client bitmap");
    auto finishBefore=engine.finishSequence;
    engine.open_or_restore_editor();engine.open_or_restore_editor();
    require(engine.finishSequence==finishBefore&&engine.editing&&engine.editorRestoreSequence==2,"repeated open incorrectly finished editing");
    auto original=state.document;auto revision=state.revision;
    auto begin=[&](){
        engine.visual->begin_preview(id);engine.sync();
        auto hwnd=engine.editCanvas->hwnd;
        auto point=MAKELPARAM(int(20*engine.editCanvas->scale),int(20*engine.editCanvas->scale));
        SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,point);
        require(GetCapture()==hwnd&&engine.editCanvas->mode==EditCanvas::Reference,"reference gesture did not capture mouse");
    };
    auto interrupted=[&](){
        require(!engine.editCanvas->dragging()&&!engine.visual->referencePicking&&!engine.editCanvas->referencePicking,"reference interruption kept picking gate");
        require(engine.visual->selected_error()=="reference_selection_interrupted","reference interruption missing explanation");
        require(engine.editorVisible&&!engine.calibrationEditorSuspended&&!engine.finishSequence,"reference interruption finished or hid editor");
        require(state.document==original&&state.revision==revision&&ConfigState(state.path).document==original,"reference interruption wrote configuration");
        engine.visual->cancel_preview();
    };
    begin();
    // Transfer real OS capture to another HWND on this thread: Windows sends
    // WM_CAPTURECHANGED to the actual canvas procedure synchronously.
    SetCapture(engine.target);interrupted();ReleaseCapture();
    begin();SendMessageW(engine.editCanvas->hwnd,WM_CANCELMODE,0,0);interrupted();
    begin();engine.hide_editing_canvas();interrupted();
    // A successful retry must reach confirmation, without silently activating.
    begin();auto token=engine.visual->previewToken;
    SendMessageW(engine.editCanvas->hwnd,WM_LBUTTONUP,0,
        MAKELPARAM(int(160*engine.editCanvas->scale),int(100*engine.editCanvas->scale)));
    require(!engine.visual->referencePicking&&engine.visual->previewHasReference&&engine.visual->error.empty(),"retry did not return valid confirmation preview");
    require(state.document==original&&state.revision==revision,"reference retry auto-confirmed");
    engine.visual->confirm_preview(token);
    require(engine.visual->stage=="queued","reference retry could not confirm");
    engine.visual->abort_calibration();
    engine.visual->begin_preview(id);
    engine.visual->set_reference(engine.visual->previewToken,{20,20,140,80});
    engine.visual->confirm_preview(engine.visual->previewToken);
    engine.calibrationEditorSuspended=true;engine.visual->exclusive=true;
    engine.visual->queuedAt=visual_now()-6;
    engine.visual_tick();
    require(!engine.calibrationEditorSuspended&&engine.calibrationResult=="failed"&&!engine.finishSequence,
        "calibration wait timeout did not restore editor with failure");
    require(engine.visual->selected_error()=="calibration_wait_expired"&&state.document==original,
        "calibration timeout lost reason or modified original configuration");
    std::ofstream(output/L"result.json")<<json{{"passed",true},{"real_capture_transfer",true},{"cancel_mode",true},
        {"canvas_hidden",true},{"original_configuration_preserved",true},{"retry_and_confirm",true},{"calibration_timeout_returns_failure",true}}.dump(2);
    engine.end_editing();engine.visual.reset();DestroyWindow(engine.target);engine.target=nullptr;
}

inline void lifecycle_tests(Engine& engine,const fs::path& output){
    fs::create_directories(output);
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"OverlayNativeTestTarget";
    RegisterClassW(&wc);
    auto target=[&](){
        HWND result=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Overlay lifecycle test",WS_OVERLAPPEDWINDOW,
            100,100,500,320,nullptr,nullptr,wc.hInstance,nullptr);
        require(result!=nullptr,"cannot create lifecycle target");ShowWindow(result,SW_SHOWNOACTIVATE);return result;
    };
    engine.diagnostic=true;
    engine.target=target();
    engine.surfaces.emplace_back(std::make_unique<Surface>(engine.renderer,Region(json{{"text","窗口重建 ABC"},{"scroll",true}})));
    engine.sync();
    HWND old=engine.surfaces[0]->hwnd;
    require(IsWindowVisible(old),"initial lifecycle surface hidden");
    DestroyWindow(engine.target);
    require(!IsWindow(old),"owned surface survived target destruction");
    engine.sync();
    require(!engine.animationTimer,"animation continued without target");
    HWND foreign=target();
    engine.surfaces[0]->hwnd=foreign;  // Simulate a recycled HWND with a different identity.
    engine.sync();
    require(IsWindowVisible(foreign),"stale surface handle hid an unrelated window");
    engine.surfaces[0]->hwnd=old;DestroyWindow(foreign);
    engine.target=target();engine.sync();
    auto& surface=*engine.surfaces[0];
    require(IsWindow(surface.hwnd)&&IsWindowVisible(surface.hwnd),"surface was not recreated");
    require(GetWindow(surface.hwnd,GW_OWNER)==engine.target,"recreated surface has wrong owner");
    require(engine.animationTimer,"animation did not restart");
    auto exercise=[&](){for(int i=0;i<200;i++){
        SetWindowPos(engine.target,nullptr,100+i%10,100+i%7,480+i%3*20,300+i%2*20,SWP_NOACTIVATE|SWP_NOZORDER);
        engine.sync();
        if(i%10==0){surface.release();engine.sync();}
        MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }};
    exercise();
    DWORD before=0,after=0;GetProcessHandleCount(GetCurrentProcess(),&before);
    DWORD gdi=GetGuiResources(GetCurrentProcess(),0);
    exercise();
    GetProcessHandleCount(GetCurrentProcess(),&after);
    json report={{"target_recreated",true},{"resource_rebuilt",true},{"foreign_handle_guard",true},
        {"rounds",2},{"geometry_cycles_per_round",200},{"handles_before",before},{"handles_after",after},
        {"gdi_before",gdi},{"gdi_after",GetGuiResources(GetCurrentProcess(),0)},{"errors",engine.errors}};
    std::ofstream(output/L"result.json")<<report.dump(2);
    require(after<=before+2,"native lifecycle handles accumulated");
    require(GetGuiResources(GetCurrentProcess(),0)<=gdi+2,"native lifecycle GDI accumulated");
    require(engine.errors==0,"native lifecycle rendering failed");
    engine.normal.clear();engine.surfaces.clear();
    for(int i=0;i<10;i++)engine.surfaces.emplace_back(std::make_unique<Surface>(engine.renderer,
        Region(json{{"x",i*800},{"y",i%2*300},{"width",40},{"height",30},{"text",""}}),false));
    auto sparse=plan_surfaces(engine.surfaces,10000,800,1);
    require(sparse.size()<=8,"group window count exceeds eight");
    require(sparse.size()>1,"distant regions expanded to one full-width surface");
    engine.surfaces.clear();
    engine.surfaces.emplace_back(std::make_unique<Surface>(engine.renderer,
        Region(json{{"x",10},{"y",10},{"width",80},{"height",40},{"text",""},{"background","#ff0000"},{"opacity",128}}),false));
    engine.surfaces.emplace_back(std::make_unique<Surface>(engine.renderer,
        Region(json{{"x",40},{"y",20},{"width",80},{"height",40},{"text",""},{"background","#0000ff"},{"opacity",128}}),false));
    engine.sync();
    require(engine.normal.plan.size()==1&&engine.normal.windows[0],"overlapping regions were not grouped");
    auto& group=*engine.normal.windows[0];
    float scale=GetDpiForWindow(engine.target)/96.f;
    auto pixel=[&](int x,int y){return reinterpret_cast<DWORD*>(group.image.bits)[size_t(int(std::round(y*scale)))*group.image.width+int(std::round(x*scale))];};
    require(pixel(45,15)==0xc0400080,"group alpha composition changed region order");
    require(pixel(5,5)==0x80800000,"group background pixel changed");
    require(!engine.surfaces[0]->hwnd&&!engine.surfaces[1]->hwnd,"group retained per-region windows");
    HWND grouped=group.hwnd;
    require((GetWindowLongPtrW(grouped,GWL_EXSTYLE)&(WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOPMOST))==(WS_EX_TRANSPARENT|WS_EX_NOACTIVATE),"group input or z-order style changed");
    DestroyWindow(engine.target);engine.sync();engine.target=target();engine.sync();
    require(engine.normal.windows[0]->valid()&&IsWindowVisible(engine.normal.windows[0]->hwnd),"group did not recover after target recreation");
    auto outputBytes=engine.renderer.surfaceBytes;
    engine.normal.clear();
    require(engine.renderer.surfaceBytes<outputBytes,"group output budget did not release");
    report["groups"]={{"sparse_count",sparse.size()},{"overlap_alpha_order",true},{"no_hidden_region_windows",true},{"target_recreation",true},{"budget_released",true}};
    engine.surfaces.clear();
    engine.configState=std::make_unique<ConfigState>(output/L"cancel-gesture.json");
    engine.configState->create({{"x",40},{"y",40},{"width",160},{"height",40}},engine.configState->revision);
    engine.editing=true;engine.simulateFocus=true;engine.editCanvas=std::make_unique<EditCanvas>();engine.reconcile();
    auto position=[&](int x,int y){return MAKELPARAM(int(std::round(x*engine.editCanvas->scale)),int(std::round(y*engine.editCanvas->scale)));};
    SendMessageW(engine.editCanvas->hwnd,WM_LBUTTONDOWN,MK_LBUTTON,position(50,50));
    SendMessageW(engine.editCanvas->hwnd,WM_MOUSEMOVE,MK_LBUTTON,position(90,90));
    require(engine.editCanvas->dragging(),"gesture cancellation test did not start drag");
    engine.cancelGesturePending=true;
    SendMessageW(engine.editCanvas->hwnd,WM_LBUTTONUP,0,position(110,110));
    require(!engine.editCanvas->dragging()&&engine.gestureCancelSequence>0,"pending cancellation did not precede synchronous mouse input");
    require(engine.configState->document["regions"][0]["x"]==40&&engine.configState->document["regions"][0]["y"]==40,
        "mouse-up committed a drag after cancellation was latched");
    report["pending_cancel_before_mouseup"]=true;
    engine.end_editing();
    std::ofstream(output/L"result.json")<<report.dump(2);
    DestroyWindow(engine.target);engine.sync();
}
