// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_contract.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <variant>
#include <vector>

namespace mistlane {
enum class EntityError { INVALID, UNROUTED, STALE, FORBIDDEN, TIMEOUT, CANCELLED,
                         NO_IMPLEMENTATION, NOT_READY, CAPACITY, CONFLICT };
struct EntityCaller { std::string kind; std::uint64_t id = 0; };
struct EntityTarget { std::uint32_t scope = 0, type_id = 0, incarnation = 0; std::int64_t entity_id = 0; };
struct EntityValue;
using EntityArray = std::vector<EntityValue>;
using EntityObject = std::map<std::string, EntityValue>;
struct EntityValue {
 std::variant<bool, std::int64_t, std::string, EntityArray, EntityObject> data;
 EntityValue(bool v) : data(v) {}
 EntityValue(std::int64_t v) : data(v) {}
 EntityValue(std::string v) : data(std::move(v)) {}
 EntityValue(const char* v) : data(std::string(v)) {}
 EntityValue(EntityArray v) : data(std::move(v)) {}
 EntityValue(EntityObject v) : data(std::move(v)) {}
};
using EntityValues = std::map<std::string, EntityValue>;
struct EntityInvocation { EntityTarget target; EntityCaller caller; std::string side; unsigned method_id = 0; bool encoded_ok = true; EntityValues arguments; std::uint32_t hosting_generation = 0; };
struct EntityDispatchResult { EntityError error = EntityError::INVALID; EntityValues response; bool ok = false; };

// A deterministic in-process descriptor dispatcher. The transport sproto codec
// decodes to EntityValues before this authorization/dispatch boundary.
class EntityDispatcher {
public:
 using Handler = std::function<EntityValues(const EntityCaller&, const EntityTarget&, const EntityValues&)>;
 explicit EntityDispatcher(const EntityContract& contract) : contract_(contract) {}
 void bind(std::string entity, std::string side, unsigned method, Handler handler) {
   handlers_[key(entity, side, method)] = std::move(handler);
 }
 EntityDispatchResult dispatch(const EntityInvocation& call) const {
   if (!call.encoded_ok) return fail(EntityError::INVALID);
   const auto entity = std::find_if(contract_.entities().begin(), contract_.entities().end(), [&](const auto& e){return e.id==call.target.type_id;});
   if (entity == contract_.entities().end()) return fail(EntityError::INVALID);
   if (call.target.scope == 0 || call.target.entity_id == 0 || call.target.incarnation == 0 || (call.side!="server"&&call.side!="client")) return fail(EntityError::INVALID);
   const auto& methods = call.side=="server" ? entity->server_methods : entity->client_methods;
   const auto method=std::find_if(methods.begin(),methods.end(),[&](const auto& m){return m.id==call.method_id;});
   if(method==methods.end()) return fail(EntityError::INVALID);
   if ((method->caller=="owner" && call.caller.kind!="owner") ||
       (method->caller=="server" && call.caller.kind!="server") ||
       (method->caller=="client" && call.caller.kind!="client") || call.caller.id==0) return fail(EntityError::FORBIDDEN);
   const auto hit=handlers_.find(key(entity->name,call.side,method->id));
   if(hit==handlers_.end()) return fail(EntityError::NO_IMPLEMENTATION);
   if(!valid(method->args,call.arguments,contract_.structs())) return fail(EntityError::INVALID);
   EntityDispatchResult result; result.response=hit->second(call.caller,call.target,call.arguments);
   if ((method->mode == "notify" && !result.response.empty()) ||
       (method->mode == "request" && !valid(method->returns,result.response,contract_.structs()))) {
     return fail(EntityError::INVALID);
   }
   result.ok=true; return result;
 }
private:
 static EntityDispatchResult fail(EntityError error){EntityDispatchResult r;r.error=error;return r;}
 static std::string key(const std::string& e,const std::string& s,unsigned id){return e+"/"+s+"/"+std::to_string(id);}
public:
 // One type rule for every surface: wire arguments, responses and controlled
 // property writes all share this validation.
 static bool value_valid(const std::string& type,const std::string& of,const EntityValue& value,
                         const std::map<std::string,ContractArgs>& structs){
   if(type=="ARRAY"){
     const auto* items=std::get_if<EntityArray>(&value.data);if(!items)return false;
     for(const auto& item:*items) if(!value_valid(of,{},item,structs)) return false;
     return true;
   }
   if(type=="BOOL")return std::holds_alternative<bool>(value.data);
   if(type=="INT32"||type=="UINT16"||type=="UINT32"||type=="INT64"||type=="EntityRef"){
     const auto* n=std::get_if<std::int64_t>(&value.data);if(!n)return false;
     if(type=="INT32")return *n>=std::numeric_limits<std::int32_t>::min()&&*n<=std::numeric_limits<std::int32_t>::max();
     if(type=="UINT16")return *n>=0&&*n<=std::numeric_limits<std::uint16_t>::max();
     if(type=="UINT32")return *n>=0&&static_cast<std::uint64_t>(*n)<=std::numeric_limits<std::uint32_t>::max();
     return true;
   }
   if(type=="STRING"||type=="BYTES")return std::holds_alternative<std::string>(value.data);
   const auto spec=structs.find(type);const auto* object=std::get_if<EntityObject>(&value.data);
   if(spec==structs.end()||!object||object->size()!=spec->second.size())return false;
   for(const auto& f:spec->second){auto i=object->find(f.name);if(i==object->end()||!value_valid(f.type,f.of,i->second,structs))return false;}return true;
 }
private:
 static bool valid(const ContractArgs& schema,const EntityValues& values,const std::map<std::string,ContractArgs>& structs){
   if(values.size()!=schema.size()) return false;
   for(const auto& f:schema){auto i=values.find(f.name);if(i==values.end())return false;
     if(!value_valid(f.type,f.of,i->second,structs))return false;
   } return true;
 }
 const EntityContract& contract_; std::map<std::string,Handler> handlers_;
};
}
