// 作者：xingleixu@gmail.com
// Sleep Storm 测试
// 大量 goroutine 同时 sleep 后唤醒，测试定时器和调度效率

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func main() {
	N := 10000      // 协程数量
	SLEEP_MS := 100 // sleep 时间（毫秒）

	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}
	if len(os.Args) > 2 {
		if s, err := strconv.Atoi(os.Args[2]); err == nil {
			SLEEP_MS = s
		}
	}

	fmt.Println("=== Sleep Storm 测试 ===")
	fmt.Println("协程数量:", N)
	fmt.Println("Sleep时间:", SLEEP_MS, "ms")

	sleepDuration := time.Duration(SLEEP_MS) * time.Millisecond
	results := make([]float64, N)
	var wg sync.WaitGroup

	start := time.Now()

	// 同时启动所有 goroutine
	for i := 0; i < N; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			before := time.Now()
			time.Sleep(sleepDuration)
			after := time.Now()
			results[idx] = float64(after.Sub(before)) / float64(time.Millisecond)
		}(i)
	}

	spawnTime := time.Since(start)
	spawnMs := float64(spawnTime) / float64(time.Millisecond)
	fmt.Printf("创建时间: %.3f ms\n", spawnMs)

	// 等待所有协程完成
	wg.Wait()

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	// 统计
	var totalSleep float64
	var maxSleep float64 = 0
	var minSleep float64 = 999999

	for _, actual := range results {
		totalSleep += actual
		if actual > maxSleep {
			maxSleep = actual
		}
		if actual < minSleep {
			minSleep = actual
		}
	}

	fmt.Printf("总时间: %.3f ms\n", elapsedMs)
	fmt.Printf("平均sleep: %.3f ms\n", totalSleep/float64(N))
	fmt.Printf("最小sleep: %.3f ms\n", minSleep)
	fmt.Printf("最大sleep: %.3f ms\n", maxSleep)
	fmt.Printf("调度开销: %.3f ms\n", maxSleep-float64(SLEEP_MS))
}
