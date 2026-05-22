%% Pingpong benchmark - Erlang equivalent
%% Two processes exchange plain integer messages N times
%% Usage: erl -noshell -s pingpong main -s init stop
-module(pingpong).
-export([main/0, main/1]).

main() ->
    main(1000000).

main(N) ->
    io:format("=== Pingpong 测试 (Erlang) ===~n"),
    io:format("消息次数: ~p~n", [N]),

    Self = self(),
    T0 = erlang:monotonic_time(millisecond),

    Pong = spawn(fun() ->
        receive {start, PingPid} -> pong_loop(N, PingPid) end
    end),

    Ping = spawn(fun() -> ping_loop(N, Pong, Self) end),

    Pong ! {start, Ping},

    receive done -> ok end,

    T1 = erlang:monotonic_time(millisecond),
    Elapsed = T1 - T0,
    io:format("总时间: ~p ms~n", [Elapsed]),
    io:format("消息总数: ~p~n", [N * 2]),
    case Elapsed > 0 of
        true ->
            io:format("吞吐量: ~p msg/sec~n", [N * 2 * 1000 div Elapsed]),
            io:format("单次切换: ~p ns~n", [Elapsed * 1000000 div (N * 2)]);
        false ->
            io:format("吞吐量: 太快无法测量~n"),
            io:format("单次切换: 太快无法测量~n")
    end.

ping_loop(0, _Pong, Parent) ->
    Parent ! done;
ping_loop(N, Pong, Parent) ->
    Pong ! N,
    receive _ -> ping_loop(N - 1, Pong, Parent) end.

pong_loop(0, _Ping) ->
    ok;
pong_loop(N, Ping) ->
    receive Val ->
        Ping ! Val,
        pong_loop(N - 1, Ping)
    end.
