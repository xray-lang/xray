package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func parseArg(index int, fallback int) int {
	if len(os.Args) <= index {
		return fallback
	}
	if value, err := strconv.Atoi(os.Args[index]); err == nil {
		return value
	}
	return fallback
}

func recvTimeoutWorker(ch <-chan int, timeout time.Duration) bool {
	select {
	case <-ch:
		return false
	case <-time.After(timeout):
		return true
	}
}

func sendTimeoutWorker(ch chan<- int, timeout time.Duration, value int) bool {
	select {
	case ch <- value:
		return false
	case <-time.After(timeout):
		return true
	}
}

func runRecvStorm(count int, timeout time.Duration) int {
	ch := make(chan int, 1)
	results := make(chan bool, count)
	var wg sync.WaitGroup
	wg.Add(count)
	for i := 0; i < count; i++ {
		go func() {
			defer wg.Done()
			results <- recvTimeoutWorker(ch, timeout)
		}()
	}
	wg.Wait()
	close(results)
	close(ch)

	timeouts := 0
	for timedOut := range results {
		if timedOut {
			timeouts++
		}
	}
	return timeouts
}

func runSendStorm(count int, timeout time.Duration) int {
	ch := make(chan int, 1)
	ch <- 1
	results := make(chan bool, count)
	var wg sync.WaitGroup
	wg.Add(count)
	for i := 0; i < count; i++ {
		value := i + 2
		go func() {
			defer wg.Done()
			results <- sendTimeoutWorker(ch, timeout, value)
		}()
	}
	wg.Wait()
	close(results)
	<-ch
	close(ch)

	timeouts := 0
	for timedOut := range results {
		if timedOut {
			timeouts++
		}
	}
	return timeouts
}

func main() {
	count := parseArg(1, 1000)
	timeoutMs := parseArg(2, 10)
	timeout := time.Duration(timeoutMs) * time.Millisecond

	fmt.Println("=== Timeout Storm Benchmark ===")
	fmt.Println("coroutines:", count)
	fmt.Println("timeout:", timeoutMs, "ms")

	startTotal := time.Now()

	startRecv := time.Now()
	recvTimeouts := runRecvStorm(count, timeout)
	recvElapsed := time.Since(startRecv).Seconds() * 1000.0

	startSend := time.Now()
	sendTimeouts := runSendStorm(count, timeout)
	sendElapsed := time.Since(startSend).Seconds() * 1000.0

	totalElapsed := time.Since(startTotal).Seconds() * 1000.0
	completed := count * 2
	expected := count * 2
	timedOut := recvTimeouts + sendTimeouts

	fmt.Println("recv completed:", count)
	fmt.Println("recv timeouts:", recvTimeouts)
	fmt.Printf("recv wall: %.3f ms\n", recvElapsed)
	fmt.Println("send completed:", count)
	fmt.Println("send timeouts:", sendTimeouts)
	fmt.Printf("send wall: %.3f ms\n", sendElapsed)
	fmt.Println("completed_ops:", completed)
	fmt.Println("expected_ops:", expected)
	fmt.Println("checksum:", timedOut)
	fmt.Println("correctness:", timedOut == expected)
	fmt.Printf("reported_time_ms: %.3f\n", totalElapsed)
}
