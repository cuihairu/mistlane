-- Minimal contract-hosting script for the tools/bots example plan and
-- injection runs: spawns entities on request and answers the four scenario
-- methods. Start with:
--   mistlane --role gameServer --listen 127.0.0.1:7100 \
--     --script tools/bots/example-business.lua \
--     --contract tools/bots/example-business.def --service-token s3cret
local service = {}
function service.message(payload)
    local id = tonumber(string.match(payload, "^spawn (%d+)$"))
    if id then return mistlane.entity.spawn(1, 2, 1, id, 1) and "spawned" or "duplicate" end
    return "echo:"..payload
end
service.entities = {
    Player = {
        login = function(args, ctx) assert(#args.token > 0) return { ok = 1 } end,
        joinRoom = function(args, ctx) return { ok = 1 } end,
        move = function(args, ctx) return { seq = args.x } end,
        heartbeat = function(args, ctx) return { seq = 0 } end,
    },
}
return service
