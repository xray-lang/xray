--[[
  knucleotide.lua - K-Nucleotide 频率统计基准测试 Lua 版本
  
  作者：xingleixu@gmail.com
  
  算法说明：
  1. 使用 LCG 随机数生成 DNA 序列
  2. 使用 table 统计 k-nucleotide 频率
  3. 输出频率分布和特定序列计数
]]

-- 统计 k-nucleotide 频率
local function count_frequencies(seq, k)
    local freq = {}
    local seq_len = #seq
    local end_pos = seq_len - k + 1
    
    for i = 1, end_pos do
        local subseq = string.sub(seq, i, i + k - 1)
        freq[subseq] = (freq[subseq] or 0) + 1
    end
    
    return freq
end

-- 输出特定序列的计数
local function print_count(seq, frag)
    local k = #frag
    local freq = count_frequencies(seq, k)
    local count = freq[frag] or 0
    print(count .. "\t" .. frag)
end

-- 输出 k-nucleotide 频率分布
local function print_frequencies(seq, k)
    local freq = count_frequencies(seq, k)
    local total = #seq - k + 1
    
    -- 收集键值对
    local items = {}
    for key, count in pairs(freq) do
        table.insert(items, {key = key, count = count})
    end
    
    -- 排序：按频率降序，相同频率按键降序
    table.sort(items, function(a, b)
        if a.count ~= b.count then
            return a.count > b.count
        end
        return a.key > b.key
    end)
    
    -- 输出结果
    for _, p in ipairs(items) do
        local percentage = p.count * 100.0 / total
        print(string.format("%s %.3f", p.key, percentage))
    end
    print("")
end

-- LCG 随机数生成器参数
local IM = 139968
local IA = 3877
local IC = 29573
local seed = 42

-- Human Sapiens 频率
local hs_chars = {"A", "C", "G", "T"}
local hs_probs = {0.3029549426680, 0.1979883004921, 0.1975473066391, 0.3015094502008}

-- 累积概率
local function make_cumulative(probs)
    local cp = 0.0
    for i = 1, #probs do
        cp = cp + probs[i]
        probs[i] = cp
    end
end

-- 选择随机字符
local function select_random(chars, probs)
    seed = (seed * IA + IC) % IM
    local r = seed / IM
    for i = 1, #probs do
        if r < probs[i] then
            return chars[i]
        end
    end
    return chars[#probs]
end

-- 生成测试用的 DNA 序列
local function generate_sequence(n)
    local parts = {}
    for i = 1, n do
        parts[i] = select_random(hs_chars, hs_probs)
    end
    return table.concat(parts)
end

-- 主程序
local function main()
    local n = tonumber(arg and arg[1]) or 1000
    make_cumulative(hs_probs)
    local seq = generate_sequence(n * 5)
    
    -- 单核苷酸和双核苷酸频率
    print_frequencies(seq, 1)
    print_frequencies(seq, 2)
    
    -- 特定序列计数
    print_count(seq, "GGT")
    print_count(seq, "GGTA")
    print_count(seq, "GGTATT")
    print_count(seq, "GGTATTTTAATT")
    print_count(seq, "GGTATTTTAATTTATAGT")
end

main()
