// 作者：xingleixu@gmail.com
// 扇出扇入测试（Fan-out/Fan-in）
// 一个分发器将任务分发给 N 个工作 goroutine，然后收集结果

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func main() {
	N := 1000       // 工作协程数量
	TASKS := 100000 // 任务数量

	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}
	if len(os.Args) > 2 {
		if t, err := strconv.Atoi(os.Args[2]); err == nil {
			TASKS = t
		}
	}

	fmt.Println("=== Fanout 测试 ===")
	fmt.Println("工作协程:", N)
	fmt.Println("任务数量:", TASKS)

	taskCh := make(chan int, 100)
	resultCh := make(chan int, 100)

	var wg sync.WaitGroup

	start := time.Now()

	// 启动工作协程
	for i := 0; i < N; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for task := range taskCh {
				// 模拟计算
				result := task * 2
				resultCh <- result
			}
		}(i)
	}

	// 启动收集器
	var sum int64
	done := make(chan bool)
	go func() {
		for i := 0; i < TASKS; i++ {
			sum += int64(<-resultCh)
		}
		done <- true
	}()

	// 分发任务
	for i := 0; i < TASKS; i++ {
		taskCh <- i
	}

	// 关闭任务队列
	close(taskCh)

	// 等待工作协程完成
	wg.Wait()

	// 等待收集器完成
	<-done

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	// 预期结果: (0 + 1 + 2 + ... + (TASKS-1)) * 2 = TASKS * (TASKS - 1)
	expected := int64(TASKS) * int64(TASKS-1)

	fmt.Println("结果:", sum)
	fmt.Println("预期:", expected)
	fmt.Println("正确:", sum == expected)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(TASKS)/elapsed.Seconds()), "tasks/sec")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
	}
}
