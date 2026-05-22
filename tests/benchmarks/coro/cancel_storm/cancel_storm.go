// 作者：xingleixu@gmail.com
// 大量取消测试 (Cancel Storm)
// 创建 N 个 goroutine 后立即取消，测试 context cancel 效率

package main

import (
	"context"
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func longWait(ctx context.Context, id int) int {
	select {
	case <-ctx.Done():
		return -1
	case <-time.After(60 * time.Second):
		return id
	}
}

func main() {
	N := 100000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}

	fmt.Println("=== Cancel Storm 测试 ===")
	fmt.Println("协程数量:", N)

	start := time.Now()

	ctx, cancel := context.WithCancel(context.Background())
	var wg sync.WaitGroup

	// Spawn all goroutines
	for i := 0; i < N; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			longWait(ctx, id)
		}(i)
	}

	spawnTime := time.Since(start)
	spawnMs := float64(spawnTime) / float64(time.Millisecond)
	fmt.Printf("创建时间: %.3f ms\n", spawnMs)

	// Cancel all at once
	cancelStart := time.Now()
	cancel()
	cancelTime := time.Since(cancelStart)
	cancelMs := float64(cancelTime) / float64(time.Millisecond)
	fmt.Printf("取消时间: %.3f ms\n", cancelMs)

	// Wait for all to finish
	awaitStart := time.Now()
	wg.Wait()
	awaitTime := time.Since(awaitStart)
	awaitMs := float64(awaitTime) / float64(time.Millisecond)

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)
	fmt.Printf("等待时间: %.3f ms\n", awaitMs)
	fmt.Printf("总时间: %.3f ms\n", elapsedMs)
	fmt.Println("成功取消:", N, "/", N)
	if elapsed > 0 {
		fmt.Println("速度:", int(float64(N)/elapsed.Seconds()), "cancels/sec")
	} else {
		fmt.Println("速度: 太快无法测量")
	}
}
