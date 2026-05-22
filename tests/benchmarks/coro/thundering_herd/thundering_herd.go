// 作者：xingleixu@gmail.com
// 惊群测试 (Thundering Herd)
// N 个 goroutine 同时等待同一个 channel，然后一次性全部唤醒

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func main() {
	N := 10000
	ROUNDS := 10
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			ROUNDS = n
		}
	}

	fmt.Println("=== Thundering Herd 测试 ===")
	fmt.Println("协程数量:", N)
	fmt.Println("轮数:", ROUNDS)

	start := time.Now()
	totalWoken := 0

	for r := 0; r < ROUNDS; r++ {
		signalCh := make(chan int, N)
		var wg sync.WaitGroup

		for i := 0; i < N; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				<-signalCh
			}()
		}

		// Brief pause to ensure all goroutines are blocked
		time.Sleep(1 * time.Millisecond)

		// Wake all at once
		for i := 0; i < N; i++ {
			signalCh <- i
		}

		wg.Wait()
		close(signalCh)
		totalWoken += N
	}

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Println("总唤醒数:", totalWoken)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(totalWoken)/elapsed.Seconds()), "wakeups/sec")
		fmt.Println("单次唤醒:", int(float64(elapsed.Nanoseconds())/float64(totalWoken)), "ns")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
		fmt.Println("单次唤醒: 太快无法测量")
	}
}
