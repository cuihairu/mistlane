local service={}
function service.init() end
function service.message(p)
  return 'entity runtime: '..tostring(mistlane.entity~=nil)
end
return service
