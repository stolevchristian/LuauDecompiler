-- general multiple assignment, rotations, assignment from a multi-return call into existing locals
local a, b, c = 1, 2, 3
a, b, c = c, a, b
print(a, b, c)
local function two() return 10, 20 end
a, b = two()
print(a, b)
local t = {}
t.x, t.y, a = b, a, 99
print(t.x, t.y, a)
a, b, c = b, c, a
print(a, b, c)
