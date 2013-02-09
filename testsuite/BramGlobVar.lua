Misc.CommonRTLGlobalScript = [=[
local preprocess = require "luapp" . preprocess
RTLGlobalCode, message = preprocess {input=RTLGlobalTemplate}
if message ~= nil then print(message) end

RTLGlobalCode = RTLGlobalCode .. FUs.CommonTemplate
]=]
