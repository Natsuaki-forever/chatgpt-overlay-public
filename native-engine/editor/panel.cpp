#include "../src/core.hpp"
#include "../src/visual_config.hpp"
#include "options.hpp"
#include "theme.hpp"
#include "chrome.hpp"
#include "popup.hpp"
#include <commdlg.h>
#include <imm.h>
#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <tuple>
#include <utility>

constexpr UINT resultMessage=WM_APP+1;
constexpr UINT resizePaintMessage=WM_APP+2;
constexpr UINT_PTR presentationTimer=1;
constexpr UINT_PTR referenceHoverTimer=2;
enum Control {List=100,Text,Font,Size,Apply,Done,Status,ThemeChoice,AddRegion,DeleteRegion,
    RegionsLabel=110,TextLabel,FontLabel,SizeLabel,PickColor,MoveToTarget,RegionScroll,PickTextColor,FreeRegion,ClearRegions,
    VisualKind=400,VisualDistance,VisualInterval,VisualBegin,VisualConfirm,VisualCancel,VisualReselect,
    VisualRetry,VisualAnchor,VisualResume,VisualRefresh,VisualHint,VisualKindLabel,VisualDistanceLabel,
    VisualIntervalLabel,VisualSaveDistance,
    RelationMode=418,RelationModeLabel,RelationX,RelationY,RelationXLabel,RelationYLabel,
    RelationPadding,RelationPaddingLabel,DetailApply,DetailValue=450,DetailLabel=460,
    AppTitle=500,ConnectionStatus,Appearance,Settings,ClosePanel,Sidebar,RegionTitle,SaveState,
    PositionAdvanced=509,ReferenceStatus=512,EffectScroll,
    BoldButton=531,VisualCheckTab=540,VisualRelationTab,VisualReferenceTab,CoverDrag,PositionContext,
    StepButton=3000,TrackControl=4000,ToggleControl=5000};
constexpr int checkIntervals[]={0,15,30,60};
constexpr int sliders[]={212,215,216,218,220,224,225,227};
constexpr int switches[]={213,217,219,222,226};
constexpr int slider_units(int id){return id==220||id==227?10:1;}
struct Command {std::string op;json payload;bool close=false;std::optional<uint64_t> baseRevision;};
struct Reply {std::string op;json value;};
struct PaintBatch {
    HWND window;
    HDC dc;
    // Keep the composited paint open until the new control layout is ready.
    explicit PaintBatch(HWND owner):window(owner),dc(GetDC(owner)){}
    ~PaintBatch(){if(dc)ReleaseDC(window,dc);}
};
struct Panel {
    HWND window=nullptr;
    bool resizePaintPending=false;
    struct EdgeDrag { RECT bounds; POINT origin; int edge; HWND outline; };
    std::optional<EdgeDrag> edgeDrag;
    HWND tooltips=nullptr;
    std::wstring tooltipText;
    HWND targetWindow=nullptr;
    HWND hoveredButton=nullptr;
    ULONGLONG referenceHoverTime=0;
    double referenceHoverFrom=0;
    std::wstring targetAppLabel,targetWindowLabel;
    std::wstring pipe;
    std::string token;
    json regions=json::array();
    json effectiveRects=json::object(),preview;
    json visualDetailSource,relationForm,referenceForm,coverDragRelation;
    std::array<double,4> detailValues{};
    std::string selected;
    std::string loadedText;
    std::wstring loadedSize;
    std::vector<std::wstring> loadedOptions;
    Page page=Typography;
    Page paintedPage=PageCount;
    Theme theme;
    PanelPopup popup;
    std::string expandedPickerApp,pickerAction;
    json pickerSnapshot;
    bool fontsLoaded=false;
    bool visualAvailable=false,waitingTarget=false,calibrationBusy=false,canRetry=false;
    std::string calibrationResult,calibrationError;
    bool featureReference=false;
    int visualDetail=0;
    bool sidebarOpen=true,sidebarShown=true,syncing=false;
    bool keyboardFocus=false;
    bool calibrationWarning=false;
    int regionWheelDelta=0,regionScrollOffset=0;
    int hoveredRegionAction=-1;
    struct FieldFrame {int id;RECT bounds;bool color;};
    std::vector<FieldFrame> frames;
    RECT railBounds{},paneBounds{};
    struct Backdrop {
        HDC dc=nullptr;
        HBITMAP bitmap=nullptr;
        HGDIOBJ previous=nullptr;
        SIZE size{};
        unsigned revision=0;
        void reset(){
            if(dc&&previous)SelectObject(dc,previous);
            if(bitmap)DeleteObject(bitmap);
            if(dc)DeleteDC(dc);
            dc=nullptr;bitmap=nullptr;previous=nullptr;size={};
        }
        ~Backdrop(){reset();}
    } backdrop;
    int checkInterval=15;
    DWORD enginePid=0;
    uint64_t restoreSequence=0,focusSequence=0;
    uint64_t displayedRevision=0,finishSequence=0;
    bool connected=false,busy=false,finishRequested=false,applicationMenuAvailable=false;
    bool listingRegionTargets=false,pickerManagement=false;
    std::string scopeRegion;
    RECT scopeMenuAnchor{};
    bool hasApplication=false;
    bool canvasModes=false;
    bool regionScopes=false;
    std::string canvasMode="idle";
    std::string pendingSelection;
    std::mutex lock;
    std::condition_variable ready;
    std::queue<Command> commands;
    bool stopping=false;
    HFONT font=nullptr;
    HFONT headerFont=nullptr,sectionFont=nullptr,mutedFont=nullptr,strongFont=nullptr,previewFont=nullptr,brandFont=nullptr;
    UINT fontDpi=0;
    SIZE layoutSize{};
    UINT layoutDpi=0;
    std::vector<int> tabOrder;
    bool presentationPending=false,activateOnPresent=false,focusOnPresent=false;

