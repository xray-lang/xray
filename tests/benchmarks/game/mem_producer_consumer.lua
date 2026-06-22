-- GC Stress Test: Producer-Consumer
-- Tests cross-generational references and write barrier overhead.
-- A long-lived queue (old gen) constantly receives new objects (young gen)
-- and old objects are dequeued and discarded.
-- This creates a steady stream of old→young references.
--
-- Usage: lua gc_producer_consumer.lua [iterations]

-- Ring buffer queue using table
local function createQueue(capacity)
    local data = {}
    for i = 1, capacity do
        data[i] = false
    end
    return { data = data, head = 1, tail = 1, size = 0, capacity = capacity }
end

local function enqueue(q, item)
    q.data[q.tail] = item
    q.tail = q.tail % q.capacity + 1
    q.size = q.size + 1
end

local function dequeue(q)
    local item = q.data[q.head]
    q.data[q.head] = false
    q.head = q.head % q.capacity + 1
    q.size = q.size - 1
    return item
end

local function run(n)
    local queueSize = 1000

    -- Phase 1: Steady-state producer-consumer
    local q = createQueue(queueSize)

    -- Fill queue initially
    for i = 0, queueSize - 1 do
        enqueue(q, { id = i, payload = "data_" .. i })
    end
    print("Queue initialized: size=" .. q.size)

    -- Steady-state: produce and consume at same rate
    local checksum = 0
    for i = 0, n * 1000 - 1 do
        local old = dequeue(q)
        checksum = checksum + old.id
        enqueue(q, { id = i, payload = "new_" .. (i % 100) })
    end
    print(string.format("Phase 1 (steady state x%d): checksum=%d", n * 1000, checksum))

    -- Phase 2: Burst producer (queue grows then shrinks)
    local burstQ = createQueue(5000)
    local total = 0
    for round = 0, n - 1 do
        -- Burst: produce 100 items
        for i = 0, 99 do
            enqueue(burstQ, { v = round * 100 + i })
        end
        -- Consume all
        while burstQ.size > 0 do
            local item = dequeue(burstQ)
            total = total + item.v
        end
    end
    print(string.format("Phase 2 (burst x%d): total=%d", n, total))

    -- Phase 3: Multiple queues with cross-references
    local q1 = createQueue(500)
    local q2 = createQueue(500)
    for i = 0, 499 do
        local item1 = { id = i, partner = false }
        local item2 = { id = i + 10000, partner = item1 }
        item1.partner = item2
        enqueue(q1, item1)
        enqueue(q2, item2)
    end
    -- Churn with cross-references
    checksum = 0
    for i = 0, n * 100 - 1 do
        local old1 = dequeue(q1)
        local old2 = dequeue(q2)
        checksum = checksum + old1.id + old2.partner.id
        local new1 = { id = i, partner = false }
        local new2 = { id = i + 10000, partner = new1 }
        new1.partner = new2
        enqueue(q1, new1)
        enqueue(q2, new2)
    end
    print(string.format("Phase 3 (cross-ref x%d): checksum=%d", n * 100, checksum))
end

local n = tonumber(arg[1]) or 500
run(n)
