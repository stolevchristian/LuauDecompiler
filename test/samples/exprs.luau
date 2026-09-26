local a, b = f(), g()
local c = a and b
local d = a or b
local e = a == b
local q = a < b
local r = not a
local s = {f()}
local u = {1, f()}
a, b = b, a
a.x.y = b
local w = a and b or c
if a == b and c then h() end
h(a and b)
return a, f()
