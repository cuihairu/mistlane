local M = {}
local ready = false
function M.init(context)
  assert(context.transport == "http" or context.transport == "https")
end
function M.start() ready = true end
function M.before_request(req)
  assert(ready)
  -- Example only: use an actual authentication provider in an application.
  if req.path == "/private" and req.headers["authorization"] ~= "Bearer example" then
    return { status = 401, body = "Unauthorized" }
  end
end
function M.http(req)
  if req.method == "GET" and req.path == "/healthz" then
    return { status = 200, body = "ok" }
  elseif req.method == "POST" and req.path == "/echo" then
    return { status = 200, headers = { ["content-type"] = "application/octet-stream" }, body = req.body }
  end
  return { status = 404, body = "Not Found" }
end
function M.after_request(req, response)
  response.headers = response.headers or {}
  response.headers["x-service"] = "mistlane"
end
function M.on_error(req, error_message)
  -- Error detail stays in server logs, not in the response.
  return { status = 500, body = "Internal Server Error" }
end
function M.tick() end
function M.stop() ready = false end
function M.shutdown() end
return M
