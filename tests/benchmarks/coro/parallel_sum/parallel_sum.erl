%% Parallel sum benchmark - Erlang equivalent
%% Split range into N chunks, compute partial sums in parallel
%% Usage: erl -noshell -s parallel_sum main -s init stop
-module(parallel_sum).
-export([main/0, main/2]).

main() ->
    main(8, 10000000).

main(N, Size) ->
    io:format("=== 并行求和测试 (Erlang) ===~n"),
    io:format("并行度: ~p~n", [N]),
    io:format("数组大小: ~p~n", [Size]),

    T0 = erlang:monotonic_time(millisecond),

    Self = self(),
    ChunkSize = Size div N,
    [spawn(fun() ->
        ChunkStart = I * ChunkSize,
        ChunkEnd = case I =:= N - 1 of
            true -> Size;
            false -> ChunkStart + ChunkSize
        end,
        Sum = partial_sum(ChunkStart, ChunkEnd, 0),
        Self ! {result, Sum}
    end) || I <- lists:seq(0, N - 1)],

    Total = collect_sum(N, 0),

    T1 = erlang:monotonic_time(millisecond),
    Elapsed = T1 - T0,
    Expected = Size * (Size - 1) div 2,
    io:format("结果: ~p~n", [Total]),
    io:format("预期: ~p~n", [Expected]),
    io:format("正确: ~p~n", [Total =:= Expected]),
    io:format("时间: ~p ms~n", [Elapsed]),

    %% Serial comparison
    T2 = erlang:monotonic_time(millisecond),
    _SerialSum = partial_sum(0, Size, 0),
    T3 = erlang:monotonic_time(millisecond),
    SerialElapsed = T3 - T2,
    io:format("串行时间: ~p ms~n", [SerialElapsed]),
    case Elapsed > 0 of
        true ->
            io:format("加速比: ~p~n", [SerialElapsed / Elapsed]);
        false ->
            io:format("加速比: 太快无法测量~n")
    end.

partial_sum(End, End, Acc) ->
    Acc;
partial_sum(I, End, Acc) ->
    partial_sum(I + 1, End, Acc + I).

collect_sum(0, Sum) ->
    Sum;
collect_sum(N, Sum) ->
    receive {result, Val} ->
        collect_sum(N - 1, Sum + Val)
    end.
