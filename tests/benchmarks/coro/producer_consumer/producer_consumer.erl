%% Producer-Consumer benchmark - Erlang equivalent
%% M producers send to a shared process, N consumers receive
%% Uses a broker process to simulate buffered channel
%% Usage: erl -noshell -s producer_consumer main -s init stop
-module(producer_consumer).
-export([main/0, main/3]).

main() ->
    main(10, 10, 100000).

main(Producers, Consumers, Items) ->
    io:format("=== 生产者消费者测试 (Erlang) ===~n"),
    io:format("生产者: ~p~n", [Producers]),
    io:format("消费者: ~p~n", [Consumers]),
    io:format("总数量: ~p~n", [Items]),

    Self = self(),
    T0 = erlang:monotonic_time(millisecond),

    %% Start broker (simulates buffered channel)
    Broker = spawn(fun() -> broker_loop(queue:new(), queue:new(), 0) end),

    ItemsPerProducer = Items div Producers,

    %% Start producers
    ProducerPids = [spawn(fun() ->
        producer_loop(Id, ItemsPerProducer, Broker),
        Self ! {producer_done, Id}
    end) || Id <- lists:seq(0, Producers - 1)],

    %% Start consumers
    ConsumerPids = [spawn(fun() ->
        Count = consumer_loop(Broker, 0),
        Self ! {consumer_done, Id, Count}
    end) || Id <- lists:seq(0, Consumers - 1)],

    %% Wait for all producers
    wait_producers(Producers),

    %% Signal broker to close
    Broker ! close,

    %% Collect consumer results
    TotalConsumed = collect_consumers(Consumers, 0),

    T1 = erlang:monotonic_time(millisecond),
    Elapsed = T1 - T0,
    io:format("消费总数: ~p~n", [TotalConsumed]),
    io:format("时间: ~p ms~n", [Elapsed]),
    case Elapsed > 0 of
        true ->
            io:format("吞吐量: ~p items/sec~n", [TotalConsumed * 1000 div Elapsed]);
        false ->
            io:format("吞吐量: 太快无法测量~n")
    end.

%% Broker: buffers items, dispatches to waiting consumers
%% Uses queue for both items and waiters (O(1) amortized)
broker_loop(Queue, WaitQ, Closed) ->
    receive
        {send, Item} ->
            case queue:out(WaitQ) of
                {{value, Waiter}, WaitQ2} ->
                    Waiter ! {item, Item},
                    broker_loop(Queue, WaitQ2, Closed);
                {empty, _} ->
                    broker_loop(queue:in(Item, Queue), WaitQ, Closed)
            end;
        {recv, Consumer} ->
            case queue:out(Queue) of
                {{value, Item}, Queue2} ->
                    Consumer ! {item, Item},
                    broker_loop(Queue2, WaitQ, Closed);
                {empty, _} when Closed =:= 1 ->
                    Consumer ! closed,
                    broker_loop(Queue, WaitQ, Closed);
                {empty, _} ->
                    broker_loop(Queue, queue:in(Consumer, WaitQ), Closed)
            end;
        close ->
            drain_waiters(WaitQ),
            broker_loop(Queue, queue:new(), 1)
    end.

drain_waiters(WaitQ) ->
    case queue:out(WaitQ) of
        {{value, W}, Rest} -> W ! closed, drain_waiters(Rest);
        {empty, _} -> ok
    end.

producer_loop(_Id, 0, _Broker) ->
    ok;
producer_loop(Id, Count, Broker) ->
    Broker ! {send, Id * 1000000 + Count},
    producer_loop(Id, Count - 1, Broker).

consumer_loop(Broker, Count) ->
    Broker ! {recv, self()},
    receive
        {item, _Item} ->
            %% Simulate processing
            consumer_loop(Broker, Count + 1);
        closed ->
            Count
    end.

wait_producers(0) -> ok;
wait_producers(N) ->
    receive {producer_done, _} -> wait_producers(N - 1) end.

collect_consumers(0, Total) -> Total;
collect_consumers(N, Total) ->
    receive {consumer_done, _, Count} -> collect_consumers(N - 1, Total + Count) end.
