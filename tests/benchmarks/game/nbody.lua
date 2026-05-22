--[[
    nbody.lua - N-Body Benchmark Lua版本
    
    作者：xingleixu@gmail.com
    
    算法说明：
    1. 模拟太阳系中5个天体的运动（太阳、木星、土星、天王星、海王星）
    2. 使用牛顿万有引力定律计算天体之间的相互作用力
    3. 使用Leapfrog积分法更新速度和位置
    4. 计算系统总能量来验证模拟的正确性
    
    物理常量：
    - 引力常数 G = 1（使用天文单位制）
    - 时间步长 dt = 0.01（天）
    - 太阳质量 = 1（归一化）
]]

local PI = 3.141592653589793
local DAYS_PER_YEAR = 365.24
local SOLAR_MASS = 4.0 * PI * PI

-- 创建天体
local function Body(x, y, z, vx, vy, vz, mass)
    return {
        x = x,
        y = y,
        z = z,
        vx = vx,
        vy = vy,
        vz = vz,
        mass = mass
    }
end

-- 初始化太阳系天体
local function create_bodies()
    local bodies = {}
    
    -- 太阳
    bodies[1] = Body(
        0.0, 0.0, 0.0,
        0.0, 0.0, 0.0,
        SOLAR_MASS
    )
    
    -- 木星
    bodies[2] = Body(
        4.84143144246472090e+00,
        -1.16032004402742839e+00,
        -1.03622044471123109e-01,
        1.66007664274403694e-03 * DAYS_PER_YEAR,
        7.69901118419740425e-03 * DAYS_PER_YEAR,
        -6.90460016972063023e-05 * DAYS_PER_YEAR,
        9.54791938424326609e-04 * SOLAR_MASS
    )
    
    -- 土星
    bodies[3] = Body(
        8.34336671824457987e+00,
        4.12479856412430479e+00,
        -4.03523417114321381e-01,
        -2.76742510726862411e-03 * DAYS_PER_YEAR,
        4.99852801234917238e-03 * DAYS_PER_YEAR,
        2.30417297573763929e-05 * DAYS_PER_YEAR,
        2.85885980666130812e-04 * SOLAR_MASS
    )
    
    -- 天王星
    bodies[4] = Body(
        1.28943695621391310e+01,
        -1.51111514016986312e+01,
        -2.23307578892655734e-01,
        2.96460137564761618e-03 * DAYS_PER_YEAR,
        2.37847173959480950e-03 * DAYS_PER_YEAR,
        -2.96589568540237556e-05 * DAYS_PER_YEAR,
        4.36624404335156298e-05 * SOLAR_MASS
    )
    
    -- 海王星
    bodies[5] = Body(
        1.53796971148509165e+01,
        -2.59193146099879641e+01,
        1.79258772950371181e-01,
        2.68067772490389322e-03 * DAYS_PER_YEAR,
        1.62824170038242295e-03 * DAYS_PER_YEAR,
        -9.51592254519715870e-05 * DAYS_PER_YEAR,
        5.15138902046611451e-05 * SOLAR_MASS
    )
    
    return bodies
end

--[[
    调整系统动量，使总动量为零（质心参考系）
    通过调整太阳的速度来实现
]]
local function offset_momentum(bodies)
    local px, py, pz = 0.0, 0.0, 0.0
    
    for i = 1, #bodies do
        local b = bodies[i]
        px = px + b.vx * b.mass
        py = py + b.vy * b.mass
        pz = pz + b.vz * b.mass
    end
    
    -- 太阳获得相反的动量
    bodies[1].vx = -px / SOLAR_MASS
    bodies[1].vy = -py / SOLAR_MASS
    bodies[1].vz = -pz / SOLAR_MASS
end

--[[
    计算系统总能量
    能量 = 动能 + 势能
    动能 = 0.5 * m * v^2
    势能 = -G * m1 * m2 / r（G=1）
]]
local function energy(bodies)
    local e = 0.0
    local nbodies = #bodies
    
    for i = 1, nbodies do
        local bi = bodies[i]
        
        -- 动能
        e = e + 0.5 * bi.mass * (bi.vx * bi.vx + bi.vy * bi.vy + bi.vz * bi.vz)
        
        -- 势能（与所有后续天体的相互作用）
        for j = i + 1, nbodies do
            local bj = bodies[j]
            
            local dx = bi.x - bj.x
            local dy = bi.y - bj.y
            local dz = bi.z - bj.z
            
            local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
            e = e - (bi.mass * bj.mass) / distance
        end
    end
    
    return e
end

--[[
    执行一个时间步的模拟
    dt: 时间步长
    
    算法：
    1. 计算所有天体对之间的引力
    2. 更新速度
    3. 更新位置
]]
local function advance(bodies, dt)
    local nbodies = #bodies
    
    -- 计算引力并更新速度
    for i = 1, nbodies do
        local bi = bodies[i]
        
        for j = i + 1, nbodies do
            local bj = bodies[j]
            
            local dx = bi.x - bj.x
            local dy = bi.y - bj.y
            local dz = bi.z - bj.z
            
            local distanceSquared = dx * dx + dy * dy + dz * dz
            local distance = math.sqrt(distanceSquared)
            local mag = dt / (distanceSquared * distance)
            
            -- 更新速度（牛顿第三定律：作用力与反作用力）
            bi.vx = bi.vx - dx * bj.mass * mag
            bi.vy = bi.vy - dy * bj.mass * mag
            bi.vz = bi.vz - dz * bj.mass * mag
            
            bj.vx = bj.vx + dx * bi.mass * mag
            bj.vy = bj.vy + dy * bi.mass * mag
            bj.vz = bj.vz + dz * bi.mass * mag
        end
    end
    
    -- 更新位置
    for i = 1, nbodies do
        local bi = bodies[i]
        bi.x = bi.x + dt * bi.vx
        bi.y = bi.y + dt * bi.vy
        bi.z = bi.z + dt * bi.vz
    end
end

-- 主程序
local function main()
    -- 从命令行获取迭代次数，默认 5000000
    local n = tonumber(arg and arg[1]) or 500000
    
    -- 创建天体系统
    local bodies = create_bodies()
    
    -- 初始化：调整动量使系统质心静止
    offset_momentum(bodies)
    
    -- 输出初始能量
    print(string.format("%.9f", energy(bodies)))
    
    -- 执行 n 次模拟步骤
    for i = 1, n do
        advance(bodies, 0.01)
    end
    
    -- 输出最终能量
    print(string.format("%.9f", energy(bodies)))
end

main()
