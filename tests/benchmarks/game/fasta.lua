--[[
    fasta.lua - FASTA DNA 序列生成基准测试 Lua 版本
    
    作者：xingleixu@gmail.com
    
    算法说明：
    1. repeat_fasta: 通过循环复制给定序列生成 DNA 序列
    2. random_fasta: 使用加权随机选择从字母表生成 DNA 序列
       - 使用线性同余生成器 (LCG) 产生伪随机数
       - 将概率转换为累积概率，进行线性查找选择核苷酸
    
    LCG 参数：
      IM = 139968, IA = 3877, IC = 29573, Seed = 42
      random() = (Seed * IA + IC) % IM
]]

-- LCG 随机数生成器参数
local IM = 139968
local IA = 3877
local IC = 29573

-- 每行最大字符数
local LINE_WIDTH = 60

-- 全局随机数种子
local seed = 42

-- ALU 重复序列 - 人类 Alu 序列
local alu =
    "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTG" ..
    "GGAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGA" ..
    "GACCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAA" ..
    "AATACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAAT" ..
    "CCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAAC" ..
    "CCGGGAGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTG" ..
    "CACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA"

-- IUB 模糊代码 - 15 种核苷酸及其概率
local iub = {
    {c = 'a', p = 0.27}, {c = 'c', p = 0.12},
    {c = 'g', p = 0.12}, {c = 't', p = 0.27},
    {c = 'B', p = 0.02}, {c = 'D', p = 0.02},
    {c = 'H', p = 0.02}, {c = 'K', p = 0.02},
    {c = 'M', p = 0.02}, {c = 'N', p = 0.02},
    {c = 'R', p = 0.02}, {c = 'S', p = 0.02},
    {c = 'V', p = 0.02}, {c = 'W', p = 0.02},
    {c = 'Y', p = 0.02}
}

-- 人类同源序列频率 - 4 种核苷酸
local homosapiens = {
    {c = 'a', p = 0.3029549426680},
    {c = 'c', p = 0.1979883004921},
    {c = 'g', p = 0.1975473066391},
    {c = 't', p = 0.3015094502008}
}

-- 生成 [0, max) 范围内的随机数
local function random_num(max)
    seed = (seed * IA + IC) % IM
    return max * seed / IM
end

-- 将概率数组转换为累积概率
local function make_cumulative(table)
    local cp = 0.0
    for i = 1, #table do
        cp = cp + table[i].p
        table[i].p = cp
    end
end

-- 从累积概率表中随机选择一个核苷酸（线性查找）
local function select_random(tbl)
    local r = random_num(1.0)
    local n = #tbl
    for i = 1, n do
        if r < tbl[i].p then
            return tbl[i].c
        end
    end
    return tbl[n].c
end

-- 重复复制给定序列生成 DNA
local function repeat_fasta(id, desc, seq, count)
    io.write(">" .. id .. " " .. desc .. "\n")
    
    local len = #seq
    local pos = 1
    local line = {}
    
    while count > 0 do
        local line_len = count
        if line_len > LINE_WIDTH then
            line_len = LINE_WIDTH
        end
        
        -- 从当前位置复制字符到行缓冲区
        for i = 1, line_len do
            line[i] = string.sub(seq, pos, pos)
            pos = pos + 1
            if pos > len then
                pos = 1  -- 循环回到序列开头
            end
        end
        
        io.write(table.concat(line, "", 1, line_len) .. "\n")
        count = count - line_len
    end
end

-- 使用加权随机选择生成 DNA 序列
local function random_fasta(id, desc, tbl, count)
    io.write(">" .. id .. " " .. desc .. "\n")
    
    local line = {}
    
    while count > 0 do
        local line_len = count
        if line_len > LINE_WIDTH then
            line_len = LINE_WIDTH
        end
        
        for i = 1, line_len do
            line[i] = select_random(tbl)
        end
        
        io.write(table.concat(line, "", 1, line_len) .. "\n")
        count = count - line_len
    end
end

-- 主程序
local function main()
    -- 获取序列长度参数，默认 1000
    local n = tonumber(arg and arg[1]) or 1000
    
    -- 预处理：将概率转换为累积概率
    make_cumulative(iub)
    make_cumulative(homosapiens)
    
    -- 生成三种 DNA 序列：
    -- ONE:   重复 ALU 序列 2n 次
    -- TWO:   IUB 模糊代码随机序列 3n 次
    -- THREE: 人类同源序列频率随机序列 5n 次
    repeat_fasta("ONE", "Homo sapiens alu", alu, n * 2)
    random_fasta("TWO", "IUB ambiguity codes", iub, n * 3)
    random_fasta("THREE", "Homo sapiens frequency", homosapiens, n * 5)
end

main()
