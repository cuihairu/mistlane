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
function M.init(c) assert(c.role=='apiServer' and c.transport=='http'); print('HOOK init v1') end
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
// One process serving both Lua HTTP and the websocket lifecycle: /state and
// /echo stay with the HTTP service, everything else exercises the websocket
// contract (echo, push bounds, registry bookkeeping, lua failure handling).
inline constexpr std::string_view websocket_lua = R"fixture(
local open, closed = {}, {}
local M = {}
function M.websocket_open(id, path)
  open[id] = path
  print('WSHOOK open '..id..' '..path)
end
function M.websocket_message(id, payload)
  if payload == 'noreply' then return end -- no send: the bridge must still take the next message
  if payload == 'state' then
    local n, m = 0, 0
    for _ in pairs(open) do n = n + 1 end
    for _ in pairs(closed) do m = m + 1 end
    assert(mistlane.websocket.send(id, 'open='..n..' closed='..m..' id='..id))
  elseif payload:sub(1, 5) == 'flood' then
    -- N MiB against a reader that stopped reading: past the send bound and
    -- whatever the kernel buffers absorb. N rides on the payload because the
    -- test computes it from the host's socket buffer limits.
    local n = tonumber(payload:match('%d+')) or 16
    for _ = 1, n do assert(mistlane.websocket.send(id, string.rep('F', 1024*1024))) end
  elseif payload == 'oversize' then
    local ok, err = mistlane.websocket.send(id, string.rep('x', 1024*1024 + 1))
    assert(ok == false and err == 'payload exceeds 1 MiB', 'oversize guard missing')
    assert(mistlane.websocket.send(id, 'guarded'))
  elseif payload == 'unknown' then
    local ok, err = mistlane.websocket.send(99999, 'x')
    assert(ok == false and err == 'unknown websocket id', 'unknown id not reported')
    assert(mistlane.websocket.send(id, 'guarded'))
  elseif payload == 'boom' then
    error('message boom')
  else
    assert(mistlane.websocket.send(id, 'echo:'..payload))
  end
end
function M.websocket_close(id)
  closed[id] = true
  print('WSHOOK close '..id)
end
-- ws/wss listeners keep framed game sessions: the message callback is still
-- required whenever any listener is tcp/ws/wss.
function M.message(payload) return payload end
function M.http(r)
  if r.path == '/state' then
    local n, m = 0, 0
    for _ in pairs(open) do n = n + 1 end
    for _ in pairs(closed) do m = m + 1 end
    return {status = 200, body = 'open='..n..' closed='..m}
  end
  if r.path == '/echo' then return {status = 201, body = r.body} end
  return {status = 404, body = 'missing'}
end
return M
)fixture";
// Framed game sessions with a server-side identity: Lua reads the session id
// from the message context and pushes with mistlane.session.send/broadcast.
inline constexpr std::string_view push_lua = R"fixture(
local M = {}
function M.message(payload, context)
  if type(context) ~= 'table' then error('session context missing') end
  if type(context.session) ~= 'number' or context.session < 1 then
    error('session identity missing')
  end
  if payload == 'self' then
    return 'session='..context.session
  elseif payload == 'silent' then
    return nil
  elseif payload == 'push' then
    assert(mistlane.session.send(context.session, 'pushed'))
    return 'done'
  elseif payload:sub(1, 6) == 'burst ' then
    local n = tonumber(payload:match('%d+')) or 0
    local queued, refused = 0, 0
    for _ = 1, n do
      local ok, err = mistlane.session.send(context.session, string.rep('B', 64*1024))
      if ok then queued = queued + 1
      else
        refused = refused + 1
        assert(err == 'session backlogged', 'push refusal: '..tostring(err))
      end
    end
    local ok, err = mistlane.session.send(999999, 'x')
    assert(ok == false and err == 'unknown session id', 'unknown id: '..tostring(err))
    local big, big_err = mistlane.session.send(context.session, string.rep('x', 1024*1024+1))
    assert(big == false and big_err == 'payload exceeds 1 MiB', 'oversize: '..tostring(big_err))
    return 'burst='..queued..':'..refused
  elseif payload == 'bcast' then
    local sent = mistlane.session.broadcast('bc:'..context.session)
    return 'bcast='..sent
  end
  return payload
end
return M
)fixture";
// Optional request correlation (MLC2): Lua sees the request id from the
// message context and oversized correlated pushes report instead of failing.
inline constexpr std::string_view correlate_lua = R"fixture(
local M = {}
function M.message(payload, context)
  if type(context) ~= 'table' then error('session context missing') end
  if payload == 'self' then
    return 'session='..context.session..':req='..tostring(context.request)
  elseif payload == 'silent' then
    return nil
  elseif payload == 'push' then
    assert(mistlane.session.send(context.session, 'pushed'))
    return 'done'
  elseif payload == 'bcast' then
    return 'bcast='..mistlane.session.broadcast('bc')
  elseif payload == 'big' then
    local ok, err = mistlane.session.send(context.session, string.rep('x', 1024*1024-3))
    return 'big='..tostring(ok)..':'..tostring(err)
  elseif payload == 'oops' then
    error('mux oops')
  end
  return payload
end
return M
)fixture";
}
