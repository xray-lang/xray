%% Ring benchmark - Erlang equivalent
%% N processes in a ring, message passes M rounds
%% Main process acts as node[0], N-1 spawned nodes form the rest
%% Plain integer messages for fair comparison with xray/Go
%% Usage: erl -noshell -s ring main -s init stop
-module(ring).
-export([main/0, main/2]).

main() ->
    main(1000, 1000).

main(N, M) ->
    io:format("=== Ring 测试 (Erlang) ===~n"),
    io:format("协程数量: ~p~n", [N]),
    io:format("消息轮数: ~p~n", [M]),

    T0 = erlang:monotonic_time(millisecond),

    Self = self(),
    %% Create N-1 nodes: node[1]..node[N-1], node[N-1] sends back to Self
    Node1 = create_chain(N - 1, M, Self),

    %% Inject initial message
    self() ! 0,

    %% Self acts as node[0]: M rounds of recv + send
    node0_loop(M, Node1),

    %% After M rounds, one more message arrives from node[N-1]
    Result = receive FinalVal -> FinalVal end,

    T1 = erlang:monotonic_time(millisecond),
    Elapsed = T1 - T0,
    TotalMessages = N * M,
    io:format("最终结果: ~p~n", [Result]),
    io:format("总消息数: ~p~n", [TotalMessages]),
    io:format("总时间: ~p ms~n", [Elapsed]),
    case Elapsed > 0 of
        true ->
            io:format("吞吐量: ~p msg/sec~n", [TotalMessages * 1000 div Elapsed]);
        false ->
            io:format("吞吐量: 太快无法测量~n")
    end.

%% Create K nodes backwards: last sends to Target, returns first Pid
create_chain(1, M, Next) ->
    spawn(fun() -> ring_node(M, Next) end);
create_chain(K, M, Next) ->
    NewNext = spawn(fun() -> ring_node(M, Next) end),
    create_chain(K - 1, M, NewNext).

ring_node(0, _Next) ->
    ok;
ring_node(Rounds, Next) ->
    receive Val ->
        Next ! (Val + 1),
        ring_node(Rounds - 1, Next)
    end.

node0_loop(0, _Next) ->
    ok;
node0_loop(Rounds, Next) ->
    receive Val ->
        Next ! (Val + 1),
        node0_loop(Rounds - 1, Next)
    end.
