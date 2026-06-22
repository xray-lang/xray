-- GC Stress Test: Linked List Churn
-- Tests short-lived object allocation throughput.
-- Creates and immediately discards linked lists of various lengths,
-- forcing the GC to handle massive ephemeral allocation.
--
-- Usage: lua gc_linkedlist_churn.lua [iterations]

local function makeList(length)
    local node = nil
    for i = 0, length - 1 do
        node = { value = i, next = node }
    end
    return node
end

local function sumList(node)
    local sum = 0
    while node ~= nil do
        sum = sum + node.value
        node = node.next
    end
    return sum
end

local function run(n)
    local total = 0

    -- Phase 1: Many short lists (high churn, young gen pressure)
    for i = 1, n * 100 do
        local list = makeList(10)
        total = total + sumList(list)
    end
    print(string.format("Phase 1 (short lists x%d): sum=%d", n * 100, total))

    -- Phase 2: Fewer medium lists (mixed lifetime)
    total = 0
    for i = 1, n * 10 do
        local list = makeList(100)
        total = total + sumList(list)
    end
    print(string.format("Phase 2 (medium lists x%d): sum=%d", n * 10, total))

    -- Phase 3: Few long lists (promotion pressure)
    total = 0
    for i = 1, n do
        local list = makeList(1000)
        total = total + sumList(list)
    end
    print(string.format("Phase 3 (long lists x%d): sum=%d", n, total))
end

local n = tonumber(arg[1]) or 1000
run(n)
