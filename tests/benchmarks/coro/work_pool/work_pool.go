// 作者：xingleixu@gmail.com
// 动态任务池测试 (Work Pool)
// 固定 N 个 worker 从共享队列取任务执行

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

func fib(n int) int {
	if n <= 1 {
		return n
	}
	a, b := 0, 1
	for i := 2; i <= n; i++ {
		a, b = b, a+b
	}
	return b
}

func main() {
	WORKERS := 8
	TASKS := 100000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			WORKERS = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			TASKS = n
		}
	}

	fmt.Println("=== Work Pool 测试 ===")
	fmt.Println("Worker数:", WORKERS)
	fmt.Println("任务总数:", TASKS)

	taskCh := make(chan int, 100)
	resultCh := make(chan int, 100)
	workerLoads := make([]int64, WORKERS)

	start := time.Now()

	var wg sync.WaitGroup
	for i := 0; i < WORKERS; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			var processed int64
			for task := range taskCh {
				result := fib(task % 20)
				resultCh <- result
				processed++
			}
			atomic.StoreInt64(&workerLoads[id], processed)
		}(i)
	}

	// Submit tasks
	go func() {
		for i := 0; i < TASKS; i++ {
			taskCh <- i
		}
		close(taskCh)
	}()

	// Collect results
	total := 0
	for i := 0; i < TASKS; i++ {
		total += <-resultCh
	}

	wg.Wait()
	close(resultCh)
	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Println("结果校验和:", total)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(TASKS)/elapsed.Seconds()), "tasks/sec")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
	}

	// Print load distribution
	var minLoad, maxLoad int64
	minLoad = int64(TASKS)
	for _, load := range workerLoads {
		if load < minLoad {
			minLoad = load
		}
		if load > maxLoad {
			maxLoad = load
		}
	}
	fmt.Printf("负载均衡: min=%d max=%d ratio=%.3f\n", minLoad, maxLoad, float64(maxLoad)/float64(minLoad+1))
}
