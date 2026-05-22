%% Fanout benchmark - Erlang equivalent
%% Dispatcher sends tasks to N workers via shared broker, collector gathers results
%% Usage: erl -noshell -s fanout main -s init stop
-module(fanout).
-export([main/0, main/2]).

main() ->
    main(1000, 100000).

main(N, Tasks) ->
    io:format("=== Fanout 测试 (Erlang) ===~n"),
    io:format("工作协程: ~p~n", [N]),
    io:format("任务数量: ~p~n", [Tasks]),

    Self = self(),
    T0 = erlang:monotonic_time(millisecond),

    %% Task channel (simulated by broker)
    TaskCh = spawn(fun() -> task_broker(queue:new(), [], 0) end),

    %% Start N workers, each loops: recv from TaskCh, send result to Self
    [spawn(fun() -> worker(TaskCh, Self) end) || _ <- lists:seq(1, N)],

    %% Distribute tasks
    [TaskCh ! {send, I} || I <- lists:seq(0, Tasks - 1)],

    %% Signal close after all tasks sent
    TaskCh ! close,

    %% Collect results
    Sum = collect_results(Tasks, 0),

    T1 = erlang:monotonic_time(millisecond),
    Elapsed = T1 - T0,
    Expected = Tasks * (Tasks - 1),
    io:format("结果: ~p~n", [Sum]),
    io:format("预期: ~p~n", [Expected]),
    io:format("正确: ~p~n", [Sum =:= Expected]),
    io:format("时间: ~p ms~n", [Elapsed]),
    case Elapsed > 0 of
        true ->
            io:format("吞吐量: ~p tasks/sec~n", [Tasks * 1000 div Elapsed]);
        false ->
            io:format("吞吐量: 太快无法测量~n")
    end.

worker(TaskCh, Collector) ->
    TaskCh ! {recv, self()},
    receive
        {item, Val} ->
            Collector ! Val * 2,
            worker(TaskCh, Collector);
        closed ->
            ok
    end.

task_broker(Queue, Waiters, Closed) ->
    receive
        {send, Item} ->
            case Waiters of
                [W | Rest] ->
                    W ! {item, Item},
                    task_broker(Queue, Rest, Closed);
                [] ->
                    task_broker(queue:in(Item, Queue), [], Closed)
            end;
        {recv, Consumer} ->
            case queue:out(Queue) of
                {{value, Item}, Q2} ->
                    Consumer ! {item, Item},
                    task_broker(Q2, Waiters, Closed);
                {empty, _} when Closed =:= 1 ->
                    Consumer ! closed,
                    task_broker(Queue, Waiters, Closed);
                {empty, _} ->
                    task_broker(Queue, Waiters ++ [Consumer], Closed)
            end;
        close ->
            [W ! closed || W <- Waiters],
            task_broker(Queue, [], 1)
    end.

collect_results(0, Sum) ->
    Sum;
collect_results(N, Sum) ->
    receive Val ->
        collect_results(N - 1, Sum + Val)
    end.
