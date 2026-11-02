local service={}
function service.init() end
function service.message(p)
  mistlane.place('gameServer','-','main',function(err,addr)
    if err then print('PLACE_RESULT ERR '..err)
    else print('PLACE_RESULT OK '..addr) end
  end)
  return 'asking'
end
return service
