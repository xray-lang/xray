-- 作者：xingleixu@gmail.com
-- Reverse Complement Benchmark - Lua 版本
-- 读取 FASTA 格式 DNA 序列，输出反向补码

-- DNA 补码映射表
local COMP = {
    A = "T", a = "T",
    T = "A", t = "A",
    C = "G", c = "G",
    G = "C", g = "C",
    U = "A", u = "A",
    M = "K", m = "K",
    R = "Y", r = "Y",
    W = "W", w = "W",
    S = "S", s = "S",
    Y = "R", y = "R",
    K = "M", k = "M",
    V = "B", v = "B",
    H = "D", h = "D",
    D = "H", d = "H",
    B = "V", b = "V",
    N = "N", n = "N"
}

-- 反向补码一个序列
local function reverseComplement(seq)
    local result = {}
    local len = #seq
    for i = len, 1, -1 do
        local c = seq:sub(i, i)
        local comp = COMP[c]
        if comp then
            result[#result + 1] = comp
        end
    end
    return table.concat(result)
end

-- 按 60 字符宽度格式化输出
local function printFormatted(seq)
    local len = #seq
    for i = 1, len, 60 do
        local endPos = i + 59
        if endPos > len then endPos = len end
        print(seq:sub(i, endPos))
    end
end

-- 处理 FASTA 输入
local function processFasta(input)
    local header = ""
    local seq = {}
    
    for line in input:gmatch("[^\n]+") do
        if #line > 0 then
            if line:sub(1, 1) == ">" then
                -- 输出之前的序列
                if #header > 0 then
                    print(header)
                    printFormatted(reverseComplement(table.concat(seq)))
                end
                header = line
                seq = {}
            else
                seq[#seq + 1] = line
            end
        end
    end
    
    -- 输出最后一个序列
    if #header > 0 then
        print(header)
        printFormatted(reverseComplement(table.concat(seq)))
    end
end

-- 生成测试数据 (简化版)
local function generateFasta(n)
    local alu = "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGGGAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGACCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAATACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGGAGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA"
    
    local result = {">ONE Homo sapiens alu"}
    local aluLen = #alu
    local i = 0
    while i < n do
        local start = (i % aluLen) + 1
        local endPos = start + 59
        if endPos > aluLen then endPos = aluLen end
        result[#result + 1] = alu:sub(start, endPos)
        i = i + 60
    end
    
    result[#result + 1] = ">TWO IUB ambiguity codes"
    local iub = "ACGTMRWSYKVHDBN"
    local iubLen = #iub
    i = 0
    while i < n / 3 do
        local line = {}
        local j = 0
        while j < 60 and i + j < n / 3 do
            line[#line + 1] = iub:sub(((i + j) % iubLen) + 1, ((i + j) % iubLen) + 1)
            j = j + 1
        end
        result[#result + 1] = table.concat(line)
        i = i + 60
    end
    
    return table.concat(result, "\n")
end

-- 主程序 - 性能基准测试（从命令行读取，默认 250000）
local n = tonumber(arg and arg[1]) or 250000
local input = generateFasta(n)
processFasta(input)
