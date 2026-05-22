%% Sleep storm benchmark - Erlang equivalent
%% N processes sleep simultaneously, measure scheduling overhead
%% Usage: erl -noshell -s sleep_storm main -s init stop
-module(sleep_storm).
-export([main/0, main/2]).

main() ->
    main(10000, 100).

main(N, SleepMs) ->
    io:format("=== Sleep Storm 测试 (Erlang) ===~n"),
    io:format("协程数量: ~p~n", [N]),
    io:format("Sleep时间: ~p ms~n", [SleepMs]),

    Self = self(),
    T0 = erlang:monotonic_time(millisecond),

    [spawn(fun() ->
        Before = erlang:monotonic_time(millisecond),
        timer:sleep(SleepMs),
        After = erlang:monotonic_time(millisecond),
        Self ! {done, After - Before}
    end) || _ <- lists:seq(1, N)],

    SpawnTime = erlang:monotonic_time(millisecond) - T0,
    io:format("创建时间: ~p ms~n", [SpawnTime]),

    {TotalSleep, MinSleep, MaxSleep} = collect_sleep(N, 0, 999999, 0),

    Elapsed = erlang:monotonic_time(millisecond) - T0,
    io:format("总时间: ~p ms~n", [Elapsed]),
    io:format("平均sleep: ~p ms~n", [TotalSleep div N]),
    io:format("最小sleep: ~p ms~n", [MinSleep]),
    io:format("最大sleep: ~p ms~n", [MaxSleep]),
    io:format("调度开销: ~p ms~n", [MaxSleep - SleepMs]).

collect_sleep(0, Total, Min, Max) ->
    {Total, Min, Max};
collect_sleep(N, Total, Min, Max) ->
    receive {done, Actual} ->
        NewMin = case Actual < Min of true -> Actual; false -> Min end,
        NewMax = case Actual > Max of true -> Actual; false -> Max end,
        collect_sleep(N - 1, Total + Actual, NewMin, NewMax)
    end.
