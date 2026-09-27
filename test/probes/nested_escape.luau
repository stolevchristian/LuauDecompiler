--!optimize 2
-- an inlined function whose `return` sits inside two nested loops
local function find(grid, target)
    for r = 1, #grid do
        for c = 1, #grid[r] do
            if grid[r][c] == target then
                return r * 100 + c
            end
        end
    end
    return -1
end
local g = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}
print(find(g, 5), find(g, 9), find(g, 42))
