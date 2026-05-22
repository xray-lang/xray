// 作者：xingleixu@gmail.com
// 环形消息传递测试
// N 个 goroutine 组成环，消息在环中传递 M 轮

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func main() {
	N := 1000 // 协程数量
	M := 1000 // 消息轮数

	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}
	if len(os.Args) > 2 {
		if m, err := strconv.Atoi(os.Args[2]); err == nil {
			M = m
		}
	}

	fmt.Println("=== Ring 测试 ===")
	fmt.Println("协程数量:", N)
	fmt.Println("消息轮数:", M)

	// 创建 N 个 channel
	channels := make([]chan int, N)
	for i := 0; i < N; i++ {
		channels[i] = make(chan int, 1)
	}

	var wg sync.WaitGroup
	wg.Add(N)

	start := time.Now()

	// 启动所有 goroutine
	for i := 0; i < N; i++ {
		inCh := channels[i]
		outCh := channels[(i+1)%N]
		go func(id int, in, out chan int) {
			defer wg.Done()
			for r := 0; r < M; r++ {
				msg := <-in
				out <- msg + 1
			}
		}(i, inCh, outCh)
	}

	// 向第一个 channel 注入初始消息
	channels[0] <- 0

	// 等待所有协程完成
	wg.Wait()

	// 读取最后的结果
	result := <-channels[0]

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)
	totalMessages := int64(N * M)

	fmt.Println("最终结果:", result)
	fmt.Println("总消息数:", totalMessages)
	fmt.Printf("总时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(totalMessages)/elapsed.Seconds()), "msg/sec")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
	}
}
