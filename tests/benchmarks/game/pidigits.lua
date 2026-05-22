-- 作者：xingleixu@gmail.com
-- Pi Digits Benchmark - Lua版本
-- 算法：GMP风格流式算法（支持有符号大整数）

-- 有符号大整数实现
local function bigint(n)
    local sign = 1
    local digits = {}
    if n < 0 then sign = -1; n = -n end
    if n == 0 then
        digits[1] = 0
    else
        while n > 0 do
            digits[#digits + 1] = n % 10
            n = math.floor(n / 10)
        end
    end
    return {sign = sign, digits = digits}
end

local function bigint_clone(a)
    local nd = {}
    for i = 1, #a.digits do nd[i] = a.digits[i] end
    return {sign = a.sign, digits = nd}
end

local function normalize(a)
    local d = a.digits
    while #d > 1 and d[#d] == 0 do d[#d] = nil end
    if #d == 1 and d[1] == 0 then a.sign = 1 end
    return a
end

local function compare_abs(a, b)
    local da, db = a.digits, b.digits
    if #da > #db then return 1 end
    if #da < #db then return -1 end
    for i = #da, 1, -1 do
        if da[i] > db[i] then return 1 end
        if da[i] < db[i] then return -1 end
    end
    return 0
end

local function add_abs(a, b)
    local da, db = a.digits, b.digits
    local rd = {}
    local carry = 0
    local len = math.max(#da, #db)
    for i = 1, len do
        local sum = (da[i] or 0) + (db[i] or 0) + carry
        rd[i] = sum % 10
        carry = math.floor(sum / 10)
    end
    if carry > 0 then rd[#rd + 1] = carry end
    return {sign = 1, digits = rd}
end

local function sub_abs(a, b)
    local da, db = a.digits, b.digits
    local rd = {}
    local borrow = 0
    for i = 1, #da do
        local diff = da[i] - (db[i] or 0) - borrow
        if diff < 0 then diff = diff + 10; borrow = 1 else borrow = 0 end
        rd[i] = diff
    end
    return normalize({sign = 1, digits = rd})
end

local function bigint_add(a, b)
    if a.sign == b.sign then
        local r = add_abs(a, b)
        r.sign = a.sign
        return r
    end
    local cmp = compare_abs(a, b)
    if cmp == 0 then return bigint(0) end
    if cmp > 0 then
        local r = sub_abs(a, b)
        r.sign = a.sign
        return r
    else
        local r = sub_abs(b, a)
        r.sign = b.sign
        return r
    end
end

local function bigint_sub(a, b)
    local nb = bigint_clone(b)
    if #b.digits == 1 and b.digits[1] == 0 then
        nb.sign = 1
    else
        nb.sign = -b.sign
    end
    return bigint_add(a, nb)
end

local function bigint_mul_int(a, n)
    if n == 0 then return bigint(0) end
    local rs = a.sign
    if n < 0 then rs = -rs; n = -n end
    local da = a.digits
    local rd = {}
    local carry = 0
    for i = 1, #da do
        local prod = da[i] * n + carry
        rd[i] = prod % 10
        carry = math.floor(prod / 10)
    end
    while carry > 0 do
        rd[#rd + 1] = carry % 10
        carry = math.floor(carry / 10)
    end
    return normalize({sign = rs, digits = rd})
end

local function bigint_div(a, b)
    local rs = a.sign * b.sign
    if compare_abs(a, b) < 0 then return bigint(0) end
    local da = a.digits
    local qd = {}
    for i = 1, #da do qd[i] = 0 end
    local rem = {sign = 1, digits = {}}
    for pos = #da, 1, -1 do
        local nrd = {da[pos]}
        for j = 1, #rem.digits do nrd[#nrd + 1] = rem.digits[j] end
        rem = normalize({sign = 1, digits = nrd})
        local q = 0
        while compare_abs(rem, b) >= 0 do
            rem = sub_abs(rem, b)
            q = q + 1
        end
        qd[pos] = q
    end
    return normalize({sign = rs, digits = qd})
end

local function bigint_to_int(a)
    local d = a.digits
    local r = 0
    local base = 1
    for i = 1, #d do
        r = r + d[i] * base
        base = base * 10
    end
    return r * a.sign
end

-- LFT 算法
local lft_q, lft_r, lft_s, lft_t

local function extract(x)
    local num = bigint_add(bigint_mul_int(lft_q, x), lft_r)
    local den = bigint_add(bigint_mul_int(lft_s, x), lft_t)
    local quot = bigint_div(num, den)
    return bigint_to_int(quot)
end

local function is_safe(digit)
    return extract(4) == digit
end

local function produce(digit)
    lft_r = bigint_mul_int(bigint_sub(lft_r, bigint_mul_int(lft_t, digit)), 10)
    lft_q = bigint_mul_int(lft_q, 10)
end

local function consume(k)
    local k2 = 2 * k + 1
    local k4 = 4 * k + 2
    local nr = bigint_add(bigint_mul_int(lft_q, k4), bigint_mul_int(lft_r, k2))
    local nt = bigint_add(bigint_mul_int(lft_s, k4), bigint_mul_int(lft_t, k2))
    lft_q = bigint_mul_int(lft_q, k)
    lft_s = bigint_mul_int(lft_s, k)
    lft_r = nr
    lft_t = nt
end

local function pidigits(n)
    lft_q = bigint(1)
    lft_r = bigint(0)
    lft_s = bigint(0)
    lft_t = bigint(1)
    
    local count = 0
    local k = 0
    local output = ""
    
    while count < n do
        k = k + 1
        consume(k)
        local digit = extract(3)
        if is_safe(digit) then
            output = output .. digit
            count = count + 1
            if count % 10 == 0 then
                print(output .. "\t:" .. count)
                output = ""
            end
            produce(digit)
        end
    end
    
    if #output > 0 then
        local pad = 10 - #output
        for i = 1, pad do output = output .. " " end
        print(output .. "\t:" .. n)
    end
end

local n = tonumber(arg[1]) or 100
pidigits(n)
