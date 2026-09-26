-- Runnable feature coverage script: every construct prints deterministic output so
-- the decompiled version can be executed and compared against the original.

local function add(a, b)
    return a + b
end

local function multi()
    return 1, 2, 3
end

local function varargs(...)
    local n = select("#", ...)
    local first = ...
    return n, first, ...
end

local counter = 0
local function bump()
    counter = counter + 1
    return counter
end

function globalFn(x)
    return x * 2
end

local obj = {value = 10, name = "obj"}
function obj.static(x)
    return x + 1
end
function obj:method(x)
    return self.value + x
end

local t = {1, 2, 3, x = "a", ["key with space"] = true, [10] = "ten", nested = {a = {b = 1}}}
t.y = t.x .. "b"
t[#t + 1] = 4
t.nested.a.c = 2

print(add(1, 2), multi())
print(varargs("a", "b"))
print(bump(), bump(), counter)
print(globalFn(21), obj.static(1), obj:method(5), obj.name)
print(#t, t[4], t.y, t["key with space"], t[10], t.nested.a.b, t.nested.a.c)

local a, b, c = multi()
print(a, b, c)
local d, e = multi(), 5
print(d, e)
a, b = b, a
print(a, b)

local x = 7
print(x + 1, x - 1, x * 2, x / 2, x // 2, x % 3, x ^ 2, -x, 2 - x, 20 / x, 1 + x, "s" .. x .. "e")
print(x == 7, x ~= 7, x < 8, x <= 7, x > 6, x >= 7, not x, #"abc")
print(x > 5 and "big" or "small", nil or "default", false and 1, x and nil)
print(1e100, 0.1, -0.5, 1 / 3, 2 ^ 53, 100000000000000, 1e15, -0)
print("tab\tnewline\nquote\"backslash\\", "\0nul", "\255high")

local s = 0
for i = 1, 10 do
    if i % 2 == 0 then
        continue
    end
    s = s + i
end
print("odd sum", s)

for i = 10, 1, -3 do
    print("down", i)
end

for k, v in pairs({z = 1}) do
    print(k, v)
end
for i, v in ipairs({"p", "q"}) do
    print(i, v)
end
for k, v in next, {only = 1} do
    print(k, v)
end

local n = 0
while n < 5 do
    n = n + 1
    if n == 3 then
        break
    end
end
print("while", n)

repeat
    n = n + 10
until n > 30 or n == 13
print("repeat", n)

local w = 0
while true do
    w = w + 1
    if w > 3 then
        break
    end
end
print("forever", w)

local function classify(v)
    if v == nil then
        return "nil"
    elseif type(v) == "number" and v > 100 then
        return "large"
    elseif type(v) == "number" then
        return "number"
    elseif v == true or v == false then
        return "boolean"
    else
        return "other"
    end
end
print(classify(nil), classify(200), classify(5), classify(true), classify("s"))

local function nested(list)
    local total = 0
    for _, row in ipairs(list) do
        for _, v in ipairs(row) do
            if v < 0 then
                continue
            end
            if v > 100 then
                break
            end
            total = total + v
        end
    end
    return total
end
print(nested({{1, 2, -3}, {4, 500, 6}, {7}}))

local function makeCounter(start)
    local value = start
    return function(step)
        value = value + (step or 1)
        return value
    end, function()
        return value
    end
end
local inc, get = makeCounter(100)
inc()
inc(5)
print(get())

local function early(v)
    if v then
        return "yes"
    end
    print("no path")
    return "no"
end
print(early(true), early(false))

local function fact(k)
    if k <= 1 then
        return 1
    end
    return k * fact(k - 1)
end
print(fact(6))

local grid = {}
for i = 1, 3 do
    grid[i] = {}
    for j = 1, 3 do
        grid[i][j] = i * j
    end
end
print(grid[2][3], #grid, #grid[1])

local str = "hello"
print(str:upper(), str:sub(2, 3), ("x"):rep(3), string.len(str), math.max(1, 5, 3), math.abs(-4))
print(tostring(nil), tonumber("42"), type(print))

local ok, err = pcall(function()
    error("boom")
end)
print(ok, err ~= nil)

local big = {}
for i = 1, 20 do
    big[i] = i * i
end
print(#big, big[20], table.concat(big, ",", 1, 5))

print(select("#"), select("#", 1, nil), (multi()))
do
    local scoped = "inner"
    print(scoped)
end
print(add(add(1, 2), add(3, 4)), (add(1, 2)))
