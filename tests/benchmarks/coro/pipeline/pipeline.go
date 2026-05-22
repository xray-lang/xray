// 作者：xingleixu@gmail.com
// 多级管道测试 (Pipeline)
// 数据流经 N 级处理阶段，每级一个 goroutine

package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func stage(in <-chan int, out chan<- int) {
	for val := range in {
		out <- val + 1
	}
	close(out)
}

func main() {
	STAGES := 100
	ITEMS := 100000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			STAGES = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			ITEMS = n
		}
	}

	fmt.Println("=== Pipeline 测试 ===")
	fmt.Println("管道级数:", STAGES)
	fmt.Println("数据项数:", ITEMS)

	start := time.Now()

	// Build pipeline
	firstCh := make(chan int, 10)
	prevCh := firstCh
	for i := 0; i < STAGES; i++ {
		nextCh := make(chan int, 10)
		go stage(prevCh, nextCh)
		prevCh = nextCh
	}
	lastCh := prevCh

	// Producer
	go func() {
		for i := 0; i < ITEMS; i++ {
			firstCh <- i
		}
		close(firstCh)
	}()

	// Consumer
	sum := 0
	count := 0
	for val := range lastCh {
		sum += val
		count++
	}

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	expected := ITEMS*(ITEMS-1)/2 + ITEMS*STAGES
	fmt.Println("接收数量:", count)
	fmt.Println("结果:", sum)
	fmt.Println("预期:", expected)
	fmt.Println("正确:", sum == expected)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(count)/elapsed.Seconds()), "items/sec")
		fmt.Printf("每项延迟: %d ns (%d stages)\n", int(float64(elapsed.Nanoseconds())/float64(count)), STAGES)
	} else {
		fmt.Println("吞吐量: 太快无法测量")
		fmt.Println("每项延迟: 太快无法测量")
	}
}
