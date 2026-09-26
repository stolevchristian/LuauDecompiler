--!optimize 2
-- Stress test: every construct prints deterministic output so the round trip is meaningful.

local out = {}
local function emit(...)
    local parts = {}
    for i = 1, select("#", ...) do
        parts[i] = tostring((select(i, ...)))
    end
    out[#out + 1] = table.concat(parts, " ")
end

-- 1. Class hierarchy through metatables, operator overloads, __tostring, __call, __index functions
local Vec = {}
Vec.__index = Vec
function Vec.new(x, y)
    return setmetatable({x = x, y = y}, Vec)
end
function Vec.__add(a, b)
    return Vec.new(a.x + b.x, a.y + b.y)
end
function Vec.__mul(a, k)
    if type(a) == "number" then
        a, k = k, a
    end
    return Vec.new(a.x * k, a.y * k)
end
function Vec.__eq(a, b)
    return a.x == b.x and a.y == b.y
end
function Vec.__lt(a, b)
    return a:len2() < b:len2()
end
function Vec.__tostring(v)
    return string.format("(%g, %g)", v.x, v.y)
end
function Vec.__unm(v)
    return Vec.new(-v.x, -v.y)
end
function Vec:len2()
    return self.x * self.x + self.y * self.y
end
function Vec:dot(o)
    return self.x * o.x + self.y * o.y
end

local Base = {}
Base.__index = Base
function Base.new(name)
    local self = setmetatable({}, Base)
    self.name = name
    self.tags = {}
    return self
end
function Base:describe()
    return self.name .. "[" .. #self.tags .. "]"
end
function Base:tag(t)
    self.tags[#self.tags + 1] = t
    return self
end

local Derived = setmetatable({}, {__index = Base})
Derived.__index = Derived
function Derived.new(name, level)
    local self = Base.new(name)
    setmetatable(self, Derived)
    self.level = level
    return self
end
function Derived:describe()
    return Base.describe(self) .. "@" .. self.level
end

local callable = setmetatable({count = 0}, {
    __call = function(self, inc)
        self.count += inc or 1
        return self.count
    end,
    __index = function(_, key)
        return "missing:" .. tostring(key)
    end,
    __newindex = function(t, key, value)
        rawset(t, key, value * 2)
    end,
})

local a, b = Vec.new(1, 2), Vec.new(3, 4)
emit(tostring(a + b), tostring(a * 2), tostring(3 * b), tostring(-a), a == Vec.new(1, 2), a < b, a:dot(b))
local d = Derived.new("obj", 3):tag("x"):tag("y")
emit(d:describe(), Base.describe(d), d.level, callable(), callable(5), callable.nothing)
callable.value = 21
emit(rawget(callable, "value"), callable.count)

-- 2. Memoized recursion, mutual recursion through upvalues, closures in loops
local memo = {}
local function fib(n)
    if n < 2 then
        return n
    end
    local cached = memo[n]
    if cached then
        return cached
    end
    local value = fib(n - 1) + fib(n - 2)
    memo[n] = value
    return value
end

local isEven, isOdd
function isEven(n)
    return n == 0 or isOdd(n - 1)
end
function isOdd(n)
    return n ~= 0 and isEven(n - 1)
end

local adders = {}
for i = 1, 3 do
    local offset = i * 10
    adders[i] = function(x)
        offset += 1
        return x + i + offset
    end
end
emit(fib(30), isEven(10), isOdd(7), adders[1](0), adders[1](0), adders[3](100))

-- 3. Varargs plumbing, select, table.pack/unpack, multiple returns and truncation
local function pack2(...)
    return {n = select("#", ...), ...}
end
local function spread(t)
    return table.unpack(t, 1, t.n)
end
local function first(...)
    return (...)
end
local function tail(_, ...)
    return ...
end
local function count(...)
    return select("#", ...)
end
local p = pack2(1, nil, 3, nil)
emit(p.n, #p, first(spread(p)), count(tail(spread(p))), count(spread(p)), (spread(p)))
local function minmax(...)
    local lo, hi = math.huge, -math.huge
    for _, v in ipairs({...}) do
        lo = if v < lo then v else lo
        hi = v > hi and v or hi
    end
    return lo, hi
end
local lo, hi = minmax(5, -2, 9, 3)
local x, y, z = minmax(1)
emit(lo, hi, x, y, z, select(2, minmax(4, 8)), select(-1, minmax(4, 8)))

-- 4. Custom iterators, stateful and stateless, closures over generic for, sorted pairs
local function range(from, to, step)
    step = step or 1
    local i = from - step
    return function()
        i += step
        if (step > 0 and i <= to) or (step < 0 and i >= to) then
            return i
        end
        return nil
    end
end
local function sortedPairs(t)
    local keys = {}
    for k in pairs(t) do
        keys[#keys + 1] = k
    end
    table.sort(keys, function(l, r)
        return tostring(l) < tostring(r)
    end)
    local idx = 0
    return function()
        idx += 1
        local k = keys[idx]
        if k ~= nil then
            return k, t[k]
        end
        return nil
    end
end
local function iter(state, control)
    if control < state then
        return control + 1, control * control
    end
end
local acc = {}
for v in range(1, 10, 3) do
    acc[#acc + 1] = v
end
for v in range(5, 1, -2) do
    acc[#acc + 1] = v
end
for i, sq in iter, 4, 0 do
    acc[#acc + 1] = i .. ":" .. sq
end
for k, v in sortedPairs({zeta = 1, alpha = 2, [3] = "three", mid = false}) do
    acc[#acc + 1] = tostring(k) .. "=" .. tostring(v)
end
emit(table.concat(acc, ","))

-- 5. String handling: patterns with callbacks, formatting, escapes, methods on literals
local text = "The quick brown fox jumps over 13 lazy dogs and 7 cats"
local words, numbers = 0, {}
for w in text:gmatch("%a+") do
    words += 1
end
for n in string.gmatch(text, "%d+") do
    numbers[#numbers + 1] = tonumber(n)
end
local swapped = text:gsub("(%a+) (%a+)", "%2 %1", 2)
local shouted = text:gsub("%a+", function(w)
    return #w > 4 and w:upper() or nil
end)
emit(words, table.concat(numbers, "+"), swapped)
emit(shouted)
emit(("%5.2f|%-6s|%03d|%x|%q"):format(math.pi, "ab", 7, 255, 'q"t'), ("abc"):rep(2, "-"), #"\n\t\\", ("x"):byte(), string.char(72, 105))
emit(text:sub(-4), text:sub(5, 9), text:find("fox"), text:match("(%d+) lazy"), text:len() == #text, ("A"):lower() .. ("b"):upper())

-- 6. Control flow torture: nested loops with break/continue, repeat with continue, early returns, while-true state machine
local function classify(n)
    if n % 15 == 0 then
        return "fizzbuzz"
    elseif n % 5 == 0 then
        return "buzz"
    elseif n % 3 == 0 then
        return "fizz"
    end
    return tostring(n)
end
local fb = {}
for i = 1, 15 do
    fb[#fb + 1] = classify(i)
end
emit(table.concat(fb, " "))

local function primes(limit)
    local found = {}
    local n = 1
    while true do
        n += 1
        if n > limit then
            break
        end
        local isPrime = true
        for _, p in ipairs(found) do
            if p * p > n then
                break
            end
            if n % p == 0 then
                isPrime = false
                break
            end
        end
        if not isPrime then
            continue
        end
        found[#found + 1] = n
    end
    return found
end
emit(table.concat(primes(50), " "))

local function collatz(n)
    local steps = 0
    repeat
        if n % 2 == 0 then
            n //= 2
        else
            n = 3 * n + 1
        end
        steps += 1
        if steps > 1000 then
            return -1
        end
    until n == 1
    return steps
end
emit(collatz(27), collatz(1), collatz(97))

local function machine(input)
    local state, pos, tokens = "start", 1, {}
    while state ~= "done" do
        local ch = input:sub(pos, pos)
        if state == "start" then
            if ch == "" then
                state = "done"
            elseif ch:match("%d") then
                state = "number"
                tokens[#tokens + 1] = {kind = "num", text = ch}
                pos += 1
            elseif ch:match("%s") then
                pos += 1
            else
                tokens[#tokens + 1] = {kind = "op", text = ch}
                pos += 1
            end
        elseif state == "number" then
            if ch:match("%d") then
                tokens[#tokens].text ..= ch
                pos += 1
            else
                state = "start"
            end
        end
    end
    local parts = {}
    for _, tok in ipairs(tokens) do
        parts[#parts + 1] = tok.kind .. "(" .. tok.text .. ")"
    end
    return table.concat(parts, " ")
end
emit(machine("12 + 345*6 - 7"))

local function search(grid, target)
    for r, row in ipairs(grid) do
        for c, v in ipairs(row) do
            if v == target then
                return r, c
            end
            if v > target then
                break
            end
        end
    end
    return nil, nil
end
emit(search({{1, 3, 5}, {7, 9, 11}, {13, 15}}, 9))
emit(search({{1, 3, 5}}, 4))

-- 7. Boolean logic soup: nested and/or, not chains, comparisons against nil/false, if-expressions in expressions
local function logic(p, q, r)
    local t1 = p and q or r
    local t2 = (p or q) and not r
    local t3 = not (p and q) == (not p or not q)
    local t4 = p ~= nil and q ~= false and r ~= 0
    local t5 = if p then (if q then "pq" else "p") else (if r then "r" else "none")
    local t6 = (p and 1 or 0) + (q and 2 or 0) + (r and 4 or 0)
    return t1, t2, t3, t4, t5, t6
end
emit(logic(true, false, 7))
emit(logic(nil, true, false))
emit(logic(1, 2, 0))
local n = 12
emit(n > 10 and n < 20, n < 10 or n > 11, not (n == 12), n >= 12 and n <= 12, n ~= 12 or n > 0, n == 12 and "twelve")
emit(nil == false, nil ~= false, not nil == not false, 0 and "zero is truthy", "" and "empty is truthy")

-- 8. Numeric corner cases and arithmetic mix
emit(7 // 2, -7 // 2, 7 % -3, -7 % 3, 2 ^ 10, 10 / 4, 1e300 * 1e10, -0.0 == 0.0, 3 == 3.0, 0.1 + 0.2 == 0.3)
emit(math.floor(-2.5), math.ceil(-2.5), math.round(2.5), math.max(1, 2, 3), math.min(4, 5), math.clamp(15, 0, 10), math.sign(-3))
emit(bit32.band(0xF0F0, 0xFF00), bit32.bor(1, 4), bit32.lshift(1, 31), bit32.rshift(0x80000000, 31), bit32.bnot(0), bit32.extract(0xABCD, 4, 8))
emit(tostring(1 / 0), tostring(-1 / 0), tostring(0 / 0) == tostring(0 / 0), 100000000000000, 2 ^ 53 + 1, 255 == 0xFF)

-- 9. Coroutines and error handling
local function producer(items)
    return coroutine.wrap(function()
        for _, item in ipairs(items) do
            coroutine.yield(item * 2)
        end
        return "end"
    end)
end
local gen = producer({1, 2, 3})
local got = {}
for _ = 1, 4 do
    got[#got + 1] = tostring(gen())
end
emit(table.concat(got, ","))

local co = coroutine.create(function(v)
    local sum = v
    while true do
        local nextv = coroutine.yield(sum)
        if nextv == nil then
            break
        end
        sum += nextv
    end
    return sum, "closed"
end)
local _, s1 = coroutine.resume(co, 10)
local _, s2 = coroutine.resume(co, 5)
local okc, s3, tag = coroutine.resume(co)
emit(s1, s2, okc, s3, tag, coroutine.status(co))

local ok, err = pcall(function()
    error({code = 42})
end)
local ok2, err2 = pcall(error, "plain", 0)
local ok3, res = pcall(function(x)
    return x * 2
end, 21)
local ok4, err4 = xpcall(function()
    local t = nil
    return t.field
end, function(e)
    return "handled: " .. (e:match("attempt to index") and "index" or e)
end)
emit(ok, type(err) == "table" and err.code, ok2, err2, ok3, res, ok4, err4)

-- 10. Deep nesting: closures capturing closures, upvalue mutation from several levels, shadowing
local function counterFactory()
    local total = 0
    local function make(step)
        local local_count = 0
        return function()
            local_count += 1
            total += step
            return local_count, total
        end, function()
            return function()
                total = total * 2
                return total
            end
        end
    end
    return make
end
local make = counterFactory()
local c1, dbl1 = make(1)
local c2 = make(10)
c1()
c2()
local lc, tot = c1()
emit(lc, tot, dbl1()(), c2())

do
    local x = 1
    do
        local x = x + 1
        do
            local x = x * 10
            emit("inner", x)
        end
        emit("middle", x)
    end
    emit("outer", x)
end

-- 11. Table manipulation: nested constructors, holes, table library, negative indices, multiple assignment
local matrix = {}
for i = 1, 3 do
    matrix[i] = {}
    for j = 1, 3 do
        matrix[i][j] = (i - 1) * 3 + j
    end
end
local flat = {}
for _, row in ipairs(matrix) do
    for _, v in ipairs(row) do
        table.insert(flat, v)
    end
end
table.sort(flat, function(l, r)
    return l > r
end)
local removed = table.remove(flat, 1)
table.insert(flat, 1, 0)
local nested = {a = {b = {c = {"deep", list = {1, 2, {3, 4}}}}}, [0] = "zero", [-1] = "neg", [1.5] = "frac", [true] = "bool"}
local q, w, e = 1, 2, 3
q, w, e = e, q, w
local t1, t2 = {}, {}
t1.next, t2.prev = t2, t1
emit(removed, table.concat(flat, ""), #flat, nested.a.b.c[1], nested.a.b.c.list[3][2], nested[0], nested[-1], nested[1.5], nested[true])
emit(q, w, e, t1.next == t2, t2.prev == t1, #matrix, matrix[2][2], table.find(flat, 5), table.concat({}, ","), next({}))

-- 12. Final: string building with .. chains, tostring on everything, and the report
local report = "lines=" .. #out .. ";" .. "first=" .. out[1]:sub(1, 6) .. ";" .. "last=" .. out[#out]:sub(-6)
emit(report)
for i, line in ipairs(out) do
    print(string.format("%02d %s", i, line))
end
