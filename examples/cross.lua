-- Research example: context is routing metadata, not authenticated identity.
return {
  init = function() end,
  message = function(payload, context)
    return context.group .. "/" .. context.room .. ":" .. payload
  end,
  tick = function() end,
  shutdown = function() end,
}
