// 作者：xingleixu@gmail.com
// 生产者消费者测试
// M 个生产者生产数据，N 个消费者消费数据

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	PRODUCERS := 10 // 生产者数量
	CONSUMERS := 10 // 消费者数量
	ITEMS := 100000 // 总生产数量

	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			PRODUCERS = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			CONSUMERS = n
		}
	}
	if len(os.Args) > 3 {
		if n, err := strconv.Atoi(os.Args[3]); err == nil {
			ITEMS = n
		}
	}

	fmt.Println("=== 生产者消费者测试 ===")
	fmt.Println("生产者:", PRODUCERS)
	fmt.Println("消费者:", CONSUMERS)
	fmt.Println("总数量:", ITEMS)

	queue := make(chan int, 100)
	var totalConsumed int64

	var producerWg sync.WaitGroup
	var consumerWg sync.WaitGroup

	itemsPerProducer := ITEMS / PRODUCERS
	remainingItems := ITEMS % PRODUCERS

	start := time.Now()

	// 启动生产者
	for i := 0; i < PRODUCERS; i++ {
		producerWg.Add(1)
		go func(id int) {
			defer producerWg.Done()
			count := itemsPerProducer
			if id < remainingItems {
				count++
			}
			for j := 0; j < count; j++ {
				queue <- id*1000000 + j
			}
		}(i)
	}

	// 等待生产者完成后关闭队列
	go func() {
		producerWg.Wait()
		close(queue)
	}()

	// 启动消费者
	for i := 0; i < CONSUMERS; i++ {
		consumerWg.Add(1)
		go func(id int) {
			defer consumerWg.Done()
			var consumed int64
			for item := range queue {
				// 模拟处理
				_ = item * 2
				consumed++
			}
			atomic.AddInt64(&totalConsumed, consumed)
		}(i)
	}

	// 等待消费者完成
	consumerWg.Wait()

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Println("消费总数:", totalConsumed)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(totalConsumed)/elapsed.Seconds()), "items/sec")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
	}
}
