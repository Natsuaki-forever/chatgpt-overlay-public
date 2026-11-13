#pragma once
#include "config.hpp"
#include <sddl.h>
#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

struct WinHandle {
    HANDLE value=nullptr;
    WinHandle()=default;
    explicit WinHandle(HANDLE value):value(value){}
    WinHandle(const WinHandle&)=delete;
    WinHandle& operator=(const WinHandle&)=delete;
    ~WinHandle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    void reset(HANDLE next=nullptr){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=next;}
};

inline HANDLE create_user_wake_event(const std::wstring& name){
    WinHandle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))throw std::runtime_error("wake_token_unavailable");
    DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
    if(!GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size))throw std::runtime_error("wake_owner_unavailable");
    LPWSTR sid=nullptr;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid))throw std::runtime_error("wake_sid_unavailable");
    std::wstring acl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error("wake_permissions_unavailable");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
    SetLastError(ERROR_SUCCESS);
    HANDLE event=CreateEventW(&security,FALSE,FALSE,name.c_str());DWORD error=GetLastError();LocalFree(descriptor);
    if(!event||error==ERROR_ALREADY_EXISTS){if(event)CloseHandle(event);throw std::runtime_error("wake_event_unavailable");}
    return event;
}

struct PipeServer {
    static constexpr UINT notification=WM_APP+50;
    static constexpr size_t messageLimit=1024*1024;
    struct Ticket {
        json request;
        DWORD clientPid=0;
        uint64_t connection=0;
        std::promise<json> response;
        WinHandle ready{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        Ticket(){if(!ready.value)throw std::runtime_error("Cannot create response event");}
    };
    HWND window;
    std::string nonce=new_id();
    std::wstring name=L"\\\\.\\pipe\\ChatGPTOverlay."+wide(new_id());
    std::atomic<DWORD> expectedPid;
    std::atomic<bool> finished{false};
    WinHandle pipe,stop,ioEvent;
    std::thread worker;
    std::mutex mutex;
    std::deque<std::shared_ptr<Ticket>> pending;
    explicit PipeServer(HWND window,DWORD pid):window(window),expectedPid(pid){
        WinHandle token;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))throw std::runtime_error("Cannot query pipe owner");
        DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
        std::vector<BYTE> storage(size);
        if(!GetTokenInformation(token.value,TokenUser,storage.data(),size,&size))throw std::runtime_error("Cannot query pipe owner SID");
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,&sid))throw std::runtime_error("Cannot encode pipe owner SID");
        std::wstring acl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))
            throw std::runtime_error("Cannot create pipe permissions");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
        pipe.value=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&security);
        LocalFree(descriptor);
        if(pipe.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create local session pipe");
        stop.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        ioEvent.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stop.value||!ioEvent.value)throw std::runtime_error("Cannot create pipe events");
        worker=std::thread([this](){
            try{run();}catch(const std::exception& error){std::ofstream(L"native-error.log",std::ios::app)<<"pipe: "<<error.what()<<'\n';}
            catch(...){std::ofstream(L"native-error.log",std::ios::app)<<"pipe: unknown failure\n";}
            finished=true;
        });
    }
    ~PipeServer(){
        if(stop.value)SetEvent(stop.value);
        if(worker.joinable())worker.join();
    }
    bool stopping()const{return WaitForSingleObject(stop.value,0)==WAIT_OBJECT_0;}
    DWORD operation(const std::function<BOOL(OVERLAPPED*,DWORD*)>& begin,DWORD& bytes){
        OVERLAPPED overlapped{};overlapped.hEvent=ioEvent.value;ResetEvent(ioEvent.value);
        if(stopping())return ERROR_OPERATION_ABORTED;
        if(begin(&overlapped,&bytes))return ERROR_SUCCESS;
        DWORD error=GetLastError();
        if(error==ERROR_MORE_DATA){GetOverlappedResult(pipe.value,&overlapped,&bytes,FALSE);return ERROR_MORE_DATA;}
        if(error!=ERROR_IO_PENDING)return error;
        HANDLE events[]={stop.value,ioEvent.value};
        DWORD result=WaitForMultipleObjects(2,events,FALSE,INFINITE);
        if(result!=WAIT_OBJECT_0+1){
            CancelIoEx(pipe.value,&overlapped);
            GetOverlappedResult(pipe.value,&overlapped,&bytes,TRUE);
            return result==WAIT_OBJECT_0?ERROR_OPERATION_ABORTED:ERROR_GEN_FAILURE;
        }
        return GetOverlappedResult(pipe.value,&overlapped,&bytes,FALSE)?ERROR_SUCCESS:GetLastError();
    }
    bool send(const json& response){
        std::string bytes=response.dump();
        if(bytes.size()>messageLimit)bytes=json({{"ok",false},{"error","response_too_large"},{"request_id",response.value("request_id",uint64_t(0))}}).dump();
        DWORD written=0;
        DWORD result=operation([&](OVERLAPPED* event,DWORD* count){return WriteFile(pipe.value,bytes.data(),DWORD(bytes.size()),count,event);},written);
        return result==ERROR_SUCCESS&&written==bytes.size();
    }
    bool receive(std::string& bytes){
        bytes.clear();
        for(;;){
            char chunk[65536];DWORD count=0;
            DWORD result=operation([&](OVERLAPPED* event,DWORD* read){return ReadFile(pipe.value,chunk,sizeof(chunk),read,event);},count);
            if(result!=ERROR_SUCCESS&&result!=ERROR_MORE_DATA)return false;
            if(bytes.size()+count>messageLimit){send({{"ok",false},{"error","message_too_large"},{"request_id",0}});return false;}
            bytes.append(chunk,count);
            if(result==ERROR_SUCCESS)return true;
        }
    }
    void run(){
        uint64_t connection=0;
        while(!stopping()){
            DWORD unused=0;
            DWORD result=operation([&](OVERLAPPED* event,DWORD*){return ConnectNamedPipe(pipe.value,event);},unused);
            if(stopping())break;
            if(result!=ERROR_SUCCESS&&result!=ERROR_PIPE_CONNECTED)throw std::runtime_error("Pipe accept failed");
            ULONG pid=0;DWORD ourSession=0,clientSession=0;
            bool valid=GetNamedPipeClientProcessId(pipe.value,&pid)&&pid==expectedPid.load()&&
                ProcessIdToSessionId(GetCurrentProcessId(),&ourSession)&&ProcessIdToSessionId(pid,&clientSession)&&ourSession==clientSession;
            ++connection;
            if(valid)while(!stopping()){
                std::string bytes;if(!receive(bytes))break;
                json request;
                try{request=json::parse(bytes);if(!request.is_object())throw std::runtime_error("object required");}
                catch(const std::exception&){if(!send({{"ok",false},{"error","invalid_message"},{"request_id",0}}))break;continue;}
                auto ticket=std::make_shared<Ticket>();ticket->request=std::move(request);ticket->clientPid=pid;ticket->connection=connection;
                auto future=ticket->response.get_future();
                {std::lock_guard<std::mutex> lock(mutex);pending.push_back(ticket);}
                if(!PostMessageW(window,notification,0,0))throw std::runtime_error("Cannot dispatch pipe request");
                HANDLE events[]={stop.value,ticket->ready.value};
                if(WaitForMultipleObjects(2,events,FALSE,INFINITE)!=WAIT_OBJECT_0+1)break;
                if(stopping())break;
                if(!send(future.get()))break;
            }
            DisconnectNamedPipe(pipe.value);
        }
    }
    void drain(const std::function<json(const Ticket&)>& dispatch){
        std::deque<std::shared_ptr<Ticket>> queue;
        {std::lock_guard<std::mutex> lock(mutex);queue.swap(pending);}
        for(auto& ticket:queue){
            json response;
            try{response=dispatch(*ticket);}
            catch(const std::exception& error){response={{"ok",false},{"error",error.what()},{"request_id",0}};}
            ticket->response.set_value(std::move(response));SetEvent(ticket->ready.value);
        }
    }
};
