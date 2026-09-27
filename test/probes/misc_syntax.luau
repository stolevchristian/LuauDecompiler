-- compound assignment, single-quoted and escaped strings, methods never called with colon syntax, integer-like keys
local s = 'it\'s "quoted"'
s ..= '\x41\65\u{263A}\n'
local obj = {n = 0}
function obj.bump(self, by)
    self.n += by or 1
    return self.n
end
obj.bump(obj, 2)
obj.bump(obj)
local m = {[1] = "one", [2.5] = "frac", [-1] = "neg", ["2"] = "str"}
local x = 7
x //= 2
x ^= 3
x %= 5
print(#s, obj.n, m[1], m[2.5], m[-1], m["2"], x)
