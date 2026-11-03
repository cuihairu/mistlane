-- Business scenario host: login, rooms, movement and heartbeats behind the
-- Player contract (examples/contracts/business.def). Pairs with
-- `mistlane bot --scenario business`, which spawns one entity per connection
-- ("spawn <entity-id>" text), then drives MLE1 login/joinRoom/move/heartbeat
-- requests. Room occupancy is published as the "rooms" script gauge so the
-- dashboard's Rooms panel tracks real business state.
local service = {}
local rooms, moves, beats = {}, 0, 0

local function room_total()
    local count = 0
    for _ in pairs(rooms) do count = count + 1 end
    return count
end

local function publish_rooms()
    mistlane.metrics.gauge("rooms", room_total())
end

function service.message(payload)
    -- The scenario bot seeds one live entity per connection before its first
    -- MLE1 frame; the entity id comes from the bot so envelope and route agree.
    local id = tonumber(string.match(payload, "^spawn (%d+)$"))
    if id then
        return mistlane.entity.spawn(1, 2, 1, id, 1) and "spawned" or "duplicate"
    end
    if payload == "rooms" then return tostring(room_total()) end
    return "echo:"..payload
end

service.entities = {
    Player = {
        login = function(args, ctx)
            assert(args.token and #args.token > 0, "empty token")
            assert(ctx.caller_kind == "server" and ctx.caller_id > 0)
            return { ok = 1 }
        end,
        joinRoom = function(args, ctx)
            rooms[args.room] = (rooms[args.room] or 0) + 1
            publish_rooms()
            return { ok = 1 }
        end,
        move = function(args, ctx)
            moves = moves + 1
            return { seq = moves }
        end,
        heartbeat = function(args, ctx)
            beats = beats + 1
            return { seq = beats }
        end,
        leaveRoom = function(args, ctx)
            if rooms[args.room] then
                rooms[args.room] = rooms[args.room] - 1
                if rooms[args.room] <= 0 then rooms[args.room] = nil end
            end
            publish_rooms()
            return { ok = 1 }
        end,
    },
}

return service
