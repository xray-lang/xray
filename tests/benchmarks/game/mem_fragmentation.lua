-- GC Stress Test: Fragmentation
-- Tests memory fragmentation and reclamation efficiency.
-- Allocates objects of varying sizes in interleaved patterns,
-- then selectively frees some to create "holes" in memory.
-- Good GC/allocators (like Immix) should handle this efficiently.
--
-- Usage: lua gc_fragmentation.lua [iterations]

local function run(n)
    -- Phase 1: Swiss cheese pattern
    local objects = {}
    for i = 0, 1999 do
        if i % 3 == 0 then
            objects[i] = { type = "small", id = i }
        elseif i % 3 == 1 then
            objects[i] = { type = "medium", id = i, a = 1, b = 2, c = 3, d = 4 }
        else
            objects[i] = { type = "large", id = i, a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8 }
        end
    end
    print("Phase 1: allocated 2000 objects")

    -- Punch holes: remove every other object
    local checksum = 0
    for round = 0, n - 1 do
        for i = 0, 1999 do
            if i % 2 == round % 2 then
                objects[i] = nil
            end
        end
        -- Allocate new objects into the holes
        for i = 0, 1999 do
            if objects[i] == nil then
                objects[i] = { round = round, idx = i, data = round * 2000 + i }
                checksum = checksum + objects[i].data
            end
        end
    end
    print(string.format("Phase 1 done (%d rounds): checksum=%d", n, checksum))

    -- Phase 2: Generational fragmentation
    local permanent = {}
    for i = 0, 199 do
        permanent[i] = { id = i, children = {} }
    end

    local total = 0
    for round = 0, n - 1 do
        for i = 0, 199 do
            local kids = {}
            for j = 0, 4 do
                kids[j] = { parent = i, gen = round, val = round * 5 + j }
            end
            permanent[i].children = kids
        end
        total = total + permanent[round % 200].children[round % 5].val
    end
    print(string.format("Phase 2 (generational frag x%d): total=%d", n, total))

    -- Phase 3: Reallocate with different sizes
    local mixed = {}
    for round = 0, n - 1 do
        mixed = {}
        for i = 0, 499 do
            if i % 4 == 0 then
                mixed[i] = { x = i }
            elseif i % 4 == 1 then
                mixed[i] = { x = i, y = i * 2, z = i * 3 }
            elseif i % 4 == 2 then
                mixed[i] = { x = i, a = 1, b = 2, c = 3, d = 4, e = 5 }
            else
                mixed[i] = "str_" .. i .. "_round_" .. round
            end
        end
    end
    print(string.format("Phase 3 (size mixing x%d): final array length=%d", n, 500))
end

local n = tonumber(arg[1]) or 500
run(n)
