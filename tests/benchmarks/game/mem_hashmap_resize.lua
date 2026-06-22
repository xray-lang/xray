-- GC Stress Test: Hash Map Resize
-- Tests write barrier pressure from frequent map mutations.
-- Old maps get new young values written in, triggering back barriers.
-- Also tests map growth/rehash behavior under GC.
--
-- Usage: lua gc_hashmap_resize.lua [iterations]

local function run(n)
    -- Phase 1: Build a long-lived map, then repeatedly overwrite values
    local bigMap = {}
    for i = 0, 999 do
        bigMap["key_" .. i] = i
    end
    print("Phase 1: built map with 1000 keys")

    -- Repeatedly overwrite with new objects (write barrier stress)
    for round = 0, n - 1 do
        for i = 0, 999 do
            bigMap["key_" .. i] = { v = round * 1000 + i }
        end
    end
    local check = bigMap["key_500"].v
    print(string.format("Phase 1 done: %d rounds, check=%d", n, check))

    -- Phase 2: Create and discard many small maps (allocation churn)
    local total = 0
    for i = 1, n * 10 do
        local m = {}
        for j = 0, 19 do
            m["k" .. j] = j * j
        end
        total = total + m["k10"]
    end
    print(string.format("Phase 2 (small map churn x%d): total=%d", n * 10, total))

    -- Phase 3: Nested maps (deep reference chains, mark traversal stress)
    local root = {}
    for i = 0, n - 1 do
        local inner = {}
        for j = 0, 9 do
            inner["field_" .. j] = { data = i * 10 + j }
        end
        root["node_" .. (i % 100)] = inner
    end
    local deepCheck = root["node_0"]["field_5"].data
    print(string.format("Phase 3 (nested maps): check=%d", deepCheck))
end

local n = tonumber(arg[1]) or 500
run(n)
