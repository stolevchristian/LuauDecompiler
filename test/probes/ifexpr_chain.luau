-- if-expressions with elseif arms and mixed value/statement and-or chains
local function grade(s)
    return if s >= 90 then "A" elseif s >= 80 then "B" elseif s >= 70 then "C" else "F"
end
local function pick(a, b, c)
    local x = a and b or c
    local y = (a or b) and (c or "d")
    local z = if a then (b or c) else (c and b)
    return x, y, z
end
print(grade(95), grade(85), grade(75), grade(10))
print(pick(nil, false, 3), pick(1, nil, nil), pick(false, 2, false))
