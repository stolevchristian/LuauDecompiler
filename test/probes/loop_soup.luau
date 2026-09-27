-- breaks and continues across nested while/repeat/for, closures capturing loop variables in a while loop
local fns, log = {}, {}
local n = 0
while n < 6 do
    n += 1
    local captured = n * 10
    fns[#fns + 1] = function() return captured end
    if n == 2 then
        continue
    end
    local k = 0
    repeat
        k += 1
        if k == 2 then
            break
        end
        for j = 1, 3 do
            if j == 2 then
                continue
            end
            if n == 5 then
                break
            end
            log[#log + 1] = n .. ":" .. k .. ":" .. j
        end
    until k >= 3
    if n == 4 then
        break
    end
end
local sum = 0
for _, f in ipairs(fns) do sum += f() end
print(sum, table.concat(log, " "))
