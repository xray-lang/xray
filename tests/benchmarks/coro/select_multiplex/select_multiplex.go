// 作者：xingleixu@gmail.com
// Select 多路复用测试
// N 个生产者向各自 channel 发送数据，1 个消费者用 select 接收

package main

import (
	"fmt"
	"os"
	"reflect"
	"strconv"
	"time"
)

func producer(ch chan<- int, id int, count int) {
	for i := 0; i < count; i++ {
		ch <- id*1000000 + i
	}
	close(ch)
}

func main() {
	PRODUCERS := 4
	MSGS := 1000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			MSGS = n
		}
	}

	fmt.Println("=== Select 多路复用测试 ===")
	fmt.Println("生产者数量:", PRODUCERS)
	fmt.Println("每生产者消息:", MSGS)
	fmt.Println("总消息数:", PRODUCERS*MSGS)

	start := time.Now()

	channels := make([]chan int, PRODUCERS)
	for i := 0; i < PRODUCERS; i++ {
		channels[i] = make(chan int, 10)
		go producer(channels[i], i, MSGS)
	}

	// Use reflect.Select for dynamic number of channels
	totalReceived := 0
	active := PRODUCERS

	cases := make([]reflect.SelectCase, PRODUCERS)
	for i := range channels {
		cases[i] = reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(channels[i])}
	}

	for active > 0 {
		chosen, value, ok := reflect.Select(cases)
		if ok {
			_ = value.Int()
			totalReceived++
		} else {
			// Channel closed, remove it
			cases[chosen] = cases[active-1]
			cases = cases[:active-1]
			active--
		}
	}

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)
	fmt.Println("接收总数:", totalReceived)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(totalReceived)/elapsed.Seconds()), "msg/sec")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
	}
}
