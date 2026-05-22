// 作者：xingleixu@gmail.com
// 协程创建性能测试
// 测试大量 goroutine 的创建和销毁速度

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func main() {
	N := 1000000 // 默认100万协程

	// 从命令行读取参数
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}

	fmt.Println("=== 协程创建测试 ===")
	fmt.Println("协程数量:", N)

	var wg sync.WaitGroup
	wg.Add(N)

	start := time.Now()

	// 创建N个goroutine
	for i := 0; i < N; i++ {
		go func() {
			// 什么都不做，立即返回
			wg.Done()
		}()
	}

	// 等待所有协程完成
	wg.Wait()
	totalTime := time.Since(start)
	totalMs := float64(totalTime) / float64(time.Millisecond)
	fmt.Printf("总时间: %.3f ms\n", totalMs)
	if totalTime > 0 {
		fmt.Println("速度:", int(float64(N)/totalTime.Seconds()), "协程/秒")
	} else {
		fmt.Println("速度: 太快无法测量")
	}
}
