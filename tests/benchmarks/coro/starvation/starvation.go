// 作者：xingleixu@gmail.com
// 公平调度测试 (Starvation Test)
// 1 个 CPU 密集型 goroutine + N 个短任务
// 测试 Go runtime 的抢占调度

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

func cpuHog(stop *atomic.Bool) {
	count := 0
	for !stop.Load() {
		for i := 0; i < 1000000; i++ {
			count++
		}
	}
}

func shortTask(id int, results chan<- float64) {
	start := time.Now()
	sum := 0
	for i := 0; i < 100; i++ {
		sum += i
	}
	latency := float64(time.Since(start)) / float64(time.Millisecond)
	results <- latency
}

func main() {
	SHORT_TASKS := 100
	DURATION_MS := 2000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			SHORT_TASKS = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			DURATION_MS = n
		}
	}

	fmt.Println("=== Starvation 测试 ===")
	fmt.Println("短任务数:", SHORT_TASKS)
	fmt.Println("持续时间:", DURATION_MS, "ms")

	results := make(chan float64, 1000)
	var stop atomic.Bool

	start := time.Now()

	// Start CPU hog
	go cpuHog(&stop)
	time.Sleep(10 * time.Millisecond)

	// Launch short tasks in batches
	batchSize := SHORT_TASKS / 10
	if batchSize < 1 {
		batchSize = 1
	}

	var wg sync.WaitGroup
	launched := 0
	for launched < SHORT_TASKS {
		remaining := SHORT_TASKS - launched
		currentBatch := batchSize
		if currentBatch > remaining {
			currentBatch = remaining
		}
		for i := 0; i < currentBatch; i++ {
			wg.Add(1)
			go func(id int) {
				defer wg.Done()
				shortTask(id, results)
			}(launched + i)
		}
		launched += currentBatch
		time.Sleep(10 * time.Millisecond)
	}

	wg.Wait()
	time.Sleep(time.Duration(DURATION_MS) * time.Millisecond)
	stop.Store(true)

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	// Collect results
	close(results)
	completed := 0
	var totalLatency, maxLatency float64
	for lat := range results {
		completed++
		totalLatency += lat
		if lat > maxLatency {
			maxLatency = lat
		}
	}

	fmt.Println("短任务完成:", completed, "/", SHORT_TASKS)
	fmt.Printf("总时间: %.3f ms\n", elapsedMs)
	if completed > 0 {
		fmt.Printf("平均延迟: %.3f ms\n", totalLatency/float64(completed))
		fmt.Printf("最大延迟: %.3f ms\n", maxLatency)
	}
	fmt.Println("公平调度:", completed == SHORT_TASKS)
}
