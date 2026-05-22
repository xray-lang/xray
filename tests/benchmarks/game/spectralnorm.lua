--[[
    spectralnorm.lua - Spectral Norm Benchmark Lua版本
    
    作者：xingleixu@gmail.com
    
    算法说明：
    计算无限矩阵 A 的谱范数（spectral norm）
    矩阵 A 的元素定义：a(i,j) = 1 / ((i+j)*(i+j+1)/2 + i + 1)
    使用幂迭代法（power iteration）计算最大特征值的平方根
    
    实现 4 个核心函数：
    1. A(i,j) - 计算矩阵元素
    2. Au(u,v) - 矩阵乘向量 v = A * u
    3. Atu(u,v) - 转置矩阵乘向量 v = A^T * u
    4. AtAu(u,v,w) - 组合操作 v = A^T * A * u
]]

-- 计算矩阵元素 A(i,j)
-- 注意：Lua 使用 0-based 索引计算，但数组是 1-based
local function A(i, j)
    return 1.0 / ((i + j) * (i + j + 1) / 2 + i + 1)
end

-- 矩阵乘向量：v = A * u
local function Au(u, v, n)
    for i = 0, n - 1 do
        local sum = 0.0
        for j = 0, n - 1 do
            sum = sum + A(i, j) * u[j + 1]
        end
        v[i + 1] = sum
    end
end

-- 转置矩阵乘向量：v = A^T * u
local function Atu(u, v, n)
    for i = 0, n - 1 do
        local sum = 0.0
        for j = 0, n - 1 do
            sum = sum + A(j, i) * u[j + 1]
        end
        v[i + 1] = sum
    end
end

-- 组合操作：v = A^T * A * u
local function AtAu(u, v, w, n)
    Au(u, w, n)   -- w = A * u
    Atu(w, v, n)  -- v = A^T * w
end

-- 计算向量点积
local function dot(u, v, n)
    local sum = 0.0
    for i = 1, n do
        sum = sum + u[i] * v[i]
    end
    return sum
end

-- 计算谱范数
local function spectralnorm(n)
    -- 初始化向量
    local u = {}
    local v = {}
    local w = {}
    
    for i = 1, n do
        u[i] = 1.0
        v[i] = 0.0
        w[i] = 0.0
    end
    
    -- 幂迭代：执行 10 次
    for _ = 1, 10 do
        AtAu(u, v, w, n)  -- v = A^T * A * u
        AtAu(v, u, w, n)  -- u = A^T * A * v
    end
    
    -- 计算 sqrt((u · v) / (v · v))
    local vBv = dot(u, v, n)
    local vv = dot(v, v, n)
    
    return math.sqrt(vBv / vv)
end

-- 获取命令行参数或使用默认值
local n = tonumber(arg and arg[1]) or 500
print(string.format("%.9f", spectralnorm(n)))
