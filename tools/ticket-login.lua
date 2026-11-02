local service={}
function service.init() end
function service.message(p)
  local realm,char,inst,gen,exp = p:match('issue (%S+) (%S+) (%S+) (%S+) (%S+)')
  if not realm then return 'usage' end
  local ticket,err = mistlane.ticket.issue(realm,char,inst,tonumber(gen),tonumber(exp))
  if not ticket then return 'ERR '..err end
  return ticket
end
return service
