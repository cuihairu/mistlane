-- SPDX-License-Identifier: Apache-2.0
-- Start an echo worker/gateway before running:
-- mistlane test --script examples/test_echo.lua
local client
return {
  name = "game",
  setup = function(t)
    client = t.connect {
      url = os.getenv("MISTLANE_TEST_URL") or "tcp://127.0.0.1:7100",
      ca_file = os.getenv("MISTLANE_TEST_CA") or "",
      timeout_ms = 2000,
    }
  end,
  teardown = function()
    if client then client:close() end
  end,
  cases = {
    echo = function(t)
      t.assert_equal(client:request("hello mistlane"), "hello mistlane")
    end,
    binary = function(t)
      local payload = string.char(0, 255) .. "game payload"
      t.assert_equal(client:request(payload), payload)
    end,
    empty = function(t)
      t.assert_equal(client:request(""), "")
    end,
  },
}
