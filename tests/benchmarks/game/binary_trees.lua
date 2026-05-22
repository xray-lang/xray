-- 作者：xingleixu@gmail.com
-- Binary Trees Benchmark - Lua版本
-- 用于性能对比测试

local function makeTree(depth)
    if depth > 0 then
        return {
            left = makeTree(depth - 1),
            right = makeTree(depth - 1)
        }
    else
        return {left = nil, right = nil}
    end
end

local function checksum(node)
    if node == nil then
        return 0
    end
    if node.left == nil then
        return 1
    end
    return checksum(node.left) + checksum(node.right) + 1
end

local function run(maxDepth)
    local minDepth = 4
    
    -- Stretch tree
    local stretchDepth = maxDepth + 1
    local stretchTree = makeTree(stretchDepth)
    print("stretch tree of depth " .. stretchDepth .. "\t check: " .. checksum(stretchTree))
    
    -- Long-lived tree
    local longLivedTree = makeTree(maxDepth)
    
    -- Iterations
    local depth = minDepth
    while depth <= maxDepth do
        local iterations = 1
        for i = 0, maxDepth - depth + minDepth - 1 do
            iterations = iterations * 2
        end
        
        local check = 0
        for i = 1, iterations do
            local tree = makeTree(depth)
            check = check + checksum(tree)
        end
        
        print(iterations .. "\t trees of depth " .. depth .. "\t check: " .. check)
        
        depth = depth + 2
    end
    
    print("long lived tree of depth " .. maxDepth .. "\t check: " .. checksum(longLivedTree))
end

-- 从命令行读取深度参数，默认 16
local maxDepth = tonumber(arg and arg[1]) or 16
run(maxDepth)
