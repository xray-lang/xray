%% Spawn benchmark - Erlang equivalent
%% Create N processes and wait for all to finish
%% Usage: erl -noshell +P 2000000 -s spawn main -s init stop
%% Note: Erlang default process limit is 262144; use +P 2000000 for 1M processes
-module(spawn).
-export([main/0, main/1]).

main() ->
    main(1000000).

main(N) ->
    io:format("=== 协程创建测试 (Erlang) ===~n"),
    io:format("协程数量: ~p~n", [N]),

    BatchSize = 10000,
    T0 = erlang:monotonic_time(millisecond),

    spawn_batched(N, BatchSize, self()),

    T1 = erlang:monotonic_time(millisecond),
    Elapsed = T1 - T0,
    io:format("总时间: ~p ms~n", [Elapsed]),
    case Elapsed > 0 of
        true ->
            io:format("速度: ~p 协程/秒~n", [N * 1000 div Elapsed]);
        false ->
            io:format("速度: 太快无法测量~n")
    end.

spawn_batched(0, _BatchSize, _Parent) ->
    ok;
spawn_batched(Remaining, BatchSize, Parent) ->
    Batch = min(Remaining, BatchSize),
    spawn_n(Batch, Parent),
    wait_n(Batch),
    spawn_batched(Remaining - Batch, BatchSize, Parent).

spawn_n(0, _Parent) ->
    ok;
spawn_n(N, Parent) ->
    erlang:spawn(fun() -> Parent ! done end),
    spawn_n(N - 1, Parent).

wait_n(0) ->
    ok;
wait_n(N) ->
    receive
        done -> wait_n(N - 1)
    end.
