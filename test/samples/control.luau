local n = 0
for i = 1, 10 do
    n = n + i
end
for k, v in pairs({a = 1}) do
    print(k, v)
end
while n > 0 do
    n = n - 7
    if n == 3 then break end
end
repeat n = n + 1 until n >= 5
if n > 10 then
    print("a")
elseif n > 5 then
    print("b")
else
    print("c")
end
if n and n > 1 or n == nil then print("d") end
local s = 0
for i = 1, 3 do if i == 2 then continue end s = s + i end
print(n, s, ...)
