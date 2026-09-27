-- `continue` inside repeat/until jumps to the condition, not to the back edge
local i, out = 0, {}
repeat
    i += 1
    if i % 2 == 0 then
        continue
    end
    out[#out + 1] = i
until i >= 9
print(table.concat(out, ","))
