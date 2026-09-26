local function CFATestStress(A, B, C)
    local acc = 0
    local t = {10, 20, 30, 40, 50}
    local f = function(x) return x * 3, x - 1 end
    for i = 1, #t do
        local v = t[i]
        if v % 3 == 1 then
            local p, q = f(v + A)
            acc = acc + p
            if q < 0 then
                return acc
            end
        elseif v % 4 == 2 then
            acc = acc + (v * 2)
        else
            acc = acc - B
        end
   end
end
print(CFATestStress(1, 2, 3), CFATestStress(-50, 7, 0), CFATestStress(0, 100, 0), CFATestStress(-11, 1, 0))
