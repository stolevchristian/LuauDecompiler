local a, b, c = f(), g(), h()
local e1 = a < b and c
local e2 = (a and b) == c
local e3 = not (a and b)
local e4 = a < b or c > 2
local w = a and b or c
local w2 = a and f() or g()
print(e1, e2, e3, e4, w, w2)
f(a == b, a ~= nil, not a)
t[a ~= b] = 1
local x = a and b
local y = a or b
x = a and f()
y = a or f()
if a then
    print(1)
end
if not a then
    print(2)
elseif b and c then
    print(3)
end
while a and b do
    a = f()
    if b then
        continue
    end
    b = g()
end
while true do
    if a then
        break
    end
    a = f()
end
local i = 0
repeat
    i = i + 1
    if i == 5 then
        break
    end
until i > 10 or c
for k, v in next, t do
    print(k, v)
end
for j = 10, 1, -1 do
    print(j)
end
for _, v in ipairs(t) do
    if v > 1 then
        print(v)
    else
        print(-v)
    end
end
local function fact(n)
    if n <= 1 then
        return 1
    end
    return n * fact(n - 1)
end
local counter = 0
local function inc()
    counter = counter + 1
    return counter
end
print(fact(5), inc(), inc(), select("#", ...))
return a < b
