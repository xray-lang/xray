--[[
    fannkuch_redux.lua - Fannkuch-Redux Benchmark Lua版本
    
    作者：xingleixu@gmail.com
    
    算法说明：
    1. 生成 n! 个排列
    2. 对每个排列执行"翻煎饼"操作：反转前 perm[1] 个元素，直到第一个元素是 1
    3. 统计最大翻转次数和校验和
]]

-- 翻转数组前 n 个元素
local function flip(perm, n)
    local i, j = 1, n
    while i < j do
        perm[i], perm[j] = perm[j], perm[i]
        i = i + 1
        j = j - 1
    end
end

-- 复制数组
local function copy_array(src, n)
    local dst = {}
    for i = 1, n do
        dst[i] = src[i]
    end
    return dst
end

-- 计算单个排列的翻转次数
local function count_flips(perm, n)
    local flips = 0
    local work = copy_array(perm, n)
    
    while work[1] ~= 1 do
        flip(work, work[1])
        flips = flips + 1
    end
    
    return flips
end

-- 生成下一个排列（使用计数器方法）
local function next_permutation(perm, count, n)
    -- 旋转排列
    local first = perm[1]
    for i = 1, n - 1 do
        perm[i] = perm[i + 1]
    end
    perm[n] = first
    
    -- 更新计数器
    local i = 2
    while count[i] >= i - 1 do
        count[i] = 0
        i = i + 1
        if i > n then
            return false  -- 完成所有排列
        end
        
        -- 旋转前 i 个元素
        first = perm[1]
        for j = 1, i - 1 do
            perm[j] = perm[j + 1]
        end
        perm[i] = first
    end
    count[i] = count[i] + 1
    return true
end

-- 运行 fannkuch-redux 算法
local function fannkuch(n)
    -- 初始化排列 [1, 2, 3, ..., n]
    local perm = {}
    local count = {}
    
    for i = 1, n do
        perm[i] = i
        count[i] = 0
    end
    
    local maxFlips = 0
    local checksum = 0
    local permIndex = 0
    
    -- 遍历所有排列
    repeat
        local flips = count_flips(perm, n)
        
        -- 更新最大翻转次数
        if flips > maxFlips then
            maxFlips = flips
        end
        
        -- 计算校验和：偶数索引加，奇数索引减
        if permIndex % 2 == 0 then
            checksum = checksum + flips
        else
            checksum = checksum - flips
        end
        
        permIndex = permIndex + 1
    until not next_permutation(perm, count, n)
    
    -- 输出结果
    print(checksum)
    print(string.format("Pfannkuchen(%d) = %d", n, maxFlips))
end

-- 获取命令行参数或使用默认值
local n = tonumber(arg and arg[1]) or 10
fannkuch(n)
