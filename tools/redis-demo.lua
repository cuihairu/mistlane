local service={}
function service.init() end
function service.message(p)
  if p=='probe' then return 'stream API: '..tostring(mistlane.stream~=nil) end
  mistlane.stream.publish('mail',{kind='reward',player='p-17'},function(err,id)
    if err then print('PUBLISH_RESULT ERR '..err)
    else print('PUBLISH_RESULT OK '..id) end
  end)
  return 'publishing'
end
return service
