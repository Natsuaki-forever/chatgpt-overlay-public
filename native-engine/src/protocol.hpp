#pragma once
#include "pipe.hpp"

struct SessionProtocol {
    ConfigState& state;
    std::string token;
    uint64_t connection=0,lastId=0;
    json lastRequest,lastResponse;
    bool visualWrites=false;
    bool visualPreviewWrites=false;
    std::function<json()> extraState=[](){return json::object();};
    std::function<json(const std::string&,const json&,DWORD)> extension;
    SessionProtocol(ConfigState& state,const std::string& token):state(state),token(token){}
    json snapshot()const{
        json result={{"regions",state.document["regions"]},{"selected_id",state.selected.empty()?json(nullptr):json(state.selected)},
            {"effective_positions",state.effective_positions()},
            {"effective_rects",state.effective_rects()},{"tracking_states",state.tracking_states()},
            {"visual_check_interval_seconds",state.visual_check_interval()},
            {"revision",state.revision},{"persisted_revision",state.persistedRevision},{"state_sequence",state.stateSequence}};
        result.update(extraState());return result;
    }
    json dispatch(const PipeServer::Ticket& ticket){
        const json& request=ticket.request;uint64_t id=0;bool authorized=false,attemptedSave=false;
        try{
            if(!request.contains("request_id")||!request["request_id"].is_number_unsigned()||request["request_id"]==0)
                throw std::runtime_error("invalid_request_id");
            id=request["request_id"].get<uint64_t>();
            if(!request.contains("protocol")||!request["protocol"].is_number_integer()||request["protocol"]!=1)
                throw std::runtime_error("unsupported_protocol");
            if(request.value("session",std::string())!=token)throw std::runtime_error("invalid_session");
            authorized=ticket.connection==connection&&connection!=0;
            std::string op=request.at("op").get<std::string>();
            json payload=request.value("payload",json::object());if(!payload.is_object())throw std::runtime_error("invalid_payload");
            if(ticket.connection==connection&&id==lastId){
                if(request==lastRequest)return lastResponse;
                throw std::runtime_error("request_id_reused");
            }
            if(op=="hello"){
                if(ticket.connection==connection)throw std::runtime_error("already_authenticated");
                bool visual=std::any_of(state.document["regions"].begin(),state.document["regions"].end(),[](const auto& r){return r["position_mode"]=="visual";});
                if(visual&&(!payload.contains("capabilities")||!payload["capabilities"].is_array()||
                    std::find(payload["capabilities"].begin(),payload["capabilities"].end(),"visual_geometry_v1")==payload["capabilities"].end()))throw std::runtime_error("component_mismatch");
                if(extension)extension(op,payload,ticket.clientPid);
                connection=ticket.connection;lastId=0;
                visualWrites=payload.contains("capabilities")&&payload["capabilities"].is_array()&&
                    std::find(payload["capabilities"].begin(),payload["capabilities"].end(),"visual_tracking_v1")!=payload["capabilities"].end();
                visualPreviewWrites=payload.contains("capabilities")&&payload["capabilities"].is_array()&&
                    std::find(payload["capabilities"].begin(),payload["capabilities"].end(),"visual_calibration_preview_v1")!=payload["capabilities"].end();
                authorized=true;
            }else if(ticket.connection!=connection||!connection)throw std::runtime_error("handshake_required");
            if(id<=lastId)throw std::runtime_error("stale_request_id");
            if(!state.persistent&&op!="hello"&&op!="snapshot"&&op!="select"&&op!="commit_close"&&
                op!="list_applications"&&op!="switch_application")throw std::runtime_error("application_required");
            json result=json::object();bool changed=false;
            if(op=="hello"){}
            else if(op=="snapshot"){
                if(payload.value("state_sequence",uint64_t(-1))==state.stateSequence){
                    result["unchanged"]=true;
                }
            }else if(op=="select"){
                std::string selected=payload.at("id").is_null()?std::string():payload["id"].get<std::string>();
                if(!selected.empty()&&state.index(selected)<0)throw std::runtime_error("region_not_found");
                if(selected!=state.selected){state.selected=selected;state.stateSequence++;}
                if(extension)extension(op,payload,ticket.clientPid);
            }else if(op=="update"){
                auto coordinateSpace=payload.value("coordinate_space",std::string("stored"));
                if(coordinateSpace=="client")state.update_positioned(payload.at("id").get<std::string>(),payload.at("values"),request.at("base_revision").get<uint64_t>());
                else if(coordinateSpace=="stored")state.update(payload.at("id").get<std::string>(),payload.at("values"),request.at("base_revision").get<uint64_t>());
                else throw std::runtime_error("invalid_coordinate_space");
                changed=true;
            }else if(op=="create"){
                result["created_id"]=state.create(payload,request.at("base_revision").get<uint64_t>());changed=true;
            }else if(op=="delete"){
                state.erase(payload.at("id").get<std::string>(),request.at("base_revision").get<uint64_t>());changed=true;
            }else if(op=="clear_regions"){
                auto candidate=state;
                candidate.erase_all(request.at("base_revision").get<uint64_t>());
                attemptedSave=true;candidate.save();attemptedSave=false;
                state=std::move(candidate);changed=true;
            }else if(op=="commit_close"){
                state.expect(request.at("base_revision").get<uint64_t>());attemptedSave=true;state.save();attemptedSave=false;
                if(extension)result.update(extension(op,payload,ticket.clientPid));
            }else if(op=="visual_calibrate"||op=="visual_retry"||op=="visual_anchor"||op=="begin_calibration"||op=="recalibrate_reference"||
                     op=="set_calibration_reference"||op=="update_calibration_preview"||op=="confirm_calibration"||op=="begin_cover_drag"||
                     op=="cancel_calibration"||op=="pick_calibration_reference"||op=="update_cover_relation"||op=="retry_calibration"||op=="set_position_mode"||op=="set_visual_search_limit"||op=="set_visual_check_interval"||op=="refresh_visual"){
                if(!visualWrites)throw std::runtime_error("component_mismatch");
                if(op!="visual_calibrate"&&op!="visual_retry"&&op!="visual_anchor"&&!visualPreviewWrites)
                    throw std::runtime_error("component_mismatch");
                state.expect(request.at("base_revision").get<uint64_t>());
                if(!extension)throw std::runtime_error("operation_unavailable");
                result.update(extension(op,payload,ticket.clientPid));
            }else if(op=="list_applications"||op=="switch_application"||op=="set_region_scope"){
                if(!extension)throw std::runtime_error("operation_unavailable");
                if(op!="list_applications")state.expect(request.at("base_revision").get<uint64_t>());
                result.update(extension(op,payload,ticket.clientPid));
            }else if(op=="select_target"||op=="pick_color"||op=="set_canvas_mode"){
                if(!extension)throw std::runtime_error("operation_unavailable");
                result.update(extension(op,payload,ticket.clientPid));
            }else throw std::runtime_error("unknown_operation");
            if(changed&&extension)extension("changed",payload,ticket.clientPid);
            if(op!="list_applications"&&!result.value("unchanged",false))result["state"]=snapshot();
            result.update({{"ok",true},{"request_id",id},{"revision",state.revision},{"persisted_revision",state.persistedRevision}});
            lastId=id;lastRequest=request;lastResponse=result;return result;
        }catch(const std::exception& error){
            json result={{"ok",false},{"request_id",id},{"error",error.what()}};
            if(attemptedSave)result.update({{"error","save_failed"},{"detail",error.what()},{"retryable",true},{"retry_requires_new_request_id",true}});
            if(authorized)result.update({{"revision",state.revision},{"persisted_revision",state.persistedRevision},{"state",snapshot()}});
            if(ticket.connection==connection&&id>lastId){lastId=id;lastRequest=request;lastResponse=result;}
            return result;
        }
    }
};
