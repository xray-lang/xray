--[[
作者：xingleixu@gmail.com

Mandelbrot 集基准测试 - Lua 版本

算法说明：
Mandelbrot 集是复平面上的一个分形集合。对于复数 c = Cr + Ci*i，
迭代公式为 z(n+1) = z(n)^2 + c，初始值 z(0) = 0。
如果迭代 50 次后 |z| <= 2，则该点属于 Mandelbrot 集。

本程序计算 [-1.5, 0.5] x [-1.0, 1.0] 范围内的 Mandelbrot 集，
并输出为 PBM（Portable Bitmap）格式图像。
]]

-- 最大迭代次数
local MAX_ITER = 50

-- 逃逸半径的平方
local LIMIT_SQ = 4.0

-- 从命令行获取图像大小
local size = tonumber(arg[1]) or 200
local w, h = size, size

-- 集合内的点数计数
local count = 0

-- 预计算常量
local inv_w = 2.0 / w
local inv_h = 2.0 / h

-- 遍历每个像素
for y = 0, h - 1 do
    local Ci = inv_h * y - 1.0
    
    for x = 0, w - 1 do
        -- 初始化 z = 0
        local Zr, Zi = 0.0, 0.0
        local Tr, Ti = 0.0, 0.0
        
        -- 将像素坐标映射到复平面
        local Cr = inv_w * x - 1.5
        
        -- 迭代计算 z = z^2 + c
        local i = 0
        while i < MAX_ITER and (Tr + Ti <= LIMIT_SQ) do
            Zi = 2.0 * Zr * Zi + Ci
            Zr = Tr - Ti + Cr
            Tr = Zr * Zr
            Ti = Zi * Zi
            i = i + 1
        end
        
        -- 判断点是否在 Mandelbrot 集内
        if Tr + Ti <= LIMIT_SQ then
            count = count + 1
        end
    end
end

-- 输出结果
print("lua mandelbrot")
print(size)
print("Points in set:")
print(count)
