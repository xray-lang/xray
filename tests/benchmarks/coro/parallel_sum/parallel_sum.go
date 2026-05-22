// 作者：xingleixu@gmail.com
// 并行求和测试
// 将大数组分成 N 份，并行计算部分和，最后汇总

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func partialSum(start, end int64) int64 {
	var sum int64
	for i := start; i < end; i++ {
		sum += i
	}
	return sum
}

func main() {
	N := 8                  // 并行度
	SIZE := int64(10000000) // 数组大小（1000万）

	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}
	if len(os.Args) > 2 {
		if s, err := strconv.ParseInt(os.Args[2], 10, 64); err == nil {
			SIZE = s
		}
	}

	fmt.Println("=== 并行求和测试 ===")
	fmt.Println("并行度:", N)
	fmt.Println("数组大小:", SIZE)

	startTime := time.Now()

	// 并行计算
	chunkSize := SIZE / int64(N)
	results := make([]int64, N)
	var wg sync.WaitGroup

	for i := 0; i < N; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			chunkStart := int64(idx) * chunkSize
			chunkEnd := chunkStart + chunkSize
			if idx == N-1 {
				chunkEnd = SIZE // 最后一个处理剩余部分
			}
			results[idx] = partialSum(chunkStart, chunkEnd)
		}(i)
	}

	wg.Wait()

	// 汇总结果
	var total int64
	for _, r := range results {
		total += r
	}

	elapsed := time.Since(startTime)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	// 预期结果: 0 + 1 + 2 + ... + (SIZE-1) = SIZE * (SIZE - 1) / 2
	expected := SIZE * (SIZE - 1) / 2

	fmt.Println("结果:", total)
	fmt.Println("预期:", expected)
	fmt.Println("正确:", total == expected)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)

	// 串行对比
	serialStart := time.Now()
	serialSum := partialSum(0, SIZE)
	serialElapsed := time.Since(serialStart)
	serialElapsedMs := float64(serialElapsed) / float64(time.Millisecond)

	fmt.Printf("串行时间: %.3f ms\n", serialElapsedMs)
	fmt.Println("串行结果:", serialSum)
	if elapsed > 0 {
		fmt.Printf("加速比: %.2f\n", float64(serialElapsed)/float64(elapsed))
	} else {
		fmt.Println("加速比: 太快无法测量")
	}
}
