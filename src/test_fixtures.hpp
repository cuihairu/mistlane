// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>
namespace mistlane::test {
inline constexpr std::string_view redis_lua = R"fixture(
local state = { stream = "pending", board = "pending" }
return {
  message = function(payload)
    if payload == "setup" then
      mistlane.stream.create_group("it", "workers", function(err)
        if err then state.stream = "error:" .. err; return end
        mistlane.stream.create_group("it", "workers", function(group_err)
          if group_err then state.stream = "error:" .. group_err; return end
          mistlane.stream.publish("it", {kind = "integration", value = "ok"}, function(pub_err, id)
            if pub_err then state.stream = "error:" .. pub_err else state.published = id end
          end)
        end)
      end)
      return "started"
    elseif payload == "read" then
      mistlane.stream.read("it", "workers", "worker-a", 10, function(err, entries)
        if err then state.stream = "error:" .. err; return end
        for _, entry in ipairs(entries) do
          if entry.fields.kind == "integration" then
            state.stream = entry.fields.value
            state.pending_id = entry.id
          end
        end
      end)
      return "reading"
    elseif payload == "claim" then
      mistlane.stream.claim("it", "workers", "worker-b", 0, "0-0", 10, function(err, result)
        if err then state.stream = "error:" .. err; return end
        if #result.entries == 0 then state.stream = "claim-empty"; return end
        mistlane.stream.ack("it", "workers", result.entries[1].id, function(ack_err, count)
          if ack_err then state.stream = "error:" .. ack_err else state.acked = count end
        end)
      end)
      return "claiming"
    elseif payload == "rank" then
      mistlane.leaderboard.add("season", "player-a", 7, function(err, added)
        if err then state.board = "error:" .. err; return end
        mistlane.leaderboard.get("season", "player-a", function(get_err, current)
          if get_err then state.board = "error:" .. get_err; return end
          mistlane.leaderboard.top("season", 0, 10, function(top_err, rows)
            if top_err then state.board = "error:" .. top_err; return end
            state.board = added.score .. ":" .. added.rank .. ":" .. current.score .. ":" .. #rows
          end)
        end)
      end)
      return "ranking"
    elseif payload == "remove" then
      mistlane.leaderboard.remove("season", "player-a", function(err, count)
        state.removed = err and ("error:" .. err) or tostring(count)
      end)
      return "removing"
    elseif payload == "pend" then
      mistlane.stream.read("it", "workers", "worker-a", 10, function() end)
      return "pending"
    elseif payload == "reconnect" then
      mistlane.stream.publish("reconnect-probe", {kind = "probe"}, function(err, id)
        state.reconnected = err and ("error:" .. err) or id
      end)
      return "probing"
    elseif payload == "probe" then
      return state.reconnected or ""
    elseif payload == "status" then
      return (state.stream or "") .. "|" .. (state.board or "") .. "|" ..
        (state.published or "") .. "|" .. tostring(state.acked or "") .. "|" .. (state.removed or "")
    end
    return payload
  end
}
)fixture";
inline constexpr std::string_view http_lua = R"fixture(
local M = {}
function M.init(c) assert(c.role=='worker' and c.transport=='http'); print('HOOK init v1') end
function M.start() print('HOOK start v1') end
function M.before_request(r)
  r.verified = true
  if r.path=='/private' then return {status=401,body='denied'} end
end
function M.http(r)
  assert(r.verified)
  if r.path=='/echo' then
    assert(r.method=='POST' and r.query=='x=1' and r.headers['x-test']=='yes')
    return {status=201,body=r.body}
  end
  if r.path=='/error' or r.path=='/fatal' then error('private detail') end
  if r.path=='/badheader' then return {headers={['bad header']='x'}} end
  return {status=404,body='missing'}
end
function M.after_request(r,res)
  res.headers = {['x-hook']='after'}
  if r.path=='/replace' then return {status=202,body='replaced'} end
end
function M.on_error(r,e)
  if r.path=='/fatal' then error('error hook failed') end
  return {status=502,body='handled'}
end
function M.tick() end
function M.stop() print('HOOK stop v1') end
function M.shutdown() print('HOOK shutdown v1') end
return M
)fixture";
}
