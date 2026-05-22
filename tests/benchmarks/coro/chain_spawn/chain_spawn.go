// 作者：xingleixu@gmail.com
// 链式递归创建测试 (Chain Spawn)
// goroutine A spawn B, B spawn C, ... 形成深度链

package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func chain(depth int) int {
	if depth <= 0 {
		return 0
	}
	ch := make(chan int, 1)
	go func() {
		ch <- chain(depth - 1)
	}()
	return <-ch + 1
}

func main() {
	DEPTH := 100000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			DEPTH = n
		}
	}

	fmt.Println("=== Chain Spawn 测试 ===")
	fmt.Println("链深度:", DEPTH)

	start := time.Now()

	ch := make(chan int, 1)
	go func() {
		ch <- chain(DEPTH)
	}()
	result := <-ch

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Println("结果:", result)
	fmt.Println("正确:", result == DEPTH)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("速度:", int(float64(DEPTH)/elapsed.Seconds()), "spawns/sec")
	} else {
		fmt.Println("速度: 太快无法测量")
	}
}
