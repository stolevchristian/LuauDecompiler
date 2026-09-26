local x = 10
local y, z = 20, "s"
local t = {1, 2, x = 3, [y] = z}
local function f(a, b)
    return a + b * 2 - x
end
t.g = function(self, n) return self.x + n end
print(f(x, y), t:g(5), #t, t[1], -x, not y, x .. z)
local v = x > 5 and "big" or "small"
print(v, x == 10, y ~= 3)
