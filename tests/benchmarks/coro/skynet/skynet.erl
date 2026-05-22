%% Skynet benchmark - Erlang equivalent
%% Classic coroutine tree benchmark: each node spawns 10 children
%% Leaf nodes return their sequence number, intermediate nodes sum children
%% Usage: erl -noshell -s skynet main -s init stop
-module(skynet).
-export([main/0, main/1]).

main() ->
    main(6).

main(Depth) ->
    Size = trunc(math:pow(10, Depth)),
    TotalCoros = calc_total(Depth),
    io:format("=== Skynet 测试 (Erlang) ===~n"),
    io:format("深度: ~p~n", [Depth]),
    io:format("预计协程数: ~p~n", [TotalCoros]),

    T0 = erlang:monotonic_time(millisecond),
    Result = skynet(0, Size, 10),
    T1 = erlang:monotonic_time(millisecond),

    Elapsed = T1 - T0,
    Expected = Size * (Size - 1) div 2,
    io:format("结果: ~p~n", [Result]),
    io:format("预期: ~p~n", [Expected]),
    io:format("正确: ~p~n", [Result =:= Expected]),
    io:format("时间: ~p ms~n", [Elapsed]),
    io:format("协程数: ~p~n", [TotalCoros]).

skynet(Num, 1, _Div) ->
    Num;
skynet(Num, Size, Div) ->
    ChildSize = Size div Div,
    Parent = self(),
    [spawn(fun() ->
        R = skynet(Num + I * ChildSize, ChildSize, Div),
        Parent ! R
    end) || I <- lists:seq(0, Div - 1)],
    collect_sum(Div, 0).

collect_sum(0, Sum) ->
    Sum;
collect_sum(N, Sum) ->
    receive Val ->
        collect_sum(N - 1, Sum + Val)
    end.

calc_total(Depth) ->
    calc_total(Depth, 0, 1).
calc_total(-1, Total, _N) ->
    Total;
calc_total(D, Total, N) ->
    calc_total(D - 1, Total + N, N * 10).
