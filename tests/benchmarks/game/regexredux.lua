-- 作者：xingleixu@gmail.com
-- Regex Redux 基准测试 - Lua 版本
-- 
-- 算法说明：
-- 1. 读取 FASTA 格式文件，记录原始长度
-- 2. 使用 Lua 模式匹配去除描述行和换行符，记录清理后长度
-- 3. 使用 9 个 DNA 模式匹配并计数
-- 4. 使用 5 个"魔术"模式替换，记录最终长度
-- 
-- 注意：Lua 使用自己的模式匹配语法，与标准正则略有不同

-- 计数模式匹配次数
local function countMatches(pattern, text)
    local count = 0
    for _ in text:gmatch(pattern) do
        count = count + 1
    end
    return count
end

-- 生成测试 FASTA 数据
local function generateFasta(n)
    local alu = "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGGGAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGACCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAATACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGGAGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA"
    
    local result = {}
    
    -- 生成 ONE 序列
    result[#result + 1] = ">ONE Homo sapiens alu"
    local aluLen = #alu
    local i = 0
    while i < n * 2 do
        local startPos = (i % aluLen) + 1
        local endPos = startPos + 59
        if endPos > aluLen then endPos = aluLen end
        result[#result + 1] = alu:sub(startPos, endPos)
        i = i + 60
    end
    
    -- 生成 TWO 序列
    result[#result + 1] = ">TWO IUB ambiguity codes"
    local iub = "acBDGHKMNRSVWY"
    local iubLen = #iub
    i = 0
    while i < n * 3 do
        local line = {}
        local j = 0
        while j < 60 and i + j < n * 3 do
            line[#line + 1] = iub:sub(((i + j) % iubLen) + 1, ((i + j) % iubLen) + 1)
            j = j + 1
        end
        result[#result + 1] = table.concat(line)
        i = i + 60
    end
    
    -- 生成 THREE 序列
    result[#result + 1] = ">THREE Homo sapiens frequency"
    local bases = "acgt"
    i = 0
    while i < n * 5 do
        local line = {}
        local j = 0
        while j < 60 and i + j < n * 5 do
            line[#line + 1] = bases:sub(((i + j) % 4) + 1, ((i + j) % 4) + 1)
            j = j + 1
        end
        result[#result + 1] = table.concat(line)
        i = i + 60
    end
    
    return table.concat(result, "\n")
end

-- Lua 模式匹配版本（Lua 不支持标准正则，使用自己的模式语法）
-- DNA 模式需要转换为 Lua 模式
local function main()
    -- 生成测试数据（从命令行读取，默认 100000）
    -- n = 100000 生成约 1MB 数据，用于基准测试
    -- n = 1000 生成约 10KB 数据，用于快速验证
    local n = tonumber(arg and arg[1]) or 100000
    local data = generateFasta(n)
    local initialLen = #data
    
    -- 步骤 1：移除 FASTA 描述行和换行符
    -- 先移除描述行（以 > 开头的行）
    local cleaned = data:gsub(">[^\n]*\n", "")
    -- 再移除所有换行符
    cleaned = cleaned:gsub("\n", "")
    local cleanedLen = #cleaned
    
    -- 步骤 2：计数 9 个 DNA 模式
    -- 注意：Lua 模式语法与标准正则不同
    -- [cgt] -> [cgt]
    -- a|b -> 需要分开匹配
    
    -- Lua 不支持 | 或运算，需要分开计数
    local patterns = {
        {"agggtaaa", "tttaccct"},
        {"[cgt]gggtaaa", "tttaccc[acg]"},
        {"a[act]ggtaaa", "tttacc[agt]t"},
        {"ag[act]gtaaa", "tttac[agt]ct"},
        {"agg[act]taaa", "ttta[agt]cct"},
        {"aggg[acg]aaa", "ttt[cgt]ccct"},
        {"agggt[cgt]aa", "tt[acg]accct"},
        {"agggta[cgt]a", "t[acg]taccct"},
        {"agggtaa[cgt]", "[acg]ttaccct"}
    }
    
    local patternNames = {
        "agggtaaa|tttaccct",
        "[cgt]gggtaaa|tttaccc[acg]",
        "a[act]ggtaaa|tttacc[agt]t",
        "ag[act]gtaaa|tttac[agt]ct",
        "agg[act]taaa|ttta[agt]cct",
        "aggg[acg]aaa|ttt[cgt]ccct",
        "agggt[cgt]aa|tt[acg]accct",
        "agggta[cgt]a|t[acg]taccct",
        "agggtaa[cgt]|[acg]ttaccct"
    }
    
    -- Lua 模式是区分大小写的，需要转换为小写匹配
    local lowerCleaned = cleaned:lower()
    
    for i, pair in ipairs(patterns) do
        local count = countMatches(pair[1], lowerCleaned) + countMatches(pair[2], lowerCleaned)
        print(patternNames[i], count)
    end
    
    -- 步骤 3：执行魔术替换
    -- 注意：Lua 模式语法转换
    local current = cleaned
    
    -- tHa[Nt] -> tHa[NT]（Lua 使用相同语法）
    current = current:gsub("tHa[Nt]", "<4>")
    
    -- aND|caN|Ha[DS]|WaS -> 分别替换
    current = current:gsub("aND", "<3>")
    current = current:gsub("caN", "<3>")
    current = current:gsub("Ha[DS]", "<3>")
    current = current:gsub("WaS", "<3>")
    
    -- a[NSt]|BY -> 分别替换
    current = current:gsub("a[NSt]", "<2>")
    current = current:gsub("BY", "<2>")
    
    -- <[^>]*> -> <[^>]*>（相同语法）
    current = current:gsub("<[^>]*>", "|")
    
    -- |[^|][^|]*| -> 在 Lua 中需要转义 |
    current = current:gsub("|[^|][^|]*|", "-")
    
    local endLen = #current
    
    -- 输出三个长度
    print("")
    print(initialLen)
    print(cleanedLen)
    print(endLen)
end

main()
