-- Only demonstrates the framework lifecycle; no game protocol or data.
local service = {}
function service.init() end
function service.message(payload) return payload end
function service.tick() end
function service.shutdown() end
return service