    void submit(Command command){
        {std::lock_guard<std::mutex> guard(lock);commands.push(std::move(command));}
        ready.notify_one();
    }
    void reply(std::string op,json value){
        auto data=std::make_unique<Reply>(Reply{std::move(op),std::move(value)});
        if(PostMessageW(window,resultMessage,0,reinterpret_cast<LPARAM>(data.get())))data.release();
    }
    void work(){
        using Handle=std::unique_ptr<void,decltype(&CloseHandle)>;
        try{
            HANDLE opened=CreateFileW(pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
            if(opened==INVALID_HANDLE_VALUE)throw std::runtime_error("open local session");
            Handle connection(opened,CloseHandle),signal(CreateEventW(nullptr,TRUE,FALSE,nullptr),CloseHandle);
            DWORD mode=PIPE_READMODE_MESSAGE;
            if(!signal||!SetNamedPipeHandleState(connection.get(),&mode,nullptr,nullptr))
                throw std::runtime_error("configure local session");
            uint64_t id=0,revision=0,sequence=0;
            std::vector<char> buffer(1024*1024);
            auto request=[&](const std::string& op,const json& payload,std::optional<uint64_t> base=std::nullopt){
                auto data=json{{"protocol",1},{"session",token},{"request_id",++id},
                    {"op",op},{"payload",payload},{"base_revision",base.value_or(revision)}}.dump();
                ResetEvent(signal.get());OVERLAPPED transfer{};transfer.hEvent=signal.get();DWORD count=0;
                if(!TransactNamedPipe(connection.get(),data.data(),DWORD(data.size()),
                    buffer.data(),DWORD(buffer.size()),&count,&transfer)){
                    if(GetLastError()!=ERROR_IO_PENDING)throw std::runtime_error("local session exchange");
                    if(WaitForSingleObject(signal.get(),2500)!=WAIT_OBJECT_0){
                        CancelIoEx(connection.get(),&transfer);
                        GetOverlappedResult(connection.get(),&transfer,&count,TRUE);
                        throw std::runtime_error("local session timeout");
                    }
                    if(!GetOverlappedResult(connection.get(),&transfer,&count,FALSE))
                        throw std::runtime_error("local session response");
                }
                auto response=json::parse(buffer.data(),buffer.data()+count);
                if(response.at("request_id")!=id)throw std::runtime_error("wrong response identifier");
                revision=response.value("revision",revision);
                if(response.contains("state"))sequence=response.at("state").at("state_sequence");
                return response;
            };
            auto initial=request("hello",{{"hwnd",reinterpret_cast<uintptr_t>(window)},
                {"capabilities",{"visual_geometry_v1","visual_tracking_v1","visual_calibration_preview_v1","editor_owns_show_v1"}}});
            if(!initial.value("ok",false))throw std::runtime_error("local session rejected");
            reply("hello",std::move(initial));
            for(;;){
                std::unique_lock<std::mutex> guard(lock);
                ready.wait_for(guard,std::chrono::milliseconds(150),[&]{return stopping||!commands.empty();});
                if(stopping)return;
                if(commands.empty()){
                    guard.unlock();
                    auto update=request("snapshot",{{"state_sequence",sequence}});
                    if(update.contains("state")||!update.value("ok",false))reply("snapshot",std::move(update));
                    continue;
                }
                auto command=std::move(commands.front());commands.pop();guard.unlock();
                auto result=request(command.op,command.payload,command.baseRevision);
                if(command.close&&result.value("ok",false)){
                    auto committed=request("commit_close",json::object());
                    bool done=committed.value("ok",false);
                    reply("commit_close",std::move(committed));
                    if(done)return;
                }else reply(command.op,std::move(result));
            }
        }catch(const std::exception& error){reply("failure",{{"detail",error.what()}});}
    }
    HWND control(int id)const{return GetDlgItem(window,id);}
    void status(const std::wstring& text){set_text(Status,text);}
    void calibration_notice(const wchar_t* text,bool warning=false){
        bool colorChanged=calibrationWarning!=warning;
        calibrationWarning=warning;
        if(!set_text(ReferenceStatus,text)&&colorChanged)InvalidateRect(control(ReferenceStatus),nullptr,FALSE);
    }
    static const wchar_t* calibration_error(const std::string& code){
        if(code=="target_unavailable"||code=="invalid_target")return L"请打开或还原目标窗口，再重新框选。";
        if(code=="capture_permission_revoked"||code=="calibration_wait_expired")return L"请把目标窗口切到前台，再重试校准。";
        if(code=="boundary-unobservable-or-ambiguous")return L"未找到完整边框，请重新框选整个外框。";
        if(code=="feature_reference_too_small")return L"参考区域太小，请稍微扩大选区。";
        if(code=="feature_reference_too_large"||code=="ValueError:template-pixel-budget-exceeded"||code=="visual_roi_budget_exceeded")
            return L"参考区域太大，请缩小选区。";
        if(code=="ValueError:insufficient-stable-features"||code=="ambiguous-features"||code=="features-lost")
            return L"请改选更清晰、独特的文字或图案。";
        if(code=="visual_worker_missing"||code=="visual_worker_component_mismatch")return L"识别组件缺失或版本不符，请从完整目录运行。";
        if(code=="visual_worker_timeout"||code=="visual_worker_exited")return L"识别组件暂时不可用，请重试。";
        if(code=="Cannot replace configuration")return L"校准保存失败。请检查配置文件后重试。";
        if(code=="cover_outside_client"||code=="invalid_visual_cover_size")return L"请调整遮盖尺寸或偏移后重试。";
        return L"视觉定位未完成，原设置已保留。请重试或重新框选。";
    }
    void show_calibration_state(){
        const auto& result=calibrationResult;
        const auto& error=calibrationError;
        if(calibrationBusy)calibration_notice(L"正在校准，请保持目标窗口在前台。");
        else if(page==Visual&&visualDetail==1&&canvasMode=="cover_draft")
            calibration_notice(L"拖动结果已预览。保存遮盖关系，或按 Esc 取消。");
        else if(waitingTarget&&page==Visual&&visualDetail==1&&visual_region())
            calibration_notice(L"可以保存遮盖关系。选定目标窗口后查看效果。");
        else if(waitingTarget)calibration_notice(L"请打开目标窗口，或从顶部应用菜单选择窗口。");
        else if(!error.empty()&&error!="calibration_cancelled")calibration_notice(calibration_error(error),true);
        else if(page==Visual&&visualDetail==2&&visual_region()&&!preview.is_object()&&visualAvailable)
            calibration_notice(L"调整参考框后，使用当前窗口内容重新校准。");
        else if(result=="saved")calibration_notice(L"校准已保存。点「完成」后开始跟踪。");
        else if(result=="cancelled"||error=="calibration_cancelled")calibration_notice(L"已取消校准，原设置已保留。");
        else if(preview.is_object()&&preview.at("reference").is_array())
            calibration_notice(L"参考框已选好，可确认或重新框选。");
        else if(preview.is_object())calibration_notice(L"在目标窗口拖动，框选参考内容。");
        else if(!visualAvailable)calibration_notice(L"识别组件不可用，请从完整目录运行。",true);
        else if(selected.empty())calibration_notice(L"请选择一个覆盖区域。");
        else if(page==Visual&&visualDetail==1&&visual_region()&&featureReference)
            calibration_notice(L"文字／图案跟踪使用固定尺寸。可调整基准、偏移和遮盖尺寸。");
        else if(page==Visual&&visualDetail==1)calibration_notice(L"选择参考区域后，即可调整遮盖关系。");
        else if(page==Visual&&visualDetail==2)calibration_notice(L"选择参考区域后，即可微调参考框。");
        else if(visual_region())calibration_notice(L"视觉定位已启用，可重新定位或重选参考框。");
        else calibration_notice(L"当前使用窗口锚定。选好参考区域后可启用视觉定位。");
    }
    static bool effect_page(Page page){return page==Scroll||page==Outline||page==Glow;}
    static const Option* option_at(int id){
        auto it=std::find_if(std::begin(options),std::end(options),[&](const Option& option){return option.id==id;});
        return it==std::end(options)?nullptr:&*it;
    }
    std::wstring contents(int id)const{
        auto item=control(id);int count=GetWindowTextLengthW(item);
        std::wstring value(size_t(count)+1,L'\0');
        value.resize(GetWindowTextW(item,value.data(),count+1));return value;
    }
    bool set_text(int id,const std::wstring& value){
        if(contents(id)==value)return false;
        SetWindowTextW(control(id),value.c_str());return true;
    }
    void select_choice(int id,LRESULT row){
        auto combo=control(id);
        auto previous=SendMessageW(combo,CB_GETCURSEL,0,0);
        if(previous==row)return;
        SendMessageW(combo,CB_SETCURSEL,row,0);
        update_option_chrome(id);
    }
    bool selecting()const{return !pendingSelection.empty();}
    bool interaction_locked()const{return busy||selecting();}
    bool block_pending_input(const MSG& msg)const{
        if(!interaction_locked()||!IsChild(window,msg.hwnd)||msg.hwnd==control(ClosePanel)||
            (!connected&&msg.hwnd==control(Done)))return false;
        switch(msg.message){
            case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_SYSCHAR:
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            case WM_MOUSEWHEEL: case WM_CONTEXTMENU: case WM_PASTE: case WM_CUT: case WM_CLEAR:
            case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION: return true;
            default: return false;
        }
    }
    auto selected_region()const{
        return std::find_if(regions.begin(),regions.end(),[&](const json& r){return r.at("id")==selected;});
    }
    std::string form_text()const{
        auto text=contents(Text);
        for(size_t pos=0;(pos=text.find(L"\r\n",pos))!=std::wstring::npos;)text.erase(pos,1);
        return utf8(text);
    }
    static std::wstring number_text(double value){
        auto text=std::to_wstring(value);
        text.erase(text.find_last_not_of(L'0')+1);
        if(text.back()==L'.')text.pop_back();
        return text;
    }
    static std::wstring detail_number_text(double value){
        wchar_t buffer[64]{};
        swprintf_s(buffer,L"%.2f",value);
        std::wstring text=buffer;
        while(text.size()>1&&text.back()==L'0')text.pop_back();
        if(!text.empty()&&text.back()==L'.')text.pop_back();
        return text;
    }
    std::wstring option_value(const Option& option)const{
        if(option.kind==OptionKind::Choice)
            return std::to_wstring(SendMessageW(control(option.id),CB_GETCURSEL,0,0));
        return contents(option.id);
    }
    bool visual_region()const{
        auto region=selected_region();
        return region!=regions.end()&&region->at("position_mode")=="visual";
    }
    void load_fonts(){
        if(fontsLoaded)return;
        fontsLoaded=true;
        auto combo=control(Font);
        std::vector<std::wstring> families;
        LOGFONTW query{};query.lfCharSet=DEFAULT_CHARSET;
        HDC dc=GetDC(window);
        if(dc){
            EnumFontFamiliesExW(dc,&query,[](const LOGFONTW* font,const TEXTMETRICW*,DWORD,LPARAM data)->int{
                if(font->lfFaceName[0]!=L'@')reinterpret_cast<std::vector<std::wstring>*>(data)->push_back(font->lfFaceName);
                return 1;
            },reinterpret_cast<LPARAM>(&families),0);
            ReleaseDC(window,dc);
        }
        // Include the localized names recognized by the engine's DirectWrite renderer.
        Com<IDWriteFactory> factory;Com<IDWriteFontCollection> collection;
        if(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.put())))&&SUCCEEDED(factory->GetSystemFontCollection(collection.put(),FALSE))){
            for(UINT32 i=0;i<collection->GetFontFamilyCount();i++){
                Com<IDWriteFontFamily> family;Com<IDWriteLocalizedStrings> names;
                if(FAILED(collection->GetFontFamily(i,family.put()))||FAILED(family->GetFamilyNames(names.put())))continue;
                for(UINT32 j=0;j<names->GetCount();j++){
                    UINT32 count=0;
                    if(FAILED(names->GetStringLength(j,&count))||!count)continue;
                    std::wstring name(size_t(count)+1,L'\0');
                    if(SUCCEEDED(names->GetString(j,name.data(),count+1))&&name[0]!=L'@'){
                        name.resize(count);families.push_back(std::move(name));
                    }
                }
            }
        }
        std::sort(families.begin(),families.end());families.erase(std::unique(families.begin(),families.end()),families.end());
        for(const auto& name:families){
            LPARAM text=reinterpret_cast<LPARAM>(name.c_str());
            if(SendMessageW(combo,CB_FINDSTRINGEXACT,WPARAM(-1),text)==CB_ERR)SendMessageW(combo,CB_ADDSTRING,0,text);
        }
    }
    bool dirty()const{
        auto region=selected_region();
        if(region==regions.end())return false;
        if(form_text()!=loadedText||utf8(contents(Font))!=region->at("font")||contents(Size)!=loadedSize)return true;
        for(size_t i=0;i<std::size(options);i++)
            if(option_value(options[i])!=loadedOptions[i])return true;
        return false;
    }
    void set_busy(bool pending){
        busy=pending;
        bool usable=connected||!busy;
        for(int id:{List,Done})EnableWindow(control(id),usable);
        EnableWindow(control(ConnectionStatus),usable&&applicationMenuAvailable);
        bool enabled=usable&&selected_region()!=regions.end();
        for(int id:{Text,Font,Size,Apply})EnableWindow(control(id),enabled);
        EnableWindow(control(BoldButton),enabled);
        auto enable_steps=[&](int id,bool value){
            EnableWindow(control(StepButton+2*id),value);
            EnableWindow(control(StepButton+2*id+1),value);
        };
        for(const auto& option:options){
            bool field=enabled&&(option.page!=Position||!visual_region());
            EnableWindow(control(option.id),field);
            if(option.kind==OptionKind::Color)EnableWindow(control(option.id+optionPicker),field);
            if(option.id>=205&&option.id<=208)enable_steps(option.id,field);
            if(std::find(std::begin(sliders),std::end(sliders),option.id)!=std::end(sliders))EnableWindow(control(TrackControl+option.id),field);
            if(std::find(std::begin(switches),std::end(switches),option.id)!=std::end(switches))EnableWindow(control(ToggleControl+option.id),field);
        }
        EnableWindow(control(AddRegion),usable&&!preview.is_object()&&(!canvasModes||!waitingTarget));
        EnableWindow(control(FreeRegion),usable&&!preview.is_object()&&!waitingTarget);
        EnableWindow(control(ClearRegions),usable&&!preview.is_object()&&!regions.empty());
        EnableWindow(control(DeleteRegion),enabled&&!preview.is_object());
        for(int id:{PickColor,PickTextColor})EnableWindow(control(id),enabled&&!waitingTarget&&!preview.is_object());
        update_visual_buttons();
    }
    void show_region(){
        auto it=selected_region();
        set_busy(busy);
        syncing=true;
        if(it==regions.end()){
            set_text(Text,L"");set_text(RegionTitle,L"选择覆盖区域");syncing=false;update_actions();return;
        }
        auto text=wide(it->at("text").get<std::string>());
        for(size_t pos=0;(pos=text.find(L'\n',pos))!=std::wstring::npos;pos++)
            if(!pos||text[pos-1]!=L'\r'){text.insert(pos,1,L'\r');pos++;}
        set_text(Text,text);
        auto title=text.substr(0,text.find_first_of(L"\r\n"));
        set_text(RegionTitle,title.empty()?L"空白遮盖":title);
        loadedText=form_text();
        auto family=wide(it->at("font").get<std::string>());
        if(contents(Font)!=family){
            auto combo=control(Font);
            auto row=SendMessageW(combo,CB_FINDSTRINGEXACT,WPARAM(-1),reinterpret_cast<LPARAM>(family.c_str()));
            if(row==CB_ERR)row=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(family.c_str()));
            SendMessageW(combo,CB_SETCURSEL,row,0);
        }
        loadedSize=std::to_wstring(int(it->at("size").get<double>()));
        set_text(Size,loadedSize);
        loadedOptions.clear();
        auto effective=effectiveRects.find(selected);
        for(const auto& option:options){
            const auto& stored=it->at(option.key);
            if(option.kind==OptionKind::Choice){
                auto value=stored.is_boolean()?(stored.get<bool>()?"true":"false"):stored.get<std::string>();
                for(int i=0;option.choices[i].label;i++)if(value==option.choices[i].value){
                    select_choice(option.id,i);
                    break;
                }
            }else{
                const auto& value=option.page==Position&&effective!=effectiveRects.end()&&effective->is_object()&&
                    effective->contains(option.key)?effective->at(option.key):stored;
                std::wstring text;
                if(option.kind==OptionKind::Color)text=wide(value.get<std::string>());
                else{
                    auto number=value.get<double>();
                    if(option.id>=205&&option.id<=208)number=std::round(number*100)/100;
                    text=number_text(number);
                }
                if(set_text(option.id,text))update_option_chrome(option.id);
            }
            loadedOptions.push_back(option_value(option));
        }
        syncing=false;update_actions();
    }
    void mark_selected_row(const std::string& id){
        auto row=std::find_if(regions.cbegin(),regions.cend(),[&](const json& region){return region.at("id")==id;});
        auto index=row==regions.cend()?-1:std::distance(regions.cbegin(),row);
        auto list=control(List);
        if(SendMessageW(list,LB_GETCURSEL,0,0)!=index)SendMessageW(list,LB_SETCURSEL,index,0);
    }
    void state(const json& value,bool forceForm){
        displayedRevision=value.at("revision");
        const auto& nextRegions=value.at("regions");
        auto nextRects=value.value("effective_rects",json::object());
        bool rebuild=!connected||regions!=nextRegions;
        bool geometryChanged=effectiveRects!=nextRects;
        if(rebuild)regions=nextRegions;
        effectiveRects=std::move(nextRects);
        auto previous=selected;
        const auto& chosen=value.at("selected_id");
        selected=chosen.is_string()?chosen.get<std::string>():std::string();
        if(selected.empty()&&!regions.empty())selected=regions.front().at("id").get<std::string>();
        auto list=control(List);
        if(rebuild){
            auto priorTop=SendMessageW(list,LB_GETTOPINDEX,0,0);
            SendMessageW(list,LB_RESETCONTENT,0,0);
            for(const auto& region:regions){
                auto label=wide(region.at("text").get<std::string>());
                auto newline=label.find_first_of(L"\r\n");if(newline!=std::wstring::npos)label.resize(newline);
                if(label.empty())label=L"空白区域";
                SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            }
            mark_selected_row(selected);
            if(previous==selected&&!regions.empty())SendMessageW(list,LB_SETTOPINDEX,priorTop,0);
            update_region_scroll();
        }else if(previous!=selected)mark_selected_row(selected);
        if(rebuild||previous!=selected||geometryChanged||forceForm)show_region();
        if(previous!=selected){
            auto region=selected_region();int distance=500;
            if(region!=regions.end()&&region->at("visual_binding").is_object()){
                const auto& binding=region->at("visual_binding");
                distance=binding.value("search_limit_dip",500);
                select_choice(VisualKind,binding.value("tracking_kind",std::string("boundary"))=="boundary");
            }
            set_text(VisualDistance,std::to_wstring(distance));
        }
    }
    int visible_region_rows()const{
        HWND list=control(List);
        RECT bounds{};GetClientRect(list,&bounds);
        int rowHeight=int(SendMessageW(list,LB_GETITEMHEIGHT,0,0));
        return rowHeight>0?std::max(1,int(bounds.bottom)/rowHeight):1;
    }
    int region_scroll_max()const{
        return std::max(0,int(regions.size())-visible_region_rows());
    }
    void update_region_scroll(){
        auto bar=control(RegionScroll);if(!bar)return;
        auto limit=region_scroll_max();
        auto rows=visible_region_rows();
        bool changed=SendMessageW(bar,TBM_GETRANGEMAX,0,0)!=limit;
        if(changed)SendMessageW(bar,TBM_SETRANGEMAX,FALSE,limit);
        if(SendMessageW(bar,TBM_GETPAGESIZE,0,0)!=rows)SendMessageW(bar,TBM_SETPAGESIZE,0,rows);
        auto top=int(SendMessageW(control(List),LB_GETTOPINDEX,0,0));
        if(top>limit)scroll_regions(limit);
        else set_region_scroll_position(bar,top);
        bool visible=sidebarShown&&limit>0;
        if(bool(GetWindowLongPtrW(bar,GWL_STYLE)&WS_VISIBLE)!=visible)ShowWindow(bar,visible?SW_SHOWNA:SW_HIDE);
        if(changed)InvalidateRect(bar,nullptr,FALSE);
    }
    void set_region_scroll_position(HWND bar,LRESULT top){
        if(SendMessageW(bar,TBM_GETPOS,0,0)==top)return;
        SendMessageW(bar,TBM_SETPOS,FALSE,top);
        InvalidateRect(bar,nullptr,FALSE);
    }
    void scroll_regions(int top){
        auto list=control(List);
        int next=std::clamp(top,0,region_scroll_max());
        if(next!=SendMessageW(list,LB_GETTOPINDEX,0,0))SendMessageW(list,LB_SETTOPINDEX,next,0);
    }
    RECT region_scroll_thumb(HWND bar)const{
        RECT box{};GetClientRect(bar,&box);
        auto dpi=GetDpiForWindow(bar);
        int inset=chrome::px(5,dpi),halfWidth=chrome::px(3,dpi);
        int trackHeight=std::max(1,int(box.bottom)-2*inset);
        int height=std::clamp(MulDiv(trackHeight,visible_region_rows(),std::max(1,int(regions.size()))),
            std::min(trackHeight,chrome::px(24,dpi)),trackHeight);
        int pos=int(SendMessageW(control(List),LB_GETTOPINDEX,0,0));
        int y=inset+MulDiv(trackHeight-height,pos,std::max(1,region_scroll_max()));
        return {box.right/2-halfWidth,y,box.right/2+halfWidth,y+height};
    }
    void drag_region_scroll(HWND bar,int mouseY){
        RECT box{};GetClientRect(bar,&box);
        int inset=chrome::px(5,GetDpiForWindow(bar));
        auto thumb=region_scroll_thumb(bar);
        int travel=std::max(1,int(box.bottom)-2*inset-int(thumb.bottom-thumb.top));
        scroll_regions(MulDiv(std::clamp(mouseY-regionScrollOffset-inset,0,travel),region_scroll_max(),travel));
    }
    static RECT region_action_rect(RECT row,UINT dpi){
        auto px=[dpi](int value){return chrome::px(value,dpi);};
        auto center=(row.top+row.bottom)/2;
        return {row.right-px(33),center-px(14),row.right-px(5),center+px(14)};
    }
    int region_action_at(HWND list,POINT point)const{
        if(!regionScopes)return -1;
        auto hit=SendMessageW(list,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
        int row=LOWORD(hit);
        if(HIWORD(hit)||size_t(row)>=regions.size())return -1;
        RECT bounds{};
        if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&bounds))==LB_ERR)return -1;
        auto action=region_action_rect(bounds,GetDpiForWindow(list));
        return PtInRect(&action,point)?row:-1;
    }
    void hover_region_action(HWND list,int row){
        if(row==hoveredRegionAction)return;
        for(int affected:{hoveredRegionAction,row}){
            RECT bounds{};
            if(affected>=0&&SendMessageW(list,LB_GETITEMRECT,affected,reinterpret_cast<LPARAM>(&bounds))!=LB_ERR)
                InvalidateRect(list,&bounds,FALSE);
        }
        hoveredRegionAction=row;
    }
    static LRESULT CALLBACK region_list_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR context){
        auto& panel=*reinterpret_cast<Panel*>(context);
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,region_list_proc,id);
        if(msg==WM_ERASEBKGND){
            RECT box{};GetClientRect(hwnd,&box);
            panel.paint_backdrop(reinterpret_cast<HDC>(wp),box,hwnd);
            return 1;
        }
        if(msg==WM_MOUSEMOVE&&panel.regionScopes){
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0};TrackMouseEvent(&tracking);
            POINT point{SHORT(LOWORD(lp)),SHORT(HIWORD(lp))};
            panel.hover_region_action(hwnd,panel.region_action_at(hwnd,point));
        }else if(msg==WM_MOUSELEAVE||msg==WM_MOUSEWHEEL||msg==LB_RESETCONTENT||msg==LB_SETTOPINDEX){
            panel.hover_region_action(hwnd,-1);
        }
        if(msg==WM_SETCURSOR&&panel.regionScopes){
            POINT point{};
            if(GetCursorPos(&point)&&ScreenToClient(hwnd,&point)&&panel.region_action_at(hwnd,point)>=0){
                SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;
            }
        }
        if(msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_LBUTTONDBLCLK){
            POINT point{SHORT(LOWORD(lp)),SHORT(HIWORD(lp))};
            if(panel.region_action_at(hwnd,point)>=0){
                if(msg==WM_LBUTTONUP){
                    ClientToScreen(hwnd,&point);
                    panel.region_menu(MAKELPARAM(point.x,point.y));
                }
                return 0;
            }
        }
        if(msg==WM_MOUSEWHEEL&&panel.region_scroll_max()>0){
            UINT lines=0;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
            panel.regionWheelDelta+=GET_WHEEL_DELTA_WPARAM(wp);
            int steps=panel.regionWheelDelta/WHEEL_DELTA;
            panel.regionWheelDelta%=WHEEL_DELTA;
            if(steps&&lines){
                int rows=lines==WHEEL_PAGESCROLL?panel.visible_region_rows():int(lines);
                int top=int(SendMessageW(hwnd,LB_GETTOPINDEX,0,0));
                panel.scroll_regions(top-steps*rows);
            }
            return 0;
        }
        auto result=DefSubclassProc(hwnd,msg,wp,lp);
        if(msg==WM_KEYDOWN||msg==WM_LBUTTONUP||msg==LB_SETTOPINDEX||msg==LB_SETCURSEL){
            auto bar=panel.control(RegionScroll);
            if(bar)panel.set_region_scroll_position(bar,SendMessageW(hwnd,LB_GETTOPINDEX,0,0));
        }
        return result;
    }
    void update_visual_buttons(){
        auto it=selected_region();
        bool usable=!busy;
        bool available=it!=regions.end()&&visualAvailable&&!waitingTarget&&usable&&!calibrationBusy;
        bool detailAvailable=it!=regions.end()&&visualAvailable&&usable&&!calibrationBusy;
        bool hasPreview=preview.is_object(),hasReference=hasPreview&&preview.at("reference").is_array();
        bool tracking=visual_region();
        for(int id:{VisualKind,VisualDistance,VisualBegin})EnableWindow(control(id),available&&!hasPreview);
        EnableWindow(control(VisualInterval),usable&&!calibrationBusy&&!hasPreview&&visualAvailable);
        EnableWindow(control(VisualConfirm),available&&hasReference);
        for(int id:{VisualCancel,VisualReselect})EnableWindow(control(id),available&&hasPreview);
        for(int id:{VisualAnchor,VisualRefresh})EnableWindow(control(id),available&&!hasPreview&&tracking);
        EnableWindow(control(VisualSaveDistance),available&&!hasPreview&&tracking&&
            it->at("visual_binding").value("tracking_kind",std::string("boundary"))=="features");
        EnableWindow(control(VisualRetry),available&&!hasPreview&&canRetry);
        EnableWindow(control(VisualResume),available&&!hasPreview&&!tracking&&it->at("visual_binding").is_object());
        bool detailed=detailAvailable&&(hasReference||(!hasPreview&&tracking));
        // Relation values can be edited while the target is temporarily gone. The
        // saved reference itself needs a live target when it is committed.
        EnableWindow(control(DetailApply),detailed&&referenceForm.is_array()&&
            (visualDetail==1||available));
        EnableWindow(control(CoverDrag),detailed&&visualDetail==1&&available&&!hasPreview);
        EnableWindow(control(RelationMode),detailed&&!featureReference);
        bool fixed=relationForm.is_object()&&relationForm.at("mode")=="position_only";
        EnableWindow(control(RelationX),detailed&&fixed);EnableWindow(control(RelationY),detailed&&fixed);
        EnableWindow(control(RelationPadding),detailed&&!fixed);
        for(int i=0;i<4;i++){
            EnableWindow(control(DetailValue+i),detailed);
            EnableWindow(control(StepButton+2*(DetailValue+i)),detailed);
            EnableWindow(control(StepButton+2*(DetailValue+i)+1),detailed);
        }
        EnableWindow(control(StepButton+2*RelationPadding),detailed&&!fixed);
        EnableWindow(control(StepButton+2*RelationPadding+1),detailed&&!fixed);
    }
    static VisualRect visual_rect(const json& values){return {values.at(0),values.at(1),values.at(2),values.at(3)};}
    void paint_detail_values(){
        std::array<double,4> numbers{};
        bool fixed=relationForm.is_object()&&relationForm.at("mode")=="position_only";
        const wchar_t* offsets[]={L"X",L"Y",L"宽",L"高"};
        const wchar_t* margins[]={L"左边距",L"上边距",L"右边距",L"下边距"};
        const wchar_t* reference[]={L"参考 X",L"参考 Y",L"参考宽",L"参考高"};
        auto labels=visualDetail==2?reference:fixed?offsets:margins;
        if(visualDetail==2&&referenceForm.is_array())for(int i=0;i<4;i++)numbers[i]=referenceForm[i];
        else if(visualDetail==1&&relationForm.is_object()){
            if(fixed){
                for(int i=0;i<2;i++){numbers[i]=relationForm["offset_dip"][i];numbers[i+2]=relationForm["fixed_size_dip"][i];}
            }else for(int i=0;i<4;i++)numbers[i]=relationForm["margins_dip"][i];
            select_choice(RelationMode,fixed?0:1);
            for(auto [id,key,choices]:{std::tuple<int,const char*,const Choice*>{RelationX,"anchor_x",horizontalChoices},
                {RelationY,"anchor_y",verticalChoices}}){
                for(int i=0;choices[i].label;i++)if(relationForm.at(key)==choices[i].value)select_choice(id,i);
            }
            set_text(RelationPadding,number_text(relationForm.at("padding_physical")));
        }
        for(int i=0;i<4;i++){
            detailValues[i]=numbers[i];
            set_text(DetailLabel+i,labels[i]);
            set_text(DetailValue+i,referenceForm.is_array()?detail_number_text(numbers[i]):L"");
        }
    }
    void load_visual_detail(bool force=false){
        if(visualDetail==0)return;
        json detail;
        if(preview.is_object()&&preview.at("reference").is_array()){
            detail={{"reference",preview.at("reference")},{"relation",preview.at("relation")},{"kind",preview.at("tracking_kind")},
                {"id",preview.at("id")},{"token",preview.at("token")}};
        }else if(!preview.is_object()&&visual_region()){
            const auto& binding=selected_region()->at("visual_binding");
            detail={{"reference",binding.at("reference_rect_dip")},{"relation",binding.at("relation")},
                {"kind",binding.value("tracking_kind",std::string("boundary"))},{"id",selected}};
        }
        if(force||detail!=visualDetailSource){
            bool hadReference=referenceForm.is_array();
            visualDetailSource=detail;
            referenceForm=detail.is_object()?detail.at("reference"):json();
            relationForm=detail.is_object()?detail.at("relation"):json();
            if(visualDetail==1&&coverDragRelation.is_object()&&referenceForm.is_array())relationForm=coverDragRelation;
            featureReference=detail.is_object()&&detail.at("kind")=="features";
            paint_detail_values();
            if(page==Visual&&hadReference!=referenceForm.is_array())layout();
            else show_page();
        }
    }
    std::optional<double> detail_number(int id,double minimum,double maximum){
        auto raw=contents(id);wchar_t* end=nullptr;
        double value=std::wcstod(raw.c_str(),&end);
        if(raw.empty()||*end||!std::isfinite(value)||value<minimum||value>maximum)return std::nullopt;
        return value;
    }
    void rebase_relation(int id){
        if(!referenceForm.is_array()||!relationForm.is_object())return;
        auto index=SendMessageW(control(id),CB_GETCURSEL,0,0);
        if(index<0)return;
        auto reference=visual_rect(referenceForm);
        auto relation=relationForm;
        if(id==RelationMode)relation["mode"]=index?"follow_bounds":"position_only";
        else if(id==RelationX)relation["anchor_x"]=horizontalChoices[index].value;
        else relation["anchor_y"]=verticalChoices[index].value;
        try{relationForm=visual_relation_at_cover(relation,reference,visual_nominal(reference,relationForm));}
        catch(const std::exception&){status(L"此基准无法保留当前位置，请先调整遮盖尺寸");}
        paint_detail_values();layout();update_visual_buttons();
    }
    void apply_visual_detail(){
        if(!IsWindowEnabled(control(DetailApply)))return;
        std::array<double,4> values{};
        bool margins=visualDetail==1&&relationForm.at("mode")=="follow_bounds";
        for(int i=0;i<4;i++){
            double low=margins?-4096:i>=2?8:visualDetail==2?0:-100000;
            auto value=contents(DetailValue+i)==detail_number_text(detailValues[i])?
                std::optional<double>(detailValues[i]):detail_number(DetailValue+i,low,margins?4096:100000);
            if(!value){calibration_notice(L"请检查参考坐标、尺寸或偏移范围。",true);return;}
            values[i]=*value;
        }
        if(visualDetail==2){
            if(preview.is_object())command("set_calibration_reference",{{"token",preview.at("token")},{"rect",values}});
            else command("recalibrate_reference",{{"id",selected},{"rect",values}},true);
            return;
        }
        auto relation=relationForm;
        if(margins){
            auto padding=detail_number(RelationPadding,0,2);
            if(!padding||std::floor(*padding)!=*padding){
                calibration_notice(L"防漏边余量应为 0–2 的整数。",true);return;
            }
            relation["padding_physical"]=int(*padding);relation["margins_dip"]=values;
        }else{
            relation["offset_dip"]={values[0],values[1]};relation["fixed_size_dip"]={values[2],values[3]};
        }
        if(preview.is_object())command("update_calibration_preview",{{"token",preview.at("token")},{"relation",relation}});
        else command("update_cover_relation",{{"id",selected},{"relation",relation}});
    }
    void command(std::string op,json payload,bool handoff=false){
        if(!connected||interaction_locked())return;
        if(dirty()){status(L"请先应用当前修改");return;}
        if(handoff&&enginePid)AllowSetForegroundWindow(enginePid);
        set_busy(true);
        submit({std::move(op),std::move(payload),false,displayedRevision});
    }
    void stop_canvas_mode(){
        if(!connected||interaction_locked())return;
        set_busy(true);
        submit({"set_canvas_mode",{{"mode","idle"}}});
    }
    void visual_command(int id){
        if(!IsWindowEnabled(control(id)))return;
        if(id==VisualBegin||id==VisualSaveDistance){
            auto raw=contents(VisualDistance);wchar_t* end=nullptr;
            long distance=std::wcstol(raw.c_str(),&end,10);
            if(raw.empty()||*end||distance<32||distance>500){
                if(IsWindowVisible(control(VisualDistance)))SetFocus(control(VisualDistance));
                calibration_notice(L"找回距离应为 32–500 DIP。",true);return;
            }
            if(id==VisualBegin){
                auto kind=SendMessageW(control(VisualKind),CB_GETCURSEL,0,0)==1?"boundary":"features";
                command("begin_calibration",{{"id",selected},{"tracking_kind",kind},{"search_limit_dip",distance}});
            }else command("set_visual_search_limit",{{"id",selected},{"limit",distance}});
            return;
        }
        if(preview.is_object()){
            const char* op=id==VisualConfirm?"confirm_calibration":id==VisualCancel?"cancel_calibration":
                id==VisualReselect?"pick_calibration_reference":nullptr;
            if(op)command(op,{{"token",preview.at("token")}},id==VisualConfirm);
            return;
        }
        if(id==VisualRetry)command("retry_calibration",json::object(),true);
        if(id==VisualRefresh)command("refresh_visual",{{"id",selected}},true);
        if(id==VisualResume)command("set_position_mode",{{"id",selected},{"mode","visual"}},true);
        if(id==VisualAnchor){
            bool stored=!effectiveRects.contains(selected)||!effectiveRects.at(selected).is_object();
            if(stored&&MessageBoxW(window,L"当前视觉位置暂不可用。使用上次保存的窗口坐标？",L"改用窗口锚定",
                MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
            command("set_position_mode",{{"id",selected},{"mode","anchor"},{"use_saved_position",stored}});
        }
    }
    void choose_color(const Option& option){
        COLORREF initial=0,custom[16]{};
        auto text=contents(option.id);
        if(text.size()==7&&text[0]==L'#'){
            auto value=std::wcstoul(text.c_str()+1,nullptr,16);
            initial=RGB(value>>16&255,value>>8&255,value&255);
        }
        CHOOSECOLORW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;
        dialog.rgbResult=initial;dialog.lpCustColors=custom;dialog.Flags=CC_RGBINIT|CC_FULLOPEN;
        if(ChooseColorW(&dialog)){
            wchar_t hex[8]{};
            swprintf_s(hex,L"#%02X%02X%02X",GetRValue(dialog.rgbResult),GetGValue(dialog.rgbResult),GetBValue(dialog.rgbResult));
            SetWindowTextW(control(option.id),hex);
        }
    }
    void invalidate_field_frame(int id,BOOL erase=FALSE){
        auto frame=std::find_if(frames.begin(),frames.end(),[id](const auto& item){return item.id==id;});
        if(frame!=frames.end())InvalidateRect(window,&frame->bounds,erase);
    }
    void show_page(){
        std::vector<std::pair<int,bool>> visibility;
        auto show=[&](int id,bool visible){
            auto child=control(id);
            if(bool(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)==visible)return;
            visibility.emplace_back(id,visible);
        };
        for(int id:{List,RegionsLabel})show(id,sidebarShown);
        show(DeleteRegion,!regionScopes&&sidebarShown&&!selected.empty());
        show(AddRegion,canvasModes||sidebarShown);show(FreeRegion,canvasModes);
        show(ClearRegions,sidebarShown);
        EnableWindow(control(ClearRegions),!interaction_locked()&&!regions.empty()&&!preview.is_object());
        update_region_scroll();
        show(ThemeChoice,false);show(MoveToTarget,false);
        for(int id:{Text,TextLabel,Font,FontLabel,Size,SizeLabel})show(id,page==Typography);
        show(BoldButton,page==Typography);
        show(PositionAdvanced,page==Position||page==Visual);
        show(ReferenceStatus,page==Position||page==Visual);
        show(PositionContext,page==Visual);
        show(EffectScroll,effect_page(page));
        for(int i=0;i<PageCount;i++)show(pageButton+i,i<4||((i==Outline||i==Glow)&&effect_page(page)));
        for(const auto& option:options){
            bool toggle=std::find(std::begin(switches),std::end(switches),option.id)!=std::end(switches);
            bool visible=option.page==page&&!(option.page==Position&&visual_region());
            bool segment=option.id==200;
            if(toggle)show(ToggleControl+option.id,visible);
            bool slider=std::find(std::begin(sliders),std::end(sliders),option.id)!=std::end(sliders);
            if(slider)show(TrackControl+option.id,visible);
            if(option.id>=205&&option.id<=208){
                show(StepButton+2*option.id,visible);show(StepButton+2*option.id+1,visible);
            }
            show(option.id,visible&&!segment&&!toggle);show(option.id+optionLabel,visible&&!segment);
            if(option.kind==OptionKind::Color)show(option.id+optionPicker,visible);
        }
        show(PickColor,page==Background);
        show(PickTextColor,page==Typography);
        bool visual=page==Visual,common=visual&&visualDetail==0;
        bool picking=preview.is_object();
        bool tracking=visual_region();
        if(visual)set_text(PositionContext,tracking?L"当前定位 · 视觉跟踪":L"当前定位 · 窗口锚定");
        auto region=selected_region();
        bool savedReference=region!=regions.end()&&region->at("visual_binding").is_object();
        bool calibrationPage=page==Position||visual;
        bool emptyDetail=visual&&!common&&!referenceForm.is_array();
        for(int id:{VisualKind,VisualKindLabel})show(id,calibrationPage&&!picking);
        show(VisualBegin,(calibrationPage||emptyDetail)&&!picking);
        for(int id:{VisualConfirm,VisualCancel,VisualReselect})show(id,calibrationPage&&picking);
        show(VisualRetry,common&&!picking&&canRetry);
        show(VisualAnchor,common&&!picking&&tracking);
        show(VisualRefresh,calibrationPage&&!picking&&tracking);
        show(VisualResume,common&&!picking&&!tracking&&savedReference);
        for(int id:{VisualDistance,VisualDistanceLabel,VisualSaveDistance,VisualInterval,VisualIntervalLabel})show(id,common);
        for(int id:{VisualCheckTab,VisualRelationTab,VisualReferenceTab})show(id,visual);
        bool relation=visual&&visualDetail==1&&!emptyDetail;
        bool fixed=relationForm.is_object()&&relationForm.at("mode")=="position_only";
        show(RelationMode,relation);show(RelationModeLabel,relation);
        for(int id:{RelationX,RelationY,RelationXLabel,RelationYLabel})show(id,relation&&fixed);
        show(RelationPadding,relation&&!fixed);show(RelationPaddingLabel,relation&&!fixed);
        show(StepButton+2*RelationPadding,relation&&!fixed);show(StepButton+2*RelationPadding+1,relation&&!fixed);
        show(DetailApply,visual&&!common&&!emptyDetail);
        show(CoverDrag,relation);
        for(int i=0;i<4;i++){
            show(DetailValue+i,visual&&!common&&!emptyDetail);show(DetailLabel+i,visual&&!common&&!emptyDetail);
            show(StepButton+2*(DetailValue+i),visual&&!common&&!emptyDetail);
            show(StepButton+2*(DetailValue+i)+1,visual&&!common&&!emptyDetail);
        }
        if(visibility.empty())return;
        auto batch=BeginDeferWindowPos(int(visibility.size()));
        for(const auto& [id,visible]:visibility){
            if(!batch)break;
            batch=DeferWindowPos(batch,control(id),nullptr,0,0,0,0,
                SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|
                (visible?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
        }
        bool changed=batch&&EndDeferWindowPos(batch);
        for(const auto& [id,visible]:visibility){
            auto child=control(id);
            if(!changed)ShowWindow(child,visible?SW_SHOWNA:SW_HIDE);
            RECT bounds{};GetWindowRect(child,&bounds);
            MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
            InvalidateRect(window,&bounds,FALSE);
            invalidate_field_frame(id);
            if(visible)InvalidateRect(child,nullptr,FALSE);
        }
    }
    void update_actions(){
        bool pending=!syncing&&loadedOptions.size()==std::size(options)&&dirty();
        auto apply=control(Apply);
        if(bool(GetWindowLongPtrW(apply,GWL_STYLE)&WS_VISIBLE)!=pending){
            ShowWindow(apply,pending?SW_SHOWNA:SW_HIDE);
            InvalidateRect(control(Done),nullptr,FALSE);
        }
        set_text(SaveState,busy?L"保存中":pending?L"已修改":L"已保存");
    }
    void update_option_chrome(int id){
        if(id==200){
            InvalidateRect(control(BoldButton),nullptr,FALSE);
        }else if(std::find(std::begin(switches),std::end(switches),id)!=std::end(switches)){
            InvalidateRect(control(ToggleControl+id),nullptr,FALSE);
        }else if(std::find(std::begin(sliders),std::end(sliders),id)!=std::end(sliders)){
            const auto* option=option_at(id);
            if(auto value=detail_number(id,option->minimum,option->maximum)){
                auto track=control(TrackControl+id);
                auto position=std::lround(*value*slider_units(id));
                if(SendMessageW(track,TBM_GETPOS,0,0)!=position)SendMessageW(track,TBM_SETPOS,TRUE,position);
            }
        }else if(auto option=option_at(id);option&&option->kind==OptionKind::Color){
            InvalidateRect(control(option->id+optionPicker),nullptr,FALSE);
        }
    }
    void paint_backdrop(HDC dc,const RECT& area,HWND child=nullptr){
        if(theme.highContrast||!layoutSize.cx||!layoutSize.cy){FillRect(dc,&area,theme.page);return;}
        if(!backdrop.dc||backdrop.revision!=theme.revision||
            backdrop.size.cx!=layoutSize.cx||backdrop.size.cy!=layoutSize.cy){
            backdrop.reset();
            backdrop.dc=CreateCompatibleDC(dc);
            backdrop.bitmap=CreateCompatibleBitmap(dc,layoutSize.cx,layoutSize.cy);
            if(!backdrop.dc||!backdrop.bitmap){backdrop.reset();FillRect(dc,&area,theme.page);return;}
            backdrop.previous=SelectObject(backdrop.dc,backdrop.bitmap);
            backdrop.size=layoutSize;backdrop.revision=theme.revision;
            auto surface=backdrop.dc;
            auto dpi=layoutDpi;
            auto scale=[dpi](int value){return chrome::px(value,dpi);};
            RECT bounds{0,0,layoutSize.cx,layoutSize.cy};
            FillRect(surface,&bounds,theme.page);
            chrome::rounded(surface,bounds,scale(15),theme.background,theme.outline);
            chrome::backdrop_glow(surface,bounds,scale(15),theme.tint);
            if(!IsRectEmpty(&railBounds)){
                chrome::shadow(surface,railBounds,scale(10),dpi,theme.dark);
                chrome::glass_surface(surface,railBounds,scale(10),theme.rail,theme.border,theme.highlight,theme.dark?125:122);
            }
            chrome::shadow(surface,paneBounds,scale(11),dpi,theme.dark);
            chrome::glass_surface(surface,paneBounds,scale(11),theme.glass,theme.border,theme.highlight,theme.dark?160:125);
        }
        POINT source{area.left,area.top};
        if(child)MapWindowPoints(child,window,&source,1);
        BitBlt(dc,area.left,area.top,area.right-area.left,area.bottom-area.top,backdrop.dc,source.x,source.y,SRCCOPY);
    }
    void paint_item(const DRAWITEMSTRUCT& item){
        UINT dpi=GetDpiForWindow(window);
        auto scale=[&](int value){return chrome::px(value,dpi);};
        auto box=item.rcItem;
        auto dc=item.hDC;
        if(item.CtlType==ODT_STATIC){
            paint_backdrop(dc,box,item.hwndItem);
            auto prior=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(item.hwndItem,WM_GETFONT,0,0)));
            chrome::label(dc,box,contents(item.CtlID).c_str(),label_color(item.hwndItem),DT_LEFT);
            SelectObject(dc,prior);
            return;
        }
        if(item.CtlType==ODT_LISTBOX){
            paint_backdrop(dc,box,item.hwndItem);
            if(item.itemID>=regions.size())return;
            const auto& region=regions[item.itemID];
            bool selected=(item.itemState&ODS_SELECTED)!=0;
            RECT card{box.left+scale(3),box.top+scale(3),box.right-scale(3),box.bottom-scale(4)};
            if(selected){
                chrome::rounded(dc,card,scale(7),theme.selected,theme.highContrast?theme.selectedBorder:theme.selected);
                if(!theme.highContrast){
                    RECT edge{card.left,card.top+scale(8),card.left+scale(2),card.bottom-scale(8)};
                    chrome::rounded(dc,edge,scale(1),theme.accent,theme.accent);
                }
            }
            auto title=wide(region.at("text").get<std::string>());title.resize(std::min(title.size(),title.find_first_of(L"\r\n")));
            if(title.empty())title=L"空白遮盖";
            bool followsApp=region.value("window_profile",std::string()).empty();
            RECT badge{card.left+scale(6),card.top+scale(8),card.left+scale(26),card.top+scale(28)};
            auto badgeFill=selected?theme.accent:theme.button;
            chrome::rounded(dc,badge,scale(4),badgeFill,badgeFill);
            chrome::icon(dc,badge,followsApp?chrome::Icon::Application:chrome::Icon::Window,
                selected?theme.accentText:theme.muted,dpi);
            RECT first{card.left+scale(31),card.top+scale(5),card.right-scale(regionScopes?32:8),card.top+scale(24)};
            RECT second=first;OffsetRect(&second,0,scale(18));
            auto previous=SelectObject(dc,strongFont);
            chrome::label(dc,first,title.c_str(),selected&&theme.highContrast?theme.accentText:theme.text,DT_LEFT);
            SelectObject(dc,mutedFont);
            bool visual=region.at("position_mode")=="visual";
            std::wstring detail=visual?L"视觉":L"固定";
            if(regionScopes)detail+=followsApp?L" · 跟随应用":L" · 跟随窗口";
            else detail=visual?L"视觉定位":L"窗口锚定";
            chrome::label(dc,second,detail.c_str(),
                selected&&theme.highContrast?theme.accentText:theme.muted,DT_LEFT);
            if(regionScopes){
                auto actions=region_action_rect(box,dpi);
                bool hover=hoveredRegionAction==int(item.itemID);
                if(hover)chrome::rounded(dc,actions,scale(6),theme.hover,theme.highContrast?theme.border:theme.hover);
                auto color=hover?theme.text:selected&&theme.highContrast?theme.accentText:theme.muted;
                chrome::icon(dc,actions,chrome::Icon::More,color,dpi);
            }
            SelectObject(dc,previous);
            if(keyboardFocus&&(item.itemState&ODS_FOCUS)){InflateRect(&card,-scale(2),-scale(2));DrawFocusRect(dc,&card);}
            return;
        }
        paint_backdrop(dc,box,item.hwndItem);
        int id=int(item.CtlID);
        bool disabled=(item.itemState&ODS_DISABLED)!=0;
        COLORREF ink=disabled?theme.disabled:theme.text;
        bool pressed=(item.itemState&ODS_SELECTED)!=0;
        bool hovered=!disabled&&hoveredButton==item.hwndItem;
        std::optional<chrome::Icon> glyph;
        if(id==Appearance)glyph=theme.mode==1?chrome::Icon::Sun:theme.mode==2?chrome::Icon::Moon:chrome::Icon::System;
        if(id==Settings)glyph=chrome::Icon::Gear;
        if(id==ClosePanel)glyph=chrome::Icon::Close;
        bool toolbar=glyph.has_value();
        bool visualPrimary=id==DetailApply||id==VisualBegin||id==VisualConfirm;
        bool visualActionAvailable=false;
        if(id==Done)
            for(int action:{DetailApply,VisualBegin,VisualConfirm})
                if(IsWindowVisible(control(action))&&IsWindowEnabled(control(action)))visualActionAvailable=true;
        bool primary=id==Apply||visualPrimary||
            (id==Done&&!IsWindowVisible(control(Apply))&&!visualActionAvailable);
        bool secondary=id==VisualRefresh||id==VisualAnchor||id==VisualRetry||id==VisualResume||
            id==VisualReselect||id==VisualCancel||id==VisualSaveDistance||id==CoverDrag||id==PositionAdvanced||id==Done;
        bool tab=id>=pageButton&&id<=pageButton+Scroll;
        bool selector=id==ConnectionStatus;
        bool activeTab=(id==pageButton+Position&&(page==Position||page==Visual))||(id==pageButton+Scroll&&effect_page(page))||
            (id==pageButton+page&&(page==Typography||page==Background));
        bool effectTab=id==EffectScroll||id==pageButton+Outline||id==pageButton+Glow;
        bool activeEffect=effectTab&&((id==EffectScroll&&page==Scroll)||id==pageButton+page);
        bool visualTab=id>=VisualCheckTab&&id<=VisualReferenceTab;
        bool activeVisual=visualTab&&id-VisualCheckTab==visualDetail;
        bool swatch=id>=optionPicker+200&&id<=optionPicker+227;
        bool toggle=id>=ToggleControl+200;
        if(swatch){
            auto hex=contents(id-optionPicker);
            auto value=hex.size()==7&&hex.front()==L'#'?std::wcstoul(hex.c_str()+1,nullptr,16):0;
            auto color=RGB(value>>16&255,value>>8&255,value&255);
            FillRect(dc,&box,theme.buttonField);
            InflateRect(&box,-scale(1),-scale(1));chrome::rounded(dc,box,scale(1),color,color);
        }else if(toggle){
            bool active=SendMessageW(control(id-ToggleControl),CB_GETCURSEL,0,0)==1;
            auto fill=active?theme.accent:theme.disabled;
            chrome::rounded(dc,box,scale(11),fill,fill);
            RECT thumb{active?box.right-scale(18):box.left+scale(4),box.top+scale(4),0,box.bottom-scale(4)};
            thumb.right=thumb.left+scale(14);
            COLORREF dot=active?theme.accentText:RGB(255,255,255);
            chrome::rounded(dc,thumb,scale(7),dot,dot);
        }else{
            if(selector||primary)InflateRect(&box,-scale(2),-scale(3));
            if(primary&&!disabled){
                if(!theme.highContrast)chrome::shadow(dc,box,scale(7),dpi,theme.dark,2);
                auto fill=theme.accent;
                if(id==VisualBegin&&!theme.highContrast){
                    auto amount=reference_hover();
                    if(pressed){OffsetRect(&box,0,scale(1));amount=-.22;}
                    auto tint=amount<0?theme.accentText:RGB(255,255,255);
                    auto mix=[amount](int base,int target){return int(std::lround(base+(target-base)*std::abs(amount)));};
                    fill=RGB(mix(GetRValue(fill),GetRValue(tint)),mix(GetGValue(fill),GetGValue(tint)),
                        mix(GetBValue(fill),GetBValue(tint)));
                }
                chrome::rounded(dc,box,scale(7),fill,fill);ink=theme.accentText;
            }
            else if(selector){
                if(!theme.highContrast)chrome::shadow(dc,box,scale(8),dpi,theme.dark,2);
                chrome::glass_surface(dc,box,scale(8),pressed||hovered?theme.hover:theme.input,
                    theme.border,theme.highlight,theme.highContrast?255:190);
                if(!theme.highContrast)chrome::accent_edge(dc,box,dpi,theme.accent);
                RECT badge{box.left+scale(4),box.top+scale(4),box.left+scale(32),box.bottom-scale(4)};
                chrome::rounded(dc,badge,scale(5),theme.accent,theme.accent);
                chrome::icon(dc,badge,chrome::Icon::Frame,theme.accentText,dpi);
            }
            else if(id==BoldButton){
                bool active=SendMessageW(control(200),CB_GETCURSEL,0,0)==1;
                auto fill=active?theme.activeSegment:hovered?theme.hover:theme.segment;
                chrome::rounded(dc,box,scale(6),fill,active?theme.focus:theme.border);
                if(active&&theme.highContrast)ink=theme.accentText;
            }
            else if(tab&&activeTab&&!theme.highContrast){
                RECT mark{box.left+scale(9),box.bottom-scale(5),box.right-scale(9),box.bottom-scale(1)};
                chrome::rounded(dc,mark,scale(2),theme.accent,theme.accent);
            }
            else if(effectTab&&activeEffect){
                chrome::rounded(dc,box,scale(5),theme.accent,theme.accent);
                ink=theme.accentText;
            }else if(tab&&activeTab){
                chrome::rounded(dc,box,scale(7),theme.selected,theme.selected);
                ink=theme.highContrast?theme.accentText:theme.text;
            }else if(visualTab){
                if(activeVisual||hovered||pressed){
                    auto fill=pressed||hovered?theme.hover:theme.activeSegment;
                    chrome::rounded(dc,box,scale(6),fill,fill);
                }
                if(activeVisual){
                    LONG middle=(box.left+box.right)/2;
                    RECT mark{middle-scale(13),box.bottom-scale(4),middle+scale(13),box.bottom-scale(1)};
                    chrome::rounded(dc,mark,scale(2),theme.accent,theme.accent);
                }
            }else if((secondary||primary)&&!theme.highContrast){
                auto fill=disabled?theme.input:pressed||hovered?theme.hover:id==PositionAdvanced?theme.input:theme.action;
                auto edge=disabled?theme.border:theme.actionBorder;
                chrome::glass_surface(dc,box,scale(6),fill,edge,theme.highlight,disabled?104:220);
            }else if(!toolbar&&!tab&&!effectTab){
                auto fill=pressed||hovered?theme.hover:theme.button;
                auto edge=id==FreeRegion&&canvasMode=="free"?theme.accent:
                    theme.highContrast?theme.border:fill;
                chrome::rounded(dc,box,scale(6),fill,edge);
            }
            else if(pressed||hovered)chrome::rounded(dc,box,scale(4),theme.hover,theme.hover);
            if(glyph)chrome::icon(dc,box,*glyph,ink,dpi);
            else{
                auto caption=id==BoldButton?L"B":contents(id);
                auto previous=SelectObject(dc,id==VisualBegin?mutedFont:
                    primary||(tab&&activeTab)||activeEffect||activeVisual||id==BoldButton?strongFont:font);
                auto label=box;
                if(selector){
                    label.left+=scale(40);label.right-=scale(24);
                    RECT arrow=box;arrow.left=arrow.right-scale(23);arrow.right-=scale(5);
                    chrome::icon(dc,arrow,chrome::Icon::Chevron,ink,dpi);
                }else if(id==AddRegion||id==FreeRegion||id==ClearRegions){
                    caption=id==AddRegion?L"新建":id==ClearRegions?L"清空":canvasMode=="free"?L"结束":L"全窗";
                    SelectObject(dc,mutedFont);
                    SIZE textSize{};GetTextExtentPoint32W(dc,caption.c_str(),int(caption.size()),&textSize);
                    auto iconSize=scale(14),gap=scale(5);
                    LONG left=(box.left+box.right-iconSize-gap-textSize.cx)/2,top=(box.top+box.bottom-iconSize)/2;
                    RECT badge{left,top,left+iconSize,top+iconSize};
                    if(id==AddRegion)chrome::rounded(dc,badge,scale(3),theme.accent,theme.accent);
                    if(id==ClearRegions&&!disabled)ink=theme.danger;
                    chrome::icon(dc,badge,id==AddRegion?chrome::Icon::Plus:id==FreeRegion?chrome::Icon::Frame:chrome::Icon::Trash,
                        id==AddRegion?theme.accentText:ink,dpi,true);
                    label.left=badge.right+gap;
                }
                if(tab)label.bottom-=scale(4);
                if(selector&&!targetAppLabel.empty()){
                    SelectObject(dc,strongFont);
                    SIZE appSize{};
                    GetTextExtentPoint32W(dc,targetAppLabel.c_str(),int(targetAppLabel.size()),&appSize);
                    RECT app=label;app.right=app.left+std::min<LONG>(appSize.cx,(label.right-label.left)*55/100);
                    chrome::label(dc,app,targetAppLabel.c_str(),ink,DT_LEFT);
                    RECT slash{app.right+scale(5),label.top,app.right+scale(17),label.bottom};
                    chrome::label(dc,slash,L"/",theme.muted);
                    label.left=slash.right+scale(4);
                    SelectObject(dc,font);
                    chrome::label(dc,label,targetWindowLabel.c_str(),ink,DT_LEFT);
                }else chrome::label(dc,label,caption.c_str(),id==DeleteRegion?theme.danger:ink,
                    selector||id==AddRegion||id==FreeRegion||id==ClearRegions?DT_LEFT:DT_CENTER);
                SelectObject(dc,previous);
            }
        }
        if(keyboardFocus&&(item.itemState&ODS_FOCUS)&&!disabled){InflateRect(&box,-scale(2),-scale(2));DrawFocusRect(dc,&box);}
    }
    void popup_header(int id){
        std::vector<PanelPopup::Row> rows;
        if(id==Appearance){
            const wchar_t* labels[]={L"跟随系统",L"浅色",L"深色"};
            for(int i=0;i<3;i++)rows.push_back({labels[i],theme.mode==i});
        }else{
            rows={{L"移到目标显示器"},{L"显示遮盖区域",sidebarOpen}};
        }
        RECT anchor{};GetWindowRect(control(id),&anchor);
        int width=chrome::px(id==Appearance?138:169,GetDpiForWindow(window));
        anchor.left=anchor.right-width;
        popup.show(window,control(id),anchor,width,theme,font,std::move(rows),[this,id](size_t choice){
            popup.close();
            if(id==Settings&&choice==0)move_to_target();
            else if(id==Settings&&choice==1)SendMessageW(window,WM_COMMAND,Sidebar,0);
            else{theme.mode=int(choice);theme.save();theme.apply(window);}
        });
    }
    void apply_picker_choice(const json& payload){
        popup.close();
        if(!connected||interaction_locked()||finishRequested)return;
        auto revision=displayedRevision;
        if(payload.contains("delete_window_profile")){
            if(dirty()){status(L"请先应用当前修改");return;}
            auto name=wide(payload.at("name").get<std::string>());
            auto message=L"确定删除「"+name+L"」的窗口配置及其专用遮盖吗？\n\n此操作无法撤销。";
            if(MessageBoxW(window,message.c_str(),L"删除窗口配置",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
            if(!connected||interaction_locked()||finishRequested||revision!=displayedRevision)return;
        }else if(payload.contains("delete_id")){
            auto name=wide(payload.at("name").get<std::string>());
            auto executable=wide(payload.at("executable").get<std::string>());
            auto message=L"确定删除「"+name+L"」的遮盖、视觉模板和配置吗？\n\n此操作无法撤销。";
            if(!executable.empty())message+=L"\n\n"+executable;
            if(MessageBoxW(window,message.c_str(),L"删除应用及配置",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
            if(!connected||interaction_locked()||finishRequested)return;
        }
        if(enginePid)AllowSetForegroundWindow(enginePid);
        set_busy(true);
        submit({"switch_application",payload,false,revision});
    }
    static std::wstring window_label(const json& entry,const json& windows){
        auto title=wide(entry.at("title").get<std::string>());
        if(title.empty())title=L"无标题窗口";
        if(std::count_if(windows.begin(),windows.end(),[&](const auto& other){return other.at("title")==entry.at("title");})>1)
            title+=L" · "+std::to_wstring(entry.at("index").get<size_t>()+1);
        return title;
    }
    void show_picker(const std::wstring& focus={}){
        const auto& response=pickerSnapshot;
        const auto active=response.at("active_application").get<std::string>();
        std::vector<PanelPopup::Row> rows;
        std::vector<std::function<void()>> actions;
        int selectedRow=-1;
        auto append=[&](PanelPopup::Row row,std::function<void()> action){
            if(!focus.empty()&&focus==row.label)selectedRow=int(rows.size());
            row.disabled=!action;
            rows.push_back(std::move(row));
            actions.push_back(std::move(action));
        };
        auto command_item=[&](std::wstring label,json payload,int indent=0,bool danger=false){
            PanelPopup::Row row{std::move(label)};
            row.indent=indent;row.danger=danger;
            append(std::move(row),[this,payload=std::move(payload)]{apply_picker_choice(payload);});
        };
        auto menu=[&](const std::string& key,const std::wstring& label){
            return [this,key,label]{pickerAction=pickerAction==key?"":key;show_picker(label);};
        };
        for(const auto& application:response.at("applications")){
            auto id=application.at("id").get<std::string>();
            auto name=wide(application.at("name").get<std::string>());
            const auto& windows=application.at("windows");
            const auto& profiles=application.at("profiles");
            bool direct=id!=active&&windows.size()<=1;
            bool expanded=expandedPickerApp==id;
            auto expand=[this,id,name]{
                expandedPickerApp=expandedPickerApp==id?"":id;
                pickerAction.clear();show_picker(name);
            };
            PanelPopup::Row appRow{name};
            appRow.heading=true;appRow.next=!direct;appRow.expanded=expanded;
            if(direct&&(!profiles.empty()||expanded))appRow.onAction=expand;
            if(direct){
                json payload={{"id",id}};
                if(!windows.empty())payload["index"]=windows.front().at("index");
                append(std::move(appRow),[this,payload=std::move(payload)]{apply_picker_choice(payload);});
            }else append(std::move(appRow),expand);
            if(!expanded)continue;
            for(const auto& entry:windows){
                auto label=window_label(entry,windows);
                auto index=entry.at("index");
                auto profile=entry.value("profile",std::string());
                auto key=id+"/live/"+profile;
                PanelPopup::Row row{label};
                row.indent=12;row.current=entry.value("bound",false);row.icon=chrome::Icon::Window;
                if(!profile.empty()){row.onAction=menu(key,label);row.expanded=pickerAction==key;}
                append(std::move(row),[this,id,index]{apply_picker_choice({{"id",id},{"index",index}});});
                if(!profile.empty()&&pickerAction==key)
                    command_item(L"删除窗口配置…",{{"id",id},{"index",index},{"delete_window_profile",profile},{"name",utf8(label)}},28,true);
            }
            if(windows.empty()){
                PanelPopup::Row waiting{L"等待窗口"};waiting.indent=12;
                append(std::move(waiting),{});
            }
            for(const auto& entry:profiles){
                auto profile=entry.at("id").get<std::string>();
                auto label=wide(entry.at("title").get<std::string>());
                if(label.empty())label=L"无标题窗口";
                if(std::count_if(profiles.begin(),profiles.end(),[&](const auto& other){return other.at("title")==entry.at("title");})>1)
                    label+=L" · "+wide(profile.substr(0,8));
                bool deleting=entry.value("deleting",false);
                if(deleting)label+=L" · 删除未完成";
                auto key=id+"/archive/"+profile;
                auto toggle=menu(key,label);
                PanelPopup::Row row{label};
                row.indent=12;row.icon=chrome::Icon::Archive;row.onAction=toggle;row.expanded=pickerAction==key;
                append(std::move(row),toggle);
                if(pickerAction!=key)continue;
                if(!deleting){
                    bool hasBound=id==active&&std::any_of(windows.begin(),windows.end(),[](const auto& window){
                        return window.value("bound",false);
                    });
                    for(const auto& window:windows){
                        if(hasBound&&!window.value("bound",false))continue;
                        command_item(L"关联到「"+window_label(window,windows)+L"」",
                            {{"id",id},{"index",window.at("index")},{"window_profile",profile}},28);
                    }
                }
                if(profile!="legacy")command_item(L"删除窗口配置…",
                    {{"id",id},{"delete_window_profile",profile},{"name",utf8(label)}},28,true);
            }
        }
        const auto& deletable=response.at("deletable_applications");
        bool canManage=!active.empty()||!deletable.empty();
        PanelPopup::Row add{L"添加应用"};
        add.icon=chrome::Icon::Plus;
        add.footer=add.footerStart=true;
        add.footerEnd=!canManage;
        append(std::move(add),[this]{apply_picker_choice({{"pick",true}});});
        if(canManage){
            PanelPopup::Row manage{L"管理应用…"};
            manage.icon=chrome::Icon::Gear;
            manage.footer=manage.footerEnd=true;
            manage.next=true;manage.expanded=pickerManagement;
            append(std::move(manage),[this]{pickerManagement=!pickerManagement;show_picker(L"管理应用…");});
            if(pickerManagement){
                if(!active.empty())command_item(L"移除当前应用（保留配置）",{{"remove_current",true}},12);
                for(const auto& record:deletable)
                    command_item(L"删除 "+wide(record.at("name").get<std::string>())+L" 及配置…",
                        {{"delete_id",record.at("id")},{"name",record.at("name")},{"executable",record.at("executable")}},12,true);
            }
        }
        RECT anchor{};auto button=control(ConnectionStatus);
        GetWindowRect(button,&anchor);
        auto dpi=GetDpiForWindow(window);
        InflateRect(&anchor,-chrome::px(2,dpi),-chrome::px(3,dpi));
        popup.show(window,button,anchor,anchor.right-anchor.left,theme,font,std::move(rows),
            [actions=std::move(actions)](size_t index){if(actions[index])actions[index]();},{},selectedRow,strongFont);
    }
    void confirm_delete(const std::string& regionId,POINT where){
        if(interaction_locked())return;
        if(dirty()){status(L"请先应用当前修改");return;}
        auto menu=CreatePopupMenu();if(!menu)return;
        AppendMenuW(menu,MF_STRING,1,L"确认删除区域");
        AppendMenuW(menu,MF_STRING,2,L"取消");
        int choice=TrackPopupMenu(menu,TPM_RETURNCMD,where.x,where.y,0,window,nullptr);
        DestroyMenu(menu);
        if(choice==1)command("delete",{{"id",regionId}});
    }
    void region_menu(LPARAM location){
        if(interaction_locked()||preview.is_object()||!sidebarShown)return;
        POINT where{SHORT(LOWORD(location)),SHORT(HIWORD(location))};
        auto list=control(List);
        LRESULT row=SendMessageW(list,LB_GETCURSEL,0,0);
        if(location!=-1){
            POINT inside=where;ScreenToClient(list,&inside);
            auto hit=SendMessageW(list,LB_ITEMFROMPOINT,0,MAKELPARAM(inside.x,inside.y));
            if(HIWORD(hit))return;
            row=LOWORD(hit);
            RECT item{};
            if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))==LB_ERR||!PtInRect(&item,inside))return;
        }else{
            RECT bounds{};GetWindowRect(list,&bounds);where={bounds.left+18,bounds.top+28};
        }
        if(row<0||size_t(row)>=regions.size())return;
        if(dirty()){status(L"请先应用当前修改");return;}
        auto regionId=regions[row].at("id").get<std::string>();
        std::vector<PanelPopup::Row> rows;
        if(regionScopes){
            bool followsApp=regions[row].value("window_profile",std::string()).empty();
            rows.push_back({L"跟随应用",followsApp});
            rows.push_back({L"只在指定窗口显示…",!followsApp,true});
        }
        rows.push_back({L"删除区域…",false,false,true});
        RECT anchor{};
        SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&anchor));
        MapWindowPoints(list,nullptr,reinterpret_cast<POINT*>(&anchor),2);
        auto width=chrome::px(195,GetDpiForWindow(window));
        popup.show(window,list,anchor,width,theme,font,std::move(rows),[this,regionId,anchor](size_t choice){
            popup.close();
            if(interaction_locked()||finishRequested)return;
            if(!regionScopes||choice==2)confirm_delete(regionId,{anchor.left,anchor.bottom});
            else if(choice==0)command("set_region_scope",{{"id",regionId}});
            else{
                scopeRegion=regionId;scopeMenuAnchor=anchor;listingRegionTargets=true;
                set_busy(true);submit({"list_applications",json::object()});
            }
        });
    }
    void popup_region_targets(const json& response){
        std::vector<PanelPopup::Row> rows;
        std::vector<size_t> indices;
        for(const auto& app:response.at("applications")){
            if(app.at("id")!=response.at("active_application"))continue;
            const auto& windows=app.at("windows");
            for(const auto& entry:windows){
                rows.push_back({window_label(entry,windows),entry.value("bound",false)});
                indices.push_back(entry.at("index").get<size_t>());
            }
        }
        if(rows.empty())rows.push_back({L"没有打开的窗口",false,false,false,true});
        popup.show(window,control(List),scopeMenuAnchor,chrome::px(240,GetDpiForWindow(window)),theme,font,std::move(rows),
            [this,indices=std::move(indices)](size_t choice){
                popup.close();
                if(choice<indices.size()&&!interaction_locked()&&!finishRequested)
                    command("set_region_scope",{{"id",scopeRegion},{"index",indices[choice]}});
            });
    }
    void move_to_target(){
        if(!IsWindow(targetWindow)){status(L"等待目标窗口");return;}
        MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
        RECT target{},panel{};
        if(!GetMonitorInfoW(MonitorFromWindow(targetWindow,MONITOR_DEFAULTTONEAREST),&monitor)||
            !GetWindowRect(targetWindow,&target)||!GetWindowRect(window,&panel))return;
        const auto& work=monitor.rcWork;
        UINT dpi=GetDpiForWindow(targetWindow),own=GetDpiForWindow(window);
        if(!dpi||!own)return;
        LONG gap=MulDiv(12,dpi,96),width=MulDiv(panel.right-panel.left,dpi,own),height=MulDiv(panel.bottom-panel.top,dpi,own);
        LONG x=target.right+gap;
        if(x+width>work.right)x=target.left-gap-width;
        if(x<work.left)x=work.left+gap;
        LONG y=std::clamp(target.top,work.top,std::max(work.top,work.bottom-height));
        SetWindowPos(window,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void apply_present_focus(bool activate,bool focusText){
        if(activate)SetForegroundWindow(window);
        if(focusText){SetFocus(control(Text));SendMessageW(control(Text),EM_SETSEL,0,-1);}
    }
    void finish_presentation(bool visible){
        KillTimer(window,presentationTimer);
        if(presentationPending){
            GdiFlush();
            BOOL cloak=FALSE;
            DwmSetWindowAttribute(window,DWMWA_CLOAK,&cloak,sizeof(cloak));
            presentationPending=false;
        }
        if(visible)apply_present_focus(activateOnPresent,focusOnPresent);
        activateOnPresent=focusOnPresent=false;
    }
    void present(bool restore,bool activate,bool focusText){
        activateOnPresent|=activate;focusOnPresent|=focusText;
        if(presentationPending)return;
        if(restore||!IsWindowVisible(window)){
            BOOL cloak=TRUE;
            presentationPending=SUCCEEDED(DwmSetWindowAttribute(window,DWMWA_CLOAK,&cloak,sizeof(cloak)));
            ShowWindow(window,restore?SW_RESTORE:SW_SHOWNOACTIVATE);
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
            // Let the normal paint messages fill the redirected surface before revealing it.
            if(presentationPending&&SetTimer(window,presentationTimer,USER_TIMER_MINIMUM,nullptr))return;
        }
        finish_presentation(true);
    }
    void apply(bool close){
        if(close)popup.close(false);
        if(!connected){if(close)DestroyWindow(window);return;}
        if(interaction_locked()){if(close)finishRequested=true;return;}
        HWND focused=GetFocus();
        if(focused&&IsChild(window,focused))if(HIMC ime=ImmGetContext(focused)){
            if(ImmGetCompositionStringW(ime,GCS_COMPSTR,nullptr,0)>0)ImmNotifyIME(ime,NI_COMPOSITIONSTR,CPS_COMPLETE,0);
            ImmReleaseContext(focused,ime);
        }
        if(!dirty()){
            if(close){set_busy(true);submit({"snapshot",json::object(),true});}
            return;
        }
        auto current=selected_region();
        json values=json::object();
        auto text=form_text(),family=utf8(contents(Font));
        if(loadedText!=text)values["text"]=text;
        if(family.empty()){status(L"请输入字体名称");return;}
        if(current->at("font")!=family)values["font"]=family;
        if(contents(Size)!=loadedSize){
            auto size=detail_number(Size,6,200);
            if(!size||std::floor(*size)!=*size){
                page=Typography;layout();SetFocus(control(Size));
                status(L"请输入 6–200 的整数字号");return;
            }
            values["size"]=int(*size);
        }
        for(size_t i=0;i<std::size(options);i++){
            const auto& option=options[i];
            auto raw=option_value(option);
            if(raw==loadedOptions[i])continue;
            if(option.kind==OptionKind::Choice){
                auto index=SendMessageW(control(option.id),CB_GETCURSEL,0,0);
                if(index<0)continue;
                std::string value=option.choices[index].value;
                values[option.key]=option.choices==toggleChoices?json(value=="true"):json(value);
            }else if(option.kind==OptionKind::Color){
                if(raw.size()!=7||raw[0]!=L'#'||raw.find_first_not_of(L"0123456789abcdefABCDEF",1)!=std::wstring::npos){
                    status(L"颜色格式应为 #RRGGBB");return;
                }
                values[option.key]=utf8(raw);
            }else{
                wchar_t* end=nullptr;
                double value=std::wcstod(raw.c_str(),&end);
                if(raw.empty()||*end||!std::isfinite(value)||value<option.minimum||value>option.maximum){
                    status(std::wstring(option.label)+L"超出允许范围");return;
                }
                bool integer=option.id==202||(std::find(std::begin(sliders),std::end(sliders),option.id)!=std::end(sliders)&&
                    slider_units(option.id)==1);
                if(integer&&std::floor(value)!=value){
                    page=option.page;layout();SetFocus(control(option.id));
                    status(std::wstring(option.label)+L"请输入整数");return;
                }
                values[option.key]=value;
            }
        }
        SetWindowTextW(control(Status),L"正在提交");
        set_busy(true);
        submit({"update",{{"id",selected},{"values",values},{"coordinate_space","client"}},close,displayedRevision});
    }
    struct TabPosition {int id,x,y;};
    void order_tab_stops(std::vector<TabPosition>& form){
        // Win32 dialog navigation uses sibling order. Group controls that share a visual row
        // so the inset of an edit box doesn't put its adjacent steppers ahead of it.
        std::vector<int> order{ConnectionStatus,Appearance,Settings,ClosePanel,AddRegion,FreeRegion,List,RegionScroll,DeleteRegion};
        std::sort(form.begin(),form.end(),[](const auto& a,const auto& b){return std::tie(a.y,a.x)<std::tie(b.y,b.x);});
        for(auto first=form.begin();first!=form.end();){
            auto last=std::find_if(first,form.end(),[y=first->y](const auto& item){return item.y>y+9;});
            std::sort(first,last,[](const auto& a,const auto& b){return std::tie(a.x,a.y)<std::tie(b.x,b.y);});
            for(;first!=last;++first)order.push_back(first->id);
        }
        order.push_back(Apply);order.push_back(Done);
        if(order==tabOrder)return;
        tabOrder=std::move(order);
        auto batch=BeginDeferWindowPos(int(tabOrder.size()));
        constexpr UINT flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOREDRAW;
        for(int id:tabOrder){
            if(!batch)break;
            batch=DeferWindowPos(batch,control(id),HWND_BOTTOM,0,0,0,0,flags);
        }
        if(!batch||!EndDeferWindowPos(batch))
            for(int id:tabOrder)SetWindowPos(control(id),HWND_BOTTOM,0,0,0,0,flags);
    }
    void layout(){
        RECT client{};GetClientRect(window,&client);UINT dpi=GetDpiForWindow(window);
        if(!dpi||client.right<1||client.bottom<1)return;
        bool dpiChanged=layoutDpi!=dpi;
        bool resized=client.right!=layoutSize.cx||client.bottom!=layoutSize.cy;
        layoutSize={client.right,client.bottom};layoutDpi=dpi;
        auto dip=[&](int x){return chrome::px(x,dpi);};
        int w=MulDiv(client.right,96,dpi),h=MulDiv(client.bottom,96,dpi);
        std::vector<TabPosition> tabPositions;
        auto place=[&](int id,int x,int y,int width,int height,bool exactHeight=true){
            HWND child=control(id);
            RECT before{};GetWindowRect(child,&before);
            MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&before),2);
            auto pxX=dip(x),pxY=dip(y),pxWidth=dip(std::max(1,width)),pxHeight=dip(std::max(1,height));
            if(dpiChanged||before.left!=pxX||before.top!=pxY||before.right-before.left!=pxWidth||
                (exactHeight&&before.bottom-before.top!=pxHeight)){
                bool visible=IsWindowVisible(child)!=FALSE;
                SetWindowPos(child,nullptr,pxX,pxY,pxWidth,pxHeight,
                    SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS);
                if(visible){
                    InvalidateRect(window,&before,FALSE);
                    InvalidateRect(child,nullptr,FALSE);
                }
            }
            if(id!=ConnectionStatus&&id!=Appearance&&id!=Settings&&id!=ClosePanel&&id!=List&&id!=RegionScroll&&
                id!=AddRegion&&id!=FreeRegion&&id!=DeleteRegion&&
                id!=Apply&&id!=Done&&(GetWindowLongPtrW(child,GWL_STYLE)&WS_TABSTOP))
                tabPositions.push_back({id,x,y});
        };
        auto oldFrames=std::exchange(frames,{});
        auto field=[&](int id,int x,int y,int width,int height,bool combo=false,bool color=false){
            frames.push_back({id,{dip(x),dip(y),dip(x+width),dip(y+height)},color});
            int inset=color?31:id==Font?11:combo?6:13;
            int top=id==Text?4:combo?(height-22)/2:(height-20)/2;
            if(id==Text)inset=2;
            if(color){
                top=(height-18)/2;
                place(id+optionPicker,x+10,y+(height-16)/2,16,16);
            }
            place(id,x+inset,y+top,width-inset-(combo?3:9),combo?170:height-top-(id==Text?7:6),!combo);
        };
        auto number=[&](int id,int x,int y,int width){
            field(id,x,y,width-60,32);
            place(StepButton+2*id,x+width-55,y,26,32);
            place(StepButton+2*id+1,x+width-25,y,25,32);
        };
        auto track=[&](int id,int x,int y,int width,int height=36){
            place(TrackControl+id,x,y+2,width-94,height-4);
            field(id,x+width-85,y,85,height);
        };
        bool sidebarChanged=sidebarShown!=sidebarOpen;
        sidebarShown=sidebarOpen;
        if(dpiChanged||resized||sidebarChanged)backdrop.reset();
        constexpr int margin=23,railWidth=220,spacing=23;
        int paneLeft=sidebarShown?margin+railWidth+spacing:margin;
        int start=paneLeft+20,end=w-margin-20,main=end-start;
        railBounds=sidebarShown?RECT{dip(margin),dip(73),dip(margin+railWidth),dip(h-67)}:RECT{};
        paneBounds={dip(paneLeft),dip(162),dip(w-margin),dip(h-67)};
        place(AppTitle,margin,17,90,40);place(ConnectionStatus,118,14,290,44);
        place(Appearance,w-126,20,32,34);place(Settings,w-89,20,32,34);place(ClosePanel,w-52,20,32,34);
        place(RegionsLabel,margin+15,82,116,26);place(DeleteRegion,margin+132,80,77,30);
        int listTop=canvasModes?160:151,listBottom=h-79;
        place(List,margin+7,listTop,railWidth-14,listBottom-listTop);
        place(RegionScroll,margin+railWidth-9,listTop,8,listBottom-listTop);
        constexpr int toolGap=4,toolWidth=(railWidth-20-2*toolGap)/3,toolLeft=margin+10;
        place(AddRegion,sidebarShown?toolLeft:end-2*toolWidth-toolGap,sidebarShown?112:77,toolWidth,36);
        place(FreeRegion,sidebarShown?toolLeft+toolWidth+toolGap:end-toolWidth,sidebarShown?112:77,toolWidth,36);
        place(ClearRegions,toolLeft+2*(toolWidth+toolGap),112,toolWidth,36);
        place(RegionTitle,paneLeft,73,w-margin-paneLeft-(canvasModes&&!sidebarShown?150:0),37);
        for(int i=0;i<4;i++)place(pageButton+i,paneLeft+i*54,113,51,36);
        for(int i=0;i<3;i++){
            int id=i==0?int(EffectScroll):pageButton+Outline+i-1;
            place(id,start+i*51,176,46,31);
        }
        place(SaveState,margin,h-52,48,40);place(Status,margin+55,h-52,w-269,40);
        place(Apply,w-184,h-55,76,44);place(Done,w-99,h-55,78,44);

        bool shortPanel=h<550;
        bool compact=main<530||shortPanel;
        place(TextLabel,start,179,140,24);field(Text,start,210,main,shortPanel?43:compact?65:89);
        auto property=[&](int id,int label,int x,int y,int width,bool combo=false,bool color=false){
            place(label,x,y,width,21);
            field(id,x,y+24,width,38,combo,color);
        };
        int firstY=shortPanel?264:compact?285:318,secondY=compact?firstY+62:firstY;
        int fontWidth=compact?main-137:main-438;
        int fontEnd=start+fontWidth;
        property(Font,FontLabel,start,firstY,fontWidth,true);
        property(Size,SizeLabel,fontEnd+9,firstY,78);
        place(BoldButton,fontEnd+96,firstY+24,41,38);
        int alignStart=compact?start:fontEnd+146,alignWidth=compact?std::max(70,main-200):107;
        property(201,201+optionLabel,alignStart,secondY,alignWidth,true);
        property(204,204+optionLabel,alignStart+alignWidth+9,secondY,111,false,true);
        place(PickTextColor,alignStart+alignWidth+129,secondY+24,54,38);
        int metricsY=compact?firstY+124:393;
        property(203,203+optionLabel,start,metricsY,118);
        property(202,202+optionLabel,start+128,metricsY,118);

        int half=(main-8)/2;
        constexpr int positionNoticeY=248,positionNoticeHeight=24;
        constexpr int positionRowHeight=32,positionRowGap=6,positionGroupGap=14;
        constexpr int coordinateTop=positionNoticeY+positionNoticeHeight+positionGroupGap;
        constexpr int anchorTop=coordinateTop+2*positionRowHeight+positionRowGap+positionGroupGap;
        for(int i=0;i<4;i++){
            int x=start+i%2*(half+8),y=coordinateTop+i/2*(positionRowHeight+positionRowGap);
            place(205+i+optionLabel,x,y,34,32);number(205+i,x+34,y,half-34);
        }
        for(int i=0;i<2;i++){
            int y=anchorTop+i*(positionRowHeight+positionRowGap);
            place(209+i+optionLabel,start,y,58,32);field(209+i,start+58,y,main-58,32,true);
        }
        if(page==Position||page==Visual){
            place(PositionAdvanced,end-124,214,124,32);
            place(ReferenceStatus,start,page==Visual?436:positionNoticeY,main,page==Visual?20:positionNoticeHeight);
            set_text(PositionAdvanced,page==Visual?L"⌃  收起高级定位":L"›  高级定位设置");
            place(VisualKindLabel,start,176,58,32);field(VisualKind,start+58,176,main-58,32,true);
            int beginWidth=std::min(148,main-210);
            place(VisualBegin,start,214,beginWidth,32);
            place(VisualRefresh,start+beginWidth+8,214,70,32);
            int pickWidth=std::min(86,(main-148)/3);
            for(int i=0;i<3;i++)place(VisualConfirm+i,start+i*(pickWidth+8),214,pickWidth,32);
        }
        if(page==Visual){
            place(PositionContext,start,249,main,20);
            int tabWidth=std::min(132,(main-16)/3);
            for(int i=0;i<3;i++)place(VisualCheckTab+i,start+i*(tabWidth+8),274,tabWidth,30);
            place(VisualDistanceLabel,start,315,130,26);field(VisualDistance,start+142,312,78,28);
            place(VisualSaveDistance,start+225,312,105,28);
            place(VisualIntervalLabel,start,350,130,26);field(VisualInterval,start+142,347,main-142,28,true);
            constexpr int actionGap=8;
            int actionWidth=std::min(112,(main-2*actionGap)/3);
            place(VisualAnchor,start,386,actionWidth,29);
            place(VisualRetry,start+actionWidth+actionGap,386,actionWidth,29);
            place(VisualResume,start,386,actionWidth,29);
            place(RelationModeLabel,start,309,120,25);field(RelationMode,start+122,309,main-122,25,true);
            place(RelationXLabel,start,337,66,24);field(RelationX,start+70,337,half-70,24,true);
            place(RelationYLabel,start+half+8,337,66,24);field(RelationY,start+half+78,337,half-70,24,true);
            place(RelationPaddingLabel,start,337,130,24);field(RelationPadding,start+132,337,85,24);
            place(StepButton+2*RelationPadding,start+222,337,26,24);
            place(StepButton+2*RelationPadding+1,start+252,337,25,24);
            bool relation=visualDetail==1;
            int fieldTop=relation?365:312;
            int rowStep=relation?27:45;
            for(int i=0;i<4;i++){
                int x=start+(i%2)*(half+8),y=fieldTop+(i/2)*rowStep;
                int labelWidth=relation?(relationForm.is_object()&&relationForm.at("mode")=="position_only"?24:43):0;
                int inputY=relation?y:y+18;
                int stepWidth=relation?19:25,stepGap=4,stepSpace=2*(stepWidth+stepGap);
                place(DetailLabel+i,x,y,relation?labelWidth:half,relation?24:15);
                field(DetailValue+i,x+labelWidth,inputY,half-stepSpace-labelWidth,24);
                place(StepButton+2*(DetailValue+i),x+half-2*stepWidth-stepGap,inputY,stepWidth,24);
                place(StepButton+2*(DetailValue+i)+1,x+half-stepWidth,inputY,stepWidth,24);
            }
            int actionTop=relation?417:405;
            place(DetailApply,start,actionTop,140,26);
            set_text(DetailApply,visualDetail==1?L"保存遮盖关系":preview.is_object()?L"应用参考框":L"重新校准");
            place(CoverDrag,start+148,actionTop,140,26);
            place(ReferenceStatus,start,!referenceForm.is_array()&&visualDetail!=0?313:relation?442:436,
                main,relation&&referenceForm.is_array()?16:22);
        }

        place(212+optionLabel,start,176,83,35);track(212,start+83,176,main-83);
        place(211+optionLabel,start,226,83,38);field(211,start+83,226,main-139,38,false,true);
        place(PickColor,end-50,226,50,38);
        constexpr int effectTop=219,effectHeight=32;
        int effectStep=std::clamp((h-67-effectTop-effectHeight-6)/5,effectHeight,46);
        auto effectY=[&](int row){return effectTop+effectStep*row;};
        if(page==Scroll){
            place(ToggleControl+213,start,effectY(0)+5,40,22);place(213+optionLabel,start+51,effectY(0),180,effectHeight);
            place(214+optionLabel,start,effectY(1),78,effectHeight);field(214,start+78,effectY(1),main-78,effectHeight,true);
            place(217+optionLabel,start,effectY(2),78,effectHeight);place(ToggleControl+217,start+78,effectY(2)+5,40,22);
            int rows[]={218,215,216};
            for(int i=0;i<3;i++){
                int y=effectY(i+3);
                place(rows[i]+optionLabel,start,y,78,effectHeight);track(rows[i],start+78,y,main-78,effectHeight);
            }
        }else if(page==Outline||page==Glow){
            int row=0;
            for(const auto& option:options)if(option.page==page){
                int y=effectY(row++);
                if(option.kind==OptionKind::Choice){
                    place(ToggleControl+option.id,start,y+5,40,22);place(option.id+optionLabel,start+51,y,175,effectHeight);
                }else{
                    place(option.id+optionLabel,start,y,98,effectHeight);
                    if(option.kind==OptionKind::Color)field(option.id,start+98,y,main-98,effectHeight,false,true);
                    else track(option.id,start+98,y,main-98,effectHeight);
                }
            }
        }
        if(fontDpi!=dpi){
            auto make=[&](int size,int weight,const wchar_t* family=L"Microsoft YaHei UI"){
                return CreateFontW(-dip(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);
            };
            auto regular=make(14,FW_NORMAL),head=make(13,FW_NORMAL),section=make(22,FW_SEMIBOLD),
                muted=make(12,FW_NORMAL),strong=make(14,FW_SEMIBOLD),preview=make(24,FW_SEMIBOLD),
                brand=make(23,FW_BOLD,L"Segoe UI");
            EnumChildWindows(window,[](HWND child,LPARAM value)->BOOL{
                SendMessageW(child,WM_SETFONT,static_cast<WPARAM>(value),TRUE);return TRUE;
            },reinterpret_cast<LPARAM>(regular));
            SendMessageW(control(AppTitle),WM_SETFONT,reinterpret_cast<WPARAM>(brand),TRUE);
            SendMessageW(control(RegionTitle),WM_SETFONT,reinterpret_cast<WPARAM>(section),TRUE);
            SendMessageW(control(Text),WM_SETFONT,reinterpret_cast<WPARAM>(preview),TRUE);
            for(int id:{RegionsLabel,TextLabel,FontLabel,SizeLabel,VisualKindLabel})
                SendMessageW(control(id),WM_SETFONT,reinterpret_cast<WPARAM>(head),TRUE);
            for(const auto& option:options)
                SendMessageW(control(option.id+optionLabel),WM_SETFONT,reinterpret_cast<WPARAM>(head),TRUE);
            for(int id:{SaveState,Status,ReferenceStatus})
                SendMessageW(control(id),WM_SETFONT,reinterpret_cast<WPARAM>(muted),TRUE);
            for(auto prior:{font,headerFont,sectionFont,mutedFont,strongFont,previewFont,brandFont})if(prior)DeleteObject(prior);
            font=regular;headerFont=head;sectionFont=section;mutedFont=muted;strongFont=strong;previewFont=preview;brandFont=brand;fontDpi=dpi;
            SendMessageW(control(List),LB_SETITEMHEIGHT,0,dip(56));
        }
        auto invalidate_changed=[&](const auto& from,const auto& to){
            for(const auto& frame:from){
                if(std::none_of(to.begin(),to.end(),[&](const auto& other){
                    return other.id==frame.id&&EqualRect(&other.bounds,&frame.bounds);
                }))InvalidateRect(window,&frame.bounds,FALSE);
            }
        };
        invalidate_changed(oldFrames,frames);
        invalidate_changed(frames,oldFrames);
        show_page();
        order_tab_stops(tabPositions);
        if(paintedPage!=page){
            for(int id:{pageButton,pageButton+1,pageButton+2,pageButton+3,int(EffectScroll),pageButton+Outline,pageButton+Glow})
                InvalidateRect(control(id),nullptr,FALSE);
            paintedPage=page;
        }
        if(resized&&IsWindowVisible(window)){
            if(!resizePaintPending)resizePaintPending=PostMessageW(window,resizePaintMessage,0,0)!=FALSE;
        }else if(dpiChanged||sidebarChanged)
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|
                (IsWindowVisible(window)?RDW_UPDATENOW:0));
    }
    void end_edge_drag(){
        if(edgeDrag&&edgeDrag->outline)DestroyWindow(edgeDrag->outline);
        edgeDrag.reset();
        if(GetCapture()==window)ReleaseCapture();
    }
    RECT edge_drag_bounds(POINT cursor)const{
        const auto drag=*edgeDrag;
        const LONG dx=cursor.x-drag.origin.x,dy=cursor.y-drag.origin.y;
        RECT next=drag.bounds;
        auto dpi=GetDpiForWindow(window);
        const int minWidth=chrome::px(640,dpi),minHeight=chrome::px(525,dpi);
        switch(drag.edge){
        case HTLEFT: case HTTOPLEFT: case HTBOTTOMLEFT:
            next.left=std::min(next.left+dx,next.right-minWidth);break;
        case HTRIGHT: case HTTOPRIGHT: case HTBOTTOMRIGHT:
            next.right=std::max(next.right+dx,next.left+minWidth);break;
        }
        switch(drag.edge){
        case HTTOP: case HTTOPLEFT: case HTTOPRIGHT:
            next.top=std::min(next.top+dy,next.bottom-minHeight);break;
        case HTBOTTOM: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
            next.bottom=std::max(next.bottom+dy,next.top+minHeight);break;
        }
        return next;
    }
    void preview_edge_drag(const RECT& next){
        auto outline=edgeDrag->outline;
        if(!outline)return;
        int width=next.right-next.left,height=next.bottom-next.top;
        int stroke=chrome::px(2,GetDpiForWindow(window));
        auto border=CreateRectRgn(0,0,width,height);
        auto inner=CreateRectRgn(stroke,stroke,width-stroke,height-stroke);
        if(!border||!inner){
            if(border)DeleteObject(border);
            if(inner)DeleteObject(inner);
            return;
        }
        int result=CombineRgn(border,border,inner,RGN_DIFF);
        DeleteObject(inner);
        if(result==ERROR||!SetWindowRgn(outline,border,FALSE)){DeleteObject(border);return;}
        SetWindowPos(outline,HWND_TOP,next.left,next.top,width,height,
            SWP_NOACTIVATE|SWP_SHOWWINDOW|SWP_NOOWNERZORDER|SWP_NOSENDCHANGING);
    }
    static LRESULT CALLBACK field_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR kind,DWORD_PTR context){
        auto& panel=*reinterpret_cast<Panel*>(context);
        int id=GetDlgCtrlID(hwnd);
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,field_proc,kind);
        if(id==Text&&msg==WM_GETDLGCODE&&wp==VK_TAB)
            return DefSubclassProc(hwnd,msg,wp,lp)&~(DLGC_WANTTAB|DLGC_WANTALLKEYS);
        if(id==RegionScroll){
            int y=SHORT(HIWORD(lp));
            if(msg==WM_MOUSEWHEEL)return SendMessageW(panel.control(List),msg,wp,lp);
            if(msg==WM_LBUTTONDOWN){
                SetFocus(hwnd);
                auto thumb=panel.region_scroll_thumb(hwnd);
                panel.regionScrollOffset=y>=thumb.top&&y<thumb.bottom?y-thumb.top:(thumb.bottom-thumb.top)/2;
                SetCapture(hwnd);panel.drag_region_scroll(hwnd,y);
                InvalidateRect(hwnd,nullptr,FALSE);return 0;
            }
            if(msg==WM_MOUSEMOVE&&GetCapture()==hwnd){panel.drag_region_scroll(hwnd,y);return 0;}
            if(msg==WM_LBUTTONUP&&GetCapture()==hwnd){panel.drag_region_scroll(hwnd,y);ReleaseCapture();return 0;}
            if(msg==WM_CAPTURECHANGED)InvalidateRect(hwnd,nullptr,FALSE);
        }
        if(msg==WM_SETFOCUS||msg==WM_KILLFOCUS){
            panel.invalidate_field_frame(id);
            if(kind)InvalidateRect(hwnd,nullptr,FALSE);
        }
        if(kind&&msg==WM_ERASEBKGND)return 1;
        if(kind&&msg==WM_PAINT&&panel.theme.page){
            PAINTSTRUCT paint{};auto dc=BeginPaint(hwnd,&paint);
            RECT box{};GetClientRect(hwnd,&box);UINT dpi=GetDpiForWindow(hwnd);
            auto dip=[&](int n){return chrome::px(n,dpi);};
            if(kind==1){
                FillRect(dc,&box,panel.theme.field);
                RECT arrow{box.right-dip(23),box.top,box.right,box.bottom};
                chrome::icon(dc,arrow,chrome::Icon::Chevron,panel.theme.text,dpi);
                if((GetWindowLongPtrW(hwnd,GWL_STYLE)&3)==CBS_DROPDOWNLIST){
                    box.left+=dip(3);box.right-=dip(25);
                    auto prior=SelectObject(dc,panel.font);
                    chrome::label(dc,box,panel.contents(id).c_str(),IsWindowEnabled(hwnd)?panel.theme.text:panel.theme.disabled,DT_LEFT);
                    SelectObject(dc,prior);
                }
            }else if(id==RegionScroll){
                panel.paint_backdrop(dc,box,hwnd);
                auto center=(box.left+box.right)/2;
                RECT rail{center-dip(1),box.top+dip(5),center+dip(1),box.bottom-dip(5)};
                chrome::rounded(dc,rail,dip(1),panel.theme.border,panel.theme.border);
                auto handle=panel.region_scroll_thumb(hwnd);
                auto fill=GetCapture()==hwnd?panel.theme.accent:panel.theme.muted;
                chrome::rounded(dc,handle,dip(3),fill,fill);
                if(panel.keyboardFocus&&GetFocus()==hwnd)DrawFocusRect(dc,&box);
            }else{
                panel.paint_backdrop(dc,box,hwnd);
                int left=box.left+dip(6),right=box.right-dip(7),mid=(box.top+box.bottom)/2;
                RECT thumb{};SendMessageW(hwnd,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
                int knob=std::clamp(int((thumb.left+thumb.right)/2),left,right);
                RECT track{left,mid-dip(1),right,mid+dip(2)};
                chrome::rounded(dc,track,dip(1),panel.theme.border,panel.theme.border);
                if(knob>left){track.right=knob;chrome::rounded(dc,track,dip(1),panel.theme.accent,panel.theme.accent);}
                RECT dot{knob-dip(7),mid-dip(7),knob+dip(7),mid+dip(7)};
                chrome::rounded(dc,dot,dip(7),panel.theme.accent,panel.theme.accent);
                if(panel.keyboardFocus&&GetFocus()==hwnd)DrawFocusRect(dc,&box);
            }
            EndPaint(hwnd,&paint);return 0;
        }
        return DefSubclassProc(hwnd,msg,wp,lp);
    }
    static LRESULT CALLBACK font_edit_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,font_edit_proc,id);
        if(msg==WM_PAINT&&GetFocus()!=hwnd){
            auto range=SendMessageW(hwnd,EM_GETSEL,0,0);
            if(LOWORD(range)!=HIWORD(range))SendMessageW(hwnd,EM_SETSEL,0,0);
        }
        return DefSubclassProc(hwnd,msg,wp,lp);
    }
    double reference_hover()const{
        double end=hoveredButton==control(VisualBegin)?.22:0.;
        double progress=std::min(1.,double(GetTickCount64()-referenceHoverTime)/120.);
        return referenceHoverFrom+(end-referenceHoverFrom)*progress*(2.-progress);
    }
    static LRESULT CALLBACK button_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR context){
        auto& panel=*reinterpret_cast<Panel*>(context);
        if(msg==WM_MOUSEMOVE&&panel.hoveredButton!=hwnd){
            bool reference=id==VisualBegin||panel.hoveredButton==panel.control(VisualBegin);
            if(reference)panel.referenceHoverFrom=panel.reference_hover();
            auto previous=std::exchange(panel.hoveredButton,hwnd);
            if(reference){panel.referenceHoverTime=GetTickCount64();SetTimer(panel.window,referenceHoverTimer,16,nullptr);}
            if(previous)InvalidateRect(previous,nullptr,FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0};TrackMouseEvent(&tracking);
            InvalidateRect(hwnd,nullptr,FALSE);
        }else if(msg==WM_MOUSELEAVE||msg==WM_NCDESTROY){
            if(panel.hoveredButton==hwnd){
                if(id==VisualBegin&&msg==WM_MOUSELEAVE){
                    panel.referenceHoverFrom=panel.reference_hover();
                    panel.referenceHoverTime=GetTickCount64();SetTimer(panel.window,referenceHoverTimer,16,nullptr);
                }
                panel.hoveredButton=nullptr;InvalidateRect(hwnd,nullptr,FALSE);
            }
            if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,button_proc,id);
        }else if(msg==WM_KEYDOWN&&wp==VK_DOWN&&id==ConnectionStatus){
            SendMessageW(hwnd,BM_CLICK,0,0);return 0;
        }
        return DefSubclassProc(hwnd,msg,wp,lp);
    }
    LRESULT message(UINT msg,WPARAM wp,LPARAM lp){
        if(msg==WM_CREATE){
            auto add=[&](int id,const wchar_t* kind,const wchar_t* title,DWORD style,DWORD ex=0){
                auto child=CreateWindowExW(ex&~WS_EX_CLIENTEDGE,kind,title,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window,
                    reinterpret_cast<HMENU>(intptr_t(id)),GetModuleHandleW(nullptr),nullptr);
                if(wcscmp(kind,L"EDIT")==0||wcscmp(kind,L"COMBOBOX")==0||wcscmp(kind,TRACKBAR_CLASSW)==0){
                    UINT_PTR paint=wcscmp(kind,L"COMBOBOX")==0?1:wcscmp(kind,TRACKBAR_CLASSW)==0?2:0;
                    SetWindowSubclass(child,field_proc,paint,reinterpret_cast<DWORD_PTR>(this));
                }
                return child;
            };
            auto label=[&](int id,const wchar_t* title){
                add(id,L"STATIC",title,SS_OWNERDRAW|SS_NOPREFIX|(id==RegionTitle?SS_NOTIFY:0));
            };
            auto button=[&](int id,const wchar_t* title){
                auto child=add(id,L"BUTTON",title,WS_TABSTOP|BS_OWNERDRAW);
                if(child)SetWindowSubclass(child,button_proc,id,reinterpret_cast<DWORD_PTR>(this));
            };
            auto combo=[&](int id){return add(id,L"COMBOBOX",L"",WS_TABSTOP|WS_CLIPCHILDREN|CBS_DROPDOWNLIST|WS_VSCROLL);};
            auto item=[](HWND list,const wchar_t* title){SendMessageW(list,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));};
            label(RegionsLabel,L"遮盖区域");label(TextLabel,L"遮盖文字");label(FontLabel,L"字体");label(SizeLabel,L"字号");
            auto regionsList=add(List,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT);
            SetWindowSubclass(regionsList,region_list_proc,0,reinterpret_cast<DWORD_PTR>(this));
            add(RegionScroll,TRACKBAR_CLASSW,L"覆盖区域滚动",WS_TABSTOP|TBS_NOTICKS|TBS_VERT);
            add(Text,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN);
            auto dropdown=add(Font,L"COMBOBOX",L"",WS_TABSTOP|WS_CLIPCHILDREN|CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_VSCROLL);
            COMBOBOXINFO fontEditor{sizeof(fontEditor)};
            if(GetComboBoxInfo(dropdown,&fontEditor))
                SetWindowSubclass(fontEditor.hwndItem,font_edit_proc,0,0);
            for(const auto* family:{L"Segoe UI",L"Microsoft YaHei UI",L"Consolas",L"Arial"})
                item(dropdown,family);
            add(Size,L"EDIT",L"",WS_TABSTOP|ES_NUMBER,WS_EX_CLIENTEDGE);
            auto appearance=combo(ThemeChoice);
            for(const auto* title:{L"跟随系统",L"浅色",L"深色"})item(appearance,title);
            theme.load();SendMessageW(appearance,CB_SETCURSEL,theme.mode,0);
            for(int i=0;i<PageCount;i++)button(pageButton+i,i==Scroll?L"效果":pageLabels[i]);
            label(AppTitle,L"Overlay");button(ConnectionStatus,L"等待连接");
            EnableWindow(control(ConnectionStatus),FALSE);
            button(Appearance,L"界面主题");button(Settings,L"应用设置");button(ClosePanel,L"关闭面板");
            label(RegionTitle,L"选择遮盖区域");label(SaveState,L"已保存");
            button(BoldButton,L"粗体");
            button(PositionAdvanced,L"高级定位设置");label(ReferenceStatus,L"当前使用窗口锚定。选择参考区域并确认后，启用视觉定位。");
            label(PositionContext,L"当前定位 · 窗口锚定");
            button(EffectScroll,L"滚动");
            for(const auto& option:options){
                label(option.id+optionLabel,option.label);
                if(option.kind==OptionKind::Choice){
                    auto list=combo(option.id);
                    for(auto choice=option.choices;choice->label;choice++)item(list,choice->label);
                }else add(option.id,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,WS_EX_CLIENTEDGE);
                if(option.kind==OptionKind::Color)button(option.id+optionPicker,L"选颜色");
            }
            for(int id:{205,206,207,208,int(RelationPadding),int(DetailValue),DetailValue+1,DetailValue+2,DetailValue+3}){
                button(StepButton+2*id,L"−");button(StepButton+2*id+1,L"+");
            }
            for(int id:sliders){
                const auto* option=option_at(id);
                auto slider=add(TrackControl+id,TRACKBAR_CLASSW,option->label,WS_TABSTOP|TBS_HORZ|TBS_NOTICKS);
                SendMessageW(slider,TBM_SETRANGEMIN,FALSE,LPARAM(option->minimum*slider_units(id)));
                SendMessageW(slider,TBM_SETRANGEMAX,TRUE,LPARAM(option->maximum*slider_units(id)));
                SendMessageW(slider,TBM_SETLINESIZE,0,slider_units(id));
            }
            for(int id:switches)button(ToggleControl+id,option_at(id)->label);
            button(AddRegion,L"新建");button(DeleteRegion,L"删除区域");button(ClearRegions,L"清空");button(MoveToTarget,L"移到目标显示器");
            button(FreeRegion,L"全窗口框选");
            button(PickTextColor,L"取色");button(PickColor,L"取色");
            label(VisualKindLabel,L"参考内容");label(VisualDistanceLabel,L"找回距离 (DIP)");label(VisualIntervalLabel,L"兜底检查");
            auto kind=combo(VisualKind);item(kind,L"文字／图案位置（无需边框）");item(kind,L"完整边框区域");SendMessageW(kind,CB_SETCURSEL,0,0);
            add(VisualDistance,L"EDIT",L"500",WS_TABSTOP|ES_NUMBER,WS_EX_CLIENTEDGE);
            auto interval=combo(VisualInterval);
            for(const auto* title:{L"只随窗口变化",L"15 秒",L"30 秒",L"60 秒"})item(interval,title);
            SendMessageW(interval,CB_SETCURSEL,1,0);
            button(VisualBegin,L"选择参考区域…");button(VisualConfirm,L"确认校准");button(VisualCancel,L"取消校准");
            button(VisualReselect,L"重新框选");button(VisualRetry,L"重试校准");button(VisualAnchor,L"改用窗口锚定");
            button(VisualResume,L"恢复校准");button(VisualRefresh,L"重新定位");button(VisualSaveDistance,L"保存找回距离");
            button(VisualCheckTab,L"校准与检查");
            button(VisualRelationTab,L"遮盖关系");
            button(VisualReferenceTab,L"参考框微调");
            button(CoverDrag,L"拖动遮盖");
            label(RelationModeLabel,L"遮盖跟随方式");auto mode=combo(RelationMode);
            item(mode,L"只跟位置，固定尺寸");item(mode,L"跟随参考框四边");SendMessageW(mode,CB_SETCURSEL,0,0);
            label(RelationXLabel,L"水平基准");label(RelationYLabel,L"垂直基准");
            auto horizontal=combo(RelationX),vertical=combo(RelationY);
            for(auto choice=horizontalChoices;choice->label;choice++)item(horizontal,choice->label);
            for(auto choice=verticalChoices;choice->label;choice++)item(vertical,choice->label);
            label(RelationPaddingLabel,L"防漏边物理像素");add(RelationPadding,L"EDIT",L"0",WS_TABSTOP|ES_NUMBER,WS_EX_CLIENTEDGE);
            for(int i=0;i<4;i++){
                label(DetailLabel+i,L"");add(DetailValue+i,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,WS_EX_CLIENTEDGE);
            }
            button(DetailApply,L"应用定位设置");
            button(Apply,L"应用");button(Done,L"完成");label(Status,L"正在连接");
            tooltips=CreateWindowExW(0,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_NOPREFIX,
                CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(tooltips){
                SendMessageW(tooltips,CCM_SETUNICODEFORMAT,TRUE,0);
                SendMessageW(tooltips,TTM_SETMAXTIPWIDTH,0,chrome::px(420,GetDpiForWindow(window)));
                for(int id:std::array<int,11>{Appearance,Settings,ClosePanel,RegionTitle,ConnectionStatus,AddRegion,
                    FreeRegion,ClearRegions,BoldButton,PickTextColor,PickColor}){
                    TTTOOLINFOW info{TTTOOLINFOW_V2_SIZE};info.hwnd=window;
                    info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.uId=reinterpret_cast<UINT_PTR>(control(id));
                    info.lpszText=LPSTR_TEXTCALLBACKW;
                    SendMessageW(tooltips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
                }
            }
            theme.apply(window);layout();update_actions();return 0;
        }
        if(msg==WM_NCCALCSIZE)return wp?WVR_REDRAW:0;
        if((msg==WM_NCACTIVATE||msg==WM_NCPAINT)&&!theme.highContrast&&!IsIconic(window)){
            if(msg==WM_NCACTIVATE)return DefWindowProcW(window,msg,wp,-1);
            if(msg==WM_NCPAINT)return 0;
        }
        if(msg==WM_NCHITTEST){
            RECT bounds{};GetWindowRect(window,&bounds);
            int x=SHORT(LOWORD(lp)),y=SHORT(HIWORD(lp)),margin=chrome::px(5,GetDpiForWindow(window));
            bool left=x<bounds.left+margin,right=x>=bounds.right-margin,top=y<bounds.top+margin,bottom=y>=bounds.bottom-margin;
            if(top)return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
            if(bottom)return left?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
            if(left)return HTLEFT;if(right)return HTRIGHT;
            for(int id:{ConnectionStatus,Appearance,Settings,ClosePanel}){
                auto child=control(id);RECT button{};
                if(IsWindowVisible(child)&&GetWindowRect(child,&button)&&PtInRect(&button,POINT{x,y}))return HTCLIENT;
            }
            if(y<bounds.top+chrome::px(65,GetDpiForWindow(window)))return HTCAPTION;
            return HTCLIENT;
        }
        // Preview the proposed bounds without stretching the composited editor.
        if(msg==WM_NCLBUTTONDOWN&&wp>=HTLEFT&&wp<=HTBOTTOMRIGHT&&!IsZoomed(window)){
            popup.close(false);
            RECT bounds{};GetWindowRect(window,&bounds);
            auto outline=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,
                L"OverlayEditorResizeOutline",nullptr,WS_POPUP,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            edgeDrag=EdgeDrag{bounds,{SHORT(LOWORD(lp)),SHORT(HIWORD(lp))},int(wp),outline};
            SetCapture(window);
            if(GetCapture()!=window||!outline){end_edge_drag();return DefWindowProcW(window,msg,wp,lp);}
            return 0;
        }
        if(edgeDrag){
            if(msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP){
                POINT cursor{SHORT(LOWORD(lp)),SHORT(HIWORD(lp))};
                ClientToScreen(window,&cursor);
                auto next=edge_drag_bounds(cursor);
                if(msg==WM_MOUSEMOVE)preview_edge_drag(next);
                else{
                    auto original=edgeDrag->bounds;
                    end_edge_drag();
                    if(!EqualRect(&next,&original)){
                        bool restoreFocus=GetForegroundWindow()==window;
                        HWND focused=GetFocus();
                        // Re-present the window at its final size. Keeping its existing
                        // redirection surface visible can leave the old outer edge on screen.
                        ShowWindow(window,SW_HIDE);
                        SetWindowPos(window,nullptr,next.left,next.top,next.right-next.left,next.bottom-next.top,
                            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
                        present(false,restoreFocus,false);
                        if(focused&&IsChild(window,focused)&&IsWindowVisible(focused))SetFocus(focused);
                    }
                }
                return 0;
            }
            if(msg==WM_CAPTURECHANGED||msg==WM_CANCELMODE)end_edge_drag();
        }
        if(msg==WM_DPICHANGED){
            popup.close(false);
            auto rect=reinterpret_cast<const RECT*>(lp);
            if(tooltips)SendMessageW(tooltips,TTM_SETMAXTIPWIDTH,0,chrome::px(420,LOWORD(wp)));
            SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,
                SWP_NOZORDER|SWP_NOACTIVATE);
            if(layoutDpi!=GetDpiForWindow(window))layout();
            return 0;
        }
        if(msg==WM_SIZE){
            if(popup.open())popup.close(false);
            if(wp!=SIZE_MINIMIZED&&control(Status)&&(LOWORD(lp)!=layoutSize.cx||HIWORD(lp)!=layoutSize.cy||
                layoutDpi!=GetDpiForWindow(window)))layout();
            return 0;
        }
        if(msg==resizePaintMessage){
            resizePaintPending=false;
            if(IsWindowVisible(window)&&!IsIconic(window)){
                PaintBatch frame(window);
                RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            }
            return 0;
        }
        if(msg==WM_MOVE){if(popup.open())popup.close(false);return 0;}
        if(msg==WM_TIMER&&wp==presentationTimer){
            finish_presentation(true);return 0;
        }
        if(msg==WM_TIMER&&wp==referenceHoverTimer){
            auto button=control(VisualBegin);
            if(IsWindowVisible(button))InvalidateRect(button,nullptr,FALSE);
            if(!IsWindowVisible(button)||GetTickCount64()-referenceHoverTime>=120)KillTimer(window,referenceHoverTimer);
            return 0;
        }
        if(msg==WM_GETMINMAXINFO){
            auto info=reinterpret_cast<MINMAXINFO*>(lp);auto dpi=GetDpiForWindow(window);
            info->ptMinTrackSize={MulDiv(640,dpi,96),MulDiv(525,dpi,96)};return 0;
        }
        if(msg==WM_ERASEBKGND&&theme.page)return 1;
        if(msg==WM_PAINT&&theme.page){
            PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);UINT dpi=GetDpiForWindow(window);
            paint_backdrop(dc,paint.rcPaint);
            HWND focused=GetFocus();
            for(const auto& item:frames){
                auto child=control(item.id);if(!IsWindowVisible(child)||!RectVisible(dc,&item.bounds))continue;
                bool active=child==focused||(focused&&IsChild(child,focused));
                auto fill=item.color?theme.button:theme.input;
                if(item.id==Text){
                    if(active&&keyboardFocus)DrawFocusRect(dc,&item.bounds);
                    auto y=item.bounds.bottom+chrome::px(9,dpi);
                    RECT separator{item.bounds.left,y,item.bounds.right,y+1};
                    auto line=CreateSolidBrush(theme.border);
                    FillRect(dc,&separator,line);DeleteObject(line);
                }else chrome::rounded(dc,item.bounds,chrome::px(6,dpi),fill,active?theme.focus:theme.border);
            }
            EndPaint(window,&paint);return 0;
        }
        if((msg==WM_CTLCOLOREDIT||msg==WM_CTLCOLORLISTBOX||msg==WM_CTLCOLORSTATIC)&&theme.page){
            auto dc=reinterpret_cast<HDC>(wp);auto child=reinterpret_cast<HWND>(lp);
            auto id=GetDlgCtrlID(child);
            auto fontControl=control(Font);
            bool field=child!=control(List)&&(msg!=WM_CTLCOLORSTATIC||GetParent(child)==fontControl||
                std::any_of(frames.begin(),frames.end(),[id](const auto& frame){return frame.id==id;}));
            SetTextColor(dc,label_color(child));
            if(id==Text){SetBkColor(dc,theme.paper);return reinterpret_cast<LRESULT>(theme.paperField);}
            if(id==List){SetBkColor(dc,theme.rail);return reinterpret_cast<LRESULT>(theme.railField);}
            auto option=option_at(id);
            if(option&&option->kind==OptionKind::Color){
                SetBkColor(dc,theme.button);return reinterpret_cast<LRESULT>(theme.buttonField);
            }
            SetBkColor(dc,field?theme.input:theme.background);
            return reinterpret_cast<LRESULT>(field?theme.field:theme.page);
        }
        if(msg==WM_DRAWITEM&&theme.page){
            const auto& item=*reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if(item.CtlType==ODT_BUTTON||item.CtlType==ODT_LISTBOX||item.CtlType==ODT_STATIC){paint_item(item);return TRUE;}
        }
        if(msg==WM_NOTIFY&&lp&&reinterpret_cast<const NMHDR*>(lp)->hwndFrom==tooltips&&
            reinterpret_cast<const NMHDR*>(lp)->code==TTN_GETDISPINFOW){
            auto& tip=*reinterpret_cast<NMTTDISPINFOW*>(lp);
            tooltipText=contents(GetDlgCtrlID(reinterpret_cast<HWND>(tip.hdr.idFrom)));
            tip.lpszText=tooltipText.data();
            return 0;
        }
        if(msg==WM_MEASUREITEM){
            auto& item=*reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if(item.CtlID==List){item.itemHeight=chrome::px(56,GetDpiForWindow(window));return TRUE;}
        }
        if(msg==WM_CONTEXTMENU&&reinterpret_cast<HWND>(wp)==control(List)){region_menu(lp);return 0;}
        if(msg==WM_VSCROLL&&lp&&reinterpret_cast<HWND>(lp)==control(RegionScroll)){
            auto bar=control(RegionScroll);
            scroll_regions(int(SendMessageW(bar,TBM_GETPOS,0,0)));
            InvalidateRect(bar,nullptr,FALSE);return 0;
        }
        if(msg==WM_HSCROLL&&lp){
            if(selecting())return 0;
            auto track=reinterpret_cast<HWND>(lp);auto id=GetDlgCtrlID(track)-TrackControl;
            if(std::find(std::begin(sliders),std::end(sliders),id)!=std::end(sliders)){
                auto text=number_text(SendMessageW(track,TBM_GETPOS,0,0)/double(slider_units(id)));
                SetWindowTextW(control(id),text.c_str());InvalidateRect(track,nullptr,TRUE);return 0;
            }
        }
        if(msg==WM_SETTINGCHANGE||msg==WM_SYSCOLORCHANGE){popup.close(false);theme.apply(window);return 0;}
        if(msg==WM_COMMAND){
            int id=LOWORD(wp),notice=HIWORD(wp);
            if(id==Font&&(notice==CBN_SETFOCUS||notice==CBN_KILLFOCUS))invalidate_field_frame(Font);
            if(interaction_locked()&&id!=Done&&id!=ClosePanel){
                if(id==List&&notice==LBN_SELCHANGE)
                    mark_selected_row(selecting()?pendingSelection:selected);
                return 0;
            }
            if(!syncing&&((notice==EN_CHANGE&&(id==Text||id==Size||option_at(id)))||
                (notice==CBN_SELCHANGE&&option_at(id))||(id==Font&&(notice==CBN_EDITCHANGE||notice==CBN_SELCHANGE)))){
                update_option_chrome(id);update_actions();
            }
            if(id==ThemeChoice&&notice==CBN_SELCHANGE){
                auto index=SendMessageW(control(ThemeChoice),CB_GETCURSEL,0,0);
                if(index>=0&&index<=2){theme.mode=int(index);theme.save();theme.apply(window);}
            }else if(id==Font&&notice==CBN_DROPDOWN){load_fonts();}
            else if(id==List&&notice==LBN_SELCHANGE){
                auto i=SendMessageW(control(List),LB_GETCURSEL,0,0);
                if(!busy&&i>=0&&size_t(i)<regions.size()){
                    auto id=regions[i].at("id").get<std::string>();
                    if(id==selected)return 0;
                    if(dirty()){
                        mark_selected_row(selected);
                        status(L"请先应用当前修改");
                    }else{
                        pendingSelection=std::move(id);
                        submit({"select",{{"id",pendingSelection}}});
                    }
                }
            }else if((id==RelationMode||id==RelationX||id==RelationY)&&notice==CBN_SELCHANGE){
                if(IsWindowEnabled(control(id)))rebase_relation(id);
            }else if(id==VisualInterval&&notice==CBN_SELCHANGE){
                auto index=SendMessageW(control(id),CB_GETCURSEL,0,0);
                if(index>=0&&size_t(index)<std::size(checkIntervals)){
                    if(dirty()){
                        auto old=std::find(std::begin(checkIntervals),std::end(checkIntervals),checkInterval);
                        if(old!=std::end(checkIntervals))SendMessageW(control(id),CB_SETCURSEL,old-std::begin(checkIntervals),0);
                    }
                    command("set_visual_check_interval",{{"seconds",checkIntervals[index]}});
                }
            }else if(notice==BN_CLICKED){
                if(id==ConnectionStatus&&applicationMenuAvailable&&connected&&!interaction_locked()){
                    listingRegionTargets=false;pickerManagement=false;
                    set_busy(true);
                    submit({"list_applications",json::object()});
                }
                else if(id==Apply)apply(false);
                else if(id==Done)apply(true);
                else if(id==ClosePanel)apply(true);
                else if(id==Appearance||id==Settings){if(!keyboardFocus)SetFocus(window);popup_header(id);}
                else if(id==Sidebar){
                    sidebarOpen=!sidebarShown;
                    if(!keyboardFocus)SetFocus(window);layout();
                }
                else if(id==PositionAdvanced){page=page==Visual?Position:Visual;layout();show_calibration_state();}
                else if(id>=VisualCheckTab&&id<=VisualReferenceTab&&visualDetail!=id-VisualCheckTab){
                    int previous=visualDetail;
                    visualDetail=id-VisualCheckTab;
                    load_visual_detail(true);layout();update_visual_buttons();show_calibration_state();
                    InvalidateRect(control(VisualCheckTab+previous),nullptr,FALSE);
                    InvalidateRect(control(id),nullptr,FALSE);
                    RECT area{};GetClientRect(window,&area);
                    RECT tab{};GetWindowRect(control(VisualCheckTab),&tab);
                    MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&tab),2);
                    area.left=tab.left-chrome::px(12,layoutDpi);
                    area.top=tab.bottom;
                    area.bottom=std::min<LONG>(area.bottom,chrome::px(489,layoutDpi));
                    if(area.top<area.bottom)
                        RedrawWindow(window,&area,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
                }
                else if(id==EffectScroll){page=Scroll;layout();}
                else if(id>=pageButton&&id<pageButton+PageCount){
                    page=Page(id-pageButton);layout();
                    if(page==Position||page==Visual)show_calibration_state();
                }
                else if(id==BoldButton){
                    select_choice(200,SendMessageW(control(200),CB_GETCURSEL,0,0)!=1);update_actions();
                }
                else if(id>=StepButton&&id<TrackControl){
                    int field=(id-StepButton)/2;
                    double delta=(id-StepButton)%2?1.:-1.;
                    if(field==RelationPadding){
                        if(auto value=detail_number(field,0,2)){
                            auto text=detail_number_text(std::clamp(*value+delta,0.,2.));
                            SetWindowTextW(control(field),text.c_str());
                        }
                        return 0;
                    }
                    if(field>=DetailValue&&field<DetailValue+4){
                        if(GetKeyState(VK_SHIFT)&0x8000)delta*=0.1;
                        int index=field-DetailValue;
                        bool margins=visualDetail==1&&relationForm.is_object()&&relationForm.at("mode")=="follow_bounds";
                        double minimum=margins?-4096:index>=2?8:visualDetail==2?0:-100000;
                        double maximum=margins?4096:100000;
                        if(auto value=detail_number(field,minimum,maximum)){
                            auto text=detail_number_text(std::clamp(*value+delta,minimum,maximum));
                            SetWindowTextW(control(field),text.c_str());
                        }
                        return 0;
                    }
                    const auto* option=option_at(field);
                    if(!option)return 0;
                    if(auto value=detail_number(field,option->minimum,option->maximum)){
                        delta=(id-StepButton)%2?1.:-1.;
                        auto text=number_text(std::clamp(*value+delta,option->minimum,option->maximum));
                        SetWindowTextW(control(field),text.c_str());
                    }
                }else if(id>=ToggleControl+200){
                    int field=id-ToggleControl;
                    select_choice(field,SendMessageW(control(field),CB_GETCURSEL,0,0)!=1);update_actions();
                }
                else if(id==MoveToTarget)move_to_target();
                else if(id==AddRegion&&IsWindowEnabled(control(id))){
                    if(canvasModes)command("set_canvas_mode",{{"mode","once"}},true);
                    else command("create",json::object());
                }
                else if(id==FreeRegion&&IsWindowEnabled(control(id))){
                    if(canvasMode=="free")stop_canvas_mode();
                    else command("set_canvas_mode",{{"mode","free"}},true);
                }
                else if(id==ClearRegions&&IsWindowEnabled(control(id))){
                    if(dirty()){status(L"请先应用当前修改");return 0;}
                    auto revision=displayedRevision;
                    bool hasAppWide=std::any_of(regions.begin(),regions.end(),[](const auto& region){return region.value("window_profile",std::string()).empty();});
                    auto warning=L"将删除当前列表中的 "+std::to_wstring(regions.size())+L" 个遮盖区域。";
                    if(hasAppWide)warning+=L"\n\n「跟随应用」的区域也会从该应用的其他窗口消失。";
                    if(MessageBoxW(window,warning.c_str(),L"清空遮盖区域",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES&&
                        revision==displayedRevision)
                        command("clear_regions",json::object());
                }
                else if(id==DeleteRegion&&IsWindowEnabled(control(id))&&!selected.empty()){
                    RECT button{};GetWindowRect(control(id),&button);
                    auto regionId=selected;
                    confirm_delete(regionId,{button.left,button.bottom});
                }else if((id==PickColor||id==PickTextColor)&&IsWindowEnabled(control(id)))
                    command("pick_color",{{"id",selected},{"field",id==PickTextColor?"color":"background"}},true);
                else if(id==CoverDrag&&IsWindowEnabled(control(id))){
                    if(canvasMode=="cover")stop_canvas_mode();
                    else command("begin_cover_drag",{{"id",selected}},true);
                }
                else if(id==DetailApply)apply_visual_detail();
                else if(id>=VisualBegin&&id<=VisualSaveDistance)visual_command(id);
                else for(const auto& option:options)if(option.kind==OptionKind::Color&&id==option.id+optionPicker)
                    choose_color(option);
            }
            return 0;
        }
        if(msg==resultMessage){
            std::unique_ptr<Reply> reply(reinterpret_cast<Reply*>(lp));const auto& value=reply->value;
            const bool snapshot=reply->op=="snapshot";
            const bool selection=reply->op=="select";
            if(reply->op=="failure"){
                pendingSelection.clear();
                connected=false;
                if(!IsWindowVisible(window)){DestroyWindow(window);return 0;}
                set_busy(true);EnableWindow(control(Done),TRUE);
                SetWindowTextW(control(Status),L"连接已断开，可以关闭面板");
                return 0;
            }
            if(selection)pendingSelection.clear();
            else if(!snapshot)set_busy(false);
            if(reply->op=="list_applications"&&value.value("ok",false)){
                if(!finishRequested){
                    if(listingRegionTargets)popup_region_targets(value);
                    else{
                        pickerSnapshot=value;
                        expandedPickerApp=value.at("active_application").get<std::string>();
                        pickerAction.clear();
                        show_picker();
                    }
                }
                if(finishRequested&&!interaction_locked()){finishRequested=false;apply(true);}
                return 0;
            }
            if(!value.value("ok",false)){
                if(selection)mark_selected_row(selected);
                finishRequested=false;
                auto error=value.value("error",std::string());
                if(error=="revision_conflict"){
                    displayedRevision=value.value("revision",displayedRevision);
                    SetWindowTextW(control(Status),L"区域已变化，请确认后重试");
                }else if(error=="window_profile_delete_failed"){
                    status(L"窗口配置删除未完成，请在应用菜单中重试。");
                }else if(reply->op.find("calibration")!=std::string::npos||reply->op=="refresh_visual"||
                    reply->op=="set_position_mode"||reply->op=="set_visual_search_limit"||
                    reply->op=="set_visual_check_interval"||reply->op=="update_cover_relation"){
                    calibration_notice(calibration_error(error),true);
                }else SetWindowTextW(control(Status),L"提交失败，请重试");
                return 0;
            }
            if(reply->op=="commit_close"){DestroyWindow(window);return 0;}
            if(value.contains("state")){
                const auto& current=value.at("state");
                targetWindow=reinterpret_cast<HWND>(current.value("target_hwnd",uintptr_t(0)));
                enginePid=current.value("engine_pid",DWORD(0));
                preview=current.value("calibration_preview",json(nullptr));
                visualAvailable=current.value("visual_worker_available",false);
                waitingTarget=current.value("waiting_for_target",false);
                bool hasModes=current.value("canvas_modes",false);
                regionScopes=current.value("region_scopes",false);
                if(canvasModes!=hasModes){canvasModes=hasModes;layout();}
                auto oldCanvasMode=std::exchange(canvasMode,current.value("canvas_mode",std::string("idle")));
                auto draftRelation=current.value("cover_drag_relation",json(nullptr));
                bool draftChanged=draftRelation!=coverDragRelation;
                if(draftChanged){
                    coverDragRelation=draftRelation;
                }
                set_text(FreeRegion,canvasMode=="free"?L"结束框选":L"全窗口框选");
                auto applicationName=wide(current.value("application_name",std::string("ChatGPT")));
                applicationMenuAvailable=current.value("canvas_modes",false);
                hasApplication=!current.value("application_id",std::string()).empty();
                wchar_t title[512]{};
                if(targetWindow&&!waitingTarget)GetWindowTextW(targetWindow,title,int(std::size(title)));
                targetAppLabel=applicationMenuAvailable&&!hasApplication?L"":applicationName;
                targetWindowLabel=waitingTarget?L"请选择窗口":title[0]?title:L"无标题窗口";
                set_text(ConnectionStatus,targetAppLabel.empty()?L"选择应用":targetAppLabel+L"  /  "+targetWindowLabel);
                EnableWindow(control(ConnectionStatus),applicationMenuAvailable);
                calibrationBusy=current.value("calibration_busy",false);
                canRetry=current.value("calibration_retry_available",false);
                checkInterval=current.value("visual_check_interval_seconds",15);
                auto interval=std::find(std::begin(checkIntervals),std::end(checkIntervals),checkInterval);
                if(interval!=std::end(checkIntervals))select_choice(VisualInterval,interval-std::begin(checkIntervals));
                if((!snapshot&&reply->op!="switch_application"&&reply->op!="set_canvas_mode")||(!interaction_locked()&&!dirty()))
                    state(current,reply->op=="update");
                if(selection)mark_selected_row(selected);
                if(!snapshot)status(visual_region()?L"视觉定位 · 使用已保存的参考区域":L"窗口锚定 · 随所选窗口边缘或中心定位");
                if(canvasMode!=oldCanvasMode||reply->op=="set_canvas_mode"){
                    if(canvasMode=="once")status(L"在目标窗口拖出一个区域，完成后恢复鼠标穿透。");
                    else if(canvasMode=="free")status(L"在目标窗口连续添加区域，按 Esc 或点「结束框选」退出。");
                    else if(canvasMode=="cover")status(L"拖动遮盖区域，松开鼠标后确认位置。");
                    else if(canvasMode=="cover_draft")status(L"遮盖位置已更新，可继续拖动或保存遮盖关系。");
                    else status(L"已恢复鼠标穿透。");
                }
                if(current.value("pick_error",std::string())=="target_pixel_unavailable")
                    status(L"无法取色，请点击目标窗口内未被遮挡的位置。");
                load_visual_detail(draftChanged&&visualDetail==1);
                update_visual_buttons();
                auto sequence=current.value("finish_sequence",uint64_t(0));
                if(sequence>finishSequence){finishSequence=sequence;finishRequested=true;}
                auto restore=current.value("editor_restore_sequence",uint64_t(0));
                bool requested=restore!=restoreSequence;restoreSequence=restore;
                bool visible=current.value("visible",true);
                if(reply->op=="hello"&&targetWindow)move_to_target();
                bool canPresent=!snapshot||!busy;
                if(canPresent&&!visible&&IsWindowVisible(window)){
                    popup.close(false);
                    ShowWindow(window,SW_HIDE);
                    if(presentationPending)finish_presentation(false);
                }
                auto focus=current.value("focus_text_sequence",uint64_t(0));
                bool focusText=focus!=focusSequence&&visible;
                if(focusText){page=Typography;layout();}
                focusSequence=focus;
                calibrationResult=current.value("calibration_result",std::string());
                calibrationError=current.value("visual_error",std::string());
                show_calibration_state();
                show_page();
                if(canPresent&&visible){
                    bool activate=requested||(reply->op=="hello"&&targetWindow&&GetForegroundWindow()==targetWindow);
                    present(requested&&IsIconic(window),activate,focusText);
                }
            }
            if(reply->op=="hello")connected=true;
            if(finishRequested&&!interaction_locked()){finishRequested=false;apply(true);}
            return 0;
        }
        if(msg==WM_CLOSE){apply(true);return 0;}
        if(msg==WM_DESTROY){if(edgeDrag)end_edge_drag();popup.close(false);PostQuitMessage(0);return 0;}
        return DefWindowProcW(window,msg,wp,lp);
    }
    COLORREF label_color(HWND child)const{
        int id=GetDlgCtrlID(child);
        if(id==ReferenceStatus&&calibrationWarning)return theme.warning;
        if(id==RegionsLabel||id==SaveState||id==Status||id==ReferenceStatus||id==PositionContext)return theme.muted;
        return IsWindowEnabled(child)?theme.text:theme.disabled;
    }
    static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        auto self=reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE){
            self=static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self)return DefWindowProcW(hwnd,msg,wp,lp);
        int controlId=msg==WM_COMMAND?LOWORD(wp):0;
        bool tabChange=msg==WM_COMMAND&&HIWORD(wp)==BN_CLICKED&&!self->interaction_locked()&&
            ((controlId>=pageButton&&controlId<pageButton+PageCount&&controlId!=pageButton+self->page)||
             (controlId==EffectScroll&&self->page!=Scroll)||controlId==PositionAdvanced||
             (controlId>=VisualCheckTab&&controlId<=VisualReferenceTab&&controlId-VisualCheckTab!=self->visualDetail));
        auto reply=msg==resultMessage?reinterpret_cast<const Reply*>(lp):nullptr;
        bool regionChange=reply&&reply->op=="select"&&reply->value.value("ok",false);
        std::optional<PaintBatch> frame;
        if((tabChange||regionChange)&&IsWindowVisible(hwnd)&&!IsIconic(hwnd))frame.emplace(hwnd);
        auto previousPage=self->page;
        auto previousRegion=msg==resultMessage?self->selected:std::string();
        auto result=self->message(msg,wp,lp);
        if((self->page!=previousPage||(msg==resultMessage&&self->selected!=previousRegion))&&IsWindowVisible(hwnd)){
            auto dpi=GetDpiForWindow(hwnd);
            RECT editor=self->paneBounds;
            editor.top=chrome::px(70,dpi);
            RedrawWindow(hwnd,&editor,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
        }
        return result;
    }
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES};InitCommonControlsEx(&controls);
    chrome::DrawingSession drawing;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    Panel panel;
    for(int i=1;i+1<argc;i+=2){
        std::wstring key=argv[i];
        if(key==L"--pipe")panel.pipe=argv[i+1];
        if(key==L"--session")panel.token=utf8(argv[i+1]);
    }
    LocalFree(argv);
    if(panel.pipe.empty()||panel.token.size()!=32)return 1;
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpszClassName=L"OverlayNativePanelTrial";wc.lpfnWndProc=Panel::proc;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if(!RegisterClassW(&wc))return 2;
    WNDCLASSW outline{};
    outline.hInstance=instance;outline.lpszClassName=L"OverlayEditorResizeOutline";outline.lpfnWndProc=DefWindowProcW;
    outline.hbrBackground=GetSysColorBrush(COLOR_HIGHLIGHT);
    if(!RegisterClassW(&outline))return 2;
    auto dpi=GetDpiForSystem();
    // Compose the child controls together so a tab never presents a partly painted form.
    auto hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_COMPOSITED,wc.lpszClassName,L"ChatGPT Overlay · 编辑区域",
        WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(976,dpi,96),MulDiv(577,dpi,96),nullptr,nullptr,instance,&panel);
    if(!hwnd)return 3;
    DWORD corner=DWMWCP_ROUND;DwmSetWindowAttribute(hwnd,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
    std::thread worker([&]{panel.work();});
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){
        if(panel.popup.handles(message.hwnd)){
            TranslateMessage(&message);DispatchMessageW(&message);continue;
        }
        if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE&&panel.edgeDrag){
            panel.end_edge_drag();
            continue;
        }
        if(panel.block_pending_input(message))continue;
        if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE&&panel.canvasMode!="idle"){
            panel.stop_canvas_mode();continue;
        }
        if(message.message==WM_KEYDOWN||message.message==WM_SYSKEYDOWN)panel.keyboardFocus=true;
        else if(message.message==WM_LBUTTONDOWN||message.message==WM_RBUTTONDOWN)panel.keyboardFocus=false;
        if(!IsDialogMessageW(hwnd,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    {std::lock_guard<std::mutex> guard(panel.lock);panel.stopping=true;}
    panel.ready.notify_one();worker.join();
    for(auto font:{panel.font,panel.headerFont,panel.sectionFont,panel.mutedFont,panel.strongFont,panel.previewFont,panel.brandFont})if(font)DeleteObject(font);
    return 0;
}
