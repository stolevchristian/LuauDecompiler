-- Edge cases: precedence, upvalue chains, nested control flow, unusual literals.

local x, y = 3, 2
print(2 ^ -x, -x ^ 2, (-x) ^ 2, not x == y, not (x == y), #{1, 2} + 1, -(-x), x - -y)
print("a" .. "b" .. "c" .. x, (x .. y) .. "z", x .. (y .. "z"))
print(x * (y + 1), (x * y) + 1, x / y / 2, x / (y / 2), x - y - 1, x - (y - 1), x ^ y ^ 2, (x ^ y) ^ 2)
print(x > 2 and y > 1, x > 2 or y > 5, (x > 2) == true, x < y == false)
print(0x10, 1e3, 0.5e-3, 3.14159, 1 / 0, -1 / 0, 2 ^ 63, 123456789012)
print("multi\
line", [[long
string]], 'single "quoted"', "\65\066\x43", "\u{263A}", #"\0\0")

local function outer(a)
    local b = a * 2
    local function middle(c)
        local d = b + c
        return function(e)
            b = b + 1
            return a + b + c + d + e
        end
    end
    return middle
end
print(outer(1)(2)(3), outer(1)(2)(3))

local function sum(...)
    local total = 0
    for _, v in ipairs({...}) do
        total = total + v
    end
    return total, select("#", ...)
end
print(sum(), sum(1), sum(1, 2, 3), (sum(4, 5)))

local function search(rows, target)
    for i, row in ipairs(rows) do
        for j, v in ipairs(row) do
            if v == target then
                return i, j
            elseif v > target then
                break
            end
        end
    end
    return nil
end
print(search({{1, 5}, {2, 8}, {9}}, 8), search({{1}}, 7))

local function loops(n)
    local out = {}
    local i = 0
    while i < n do
        i = i + 1
        if i % 3 == 0 then
            continue
        elseif i > 7 then
            break
        end
        local j = i
        repeat
            j = j - 1
            if j == 2 then
                break
            end
        until j <= 0
        out[#out + 1] = i .. ":" .. j
    end
    return table.concat(out, " ")
end
print(loops(10))

local t = {}
t[1], t[2] = "first", "second"
t["k" .. 1] = "computed"
t[t[1]] = t[2]
local key = "dyn"
t[key] = #t
print(t[1], t[2], t.k1, t.first, t.dyn, t[key])

local function returnsNothing()
    return
end
local function returnsEarly(v)
    if v > 0 then
        return v
    elseif v < 0 then
        return -v
    end
end
print(returnsNothing(), returnsEarly(5), returnsEarly(-5), returnsEarly(0))

local function mixed(a, b, ...)
    return a, ..., b
end
print(mixed(1, 2, 3, 4))
print((mixed(1, 2, 3, 4)))

local cond = nil
if cond == nil then
    print("nil check")
end
if cond ~= nil then
    print("never")
else
    print("else branch")
end
local flag = true
if flag == true and x ~= 4 then
    print("bool const")
end
if x == 3 or y == 3 then
    print("num const")
end
if x == "s" then
    print("never")
elseif y ~= "s" then
    print("str const")
end
local z = x == 3 and "three" or "other"
local w = (x == 3) and (y == 2)
local v = x ~= 3 or y ~= 2
print(z, w, v, cond and 1, cond or 2, flag and cond, flag or cond)

local s = 0
for i = 1, 3 do
    for j = i, 3 do
        s = s + i * j
    end
end
print(s)

local nested = {a = {b = {c = {d = "deep"}}}}
print(nested.a.b.c.d, nested["a"]["b"].c["d"])
nested.a.b.c.d = "changed"
print(nested.a.b.c.d)

local calls = 0
local function side()
    calls = calls + 1
    return calls
end
local tbl = {side(), side(), n = side(), side()}
print(tbl[1], tbl[2], tbl[3], tbl.n, calls)
print(side() + side() * side())
local r1, r2 = side(), side()
print(r1, r2)

-- and/or value chains where the falsy operand is the result (nil vs false matters)
local function chains(p, q, r)
    local c1 = (p or q) and not r
    local c2 = (p and q) or r
    local c3 = p and q and r
    local c4 = p or q or r
    local c5 = (p or q) and (r or "d")
    return c1, c2, c3, c4, c5
end
print(chains(nil, false, 1))
print(chains(false, nil, nil))
print(chains(1, false, false))
print(chains(nil, 2, false))
