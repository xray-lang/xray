// 作者：xingleixu@gmail.com
// 协程切换性能测试（pingpong）
// 两个 goroutine 通过 channel 互相发送消息，测试切换延迟

package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func main() {
	N := 1000000 // 默认100万次

	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}

	fmt.Println("=== Pingpong 测试 ===")
	fmt.Println("消息次数:", N)

	ch1 := make(chan int, 1)
	ch2 := make(chan int, 1)
	done := make(chan bool)

	start := time.Now()

	// Ping goroutine
	go func() {
		for i := 0; i < N; i++ {
			ch1 <- i
			<-ch2
		}
		done <- true
	}()

	// Pong goroutine
	go func() {
		for i := 0; i < N; i++ {
			msg := <-ch1
			ch2 <- msg
		}
		done <- true
	}()

	<-done
	<-done

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Printf("总时间: %.3f ms\n", elapsedMs)
	fmt.Println("消息总数:", N*2)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(N*2)/elapsed.Seconds()), "msg/sec")
		fmt.Println("单次切换:", int(float64(elapsed.Nanoseconds())/float64(N*2)), "ns")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
		fmt.Println("单次切换: 太快无法测量")
	}
}
