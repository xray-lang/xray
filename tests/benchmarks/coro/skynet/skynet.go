// 作者：xingleixu@gmail.com
// Skynet 基准测试
// 经典的协程性能测试，创建树形结构的 goroutine
// 每个节点创建10个子节点，叶节点返回自己的序号
// 最终汇总所有叶节点的值

package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func skynet(c chan int64, num int64, size int64, div int64) {
	if size == 1 {
		c <- num
		return
	}

	rc := make(chan int64, div)
	childSize := size / div

	for i := int64(0); i < div; i++ {
		childNum := num + i*childSize
		go skynet(rc, childNum, childSize, div)
	}

	var sum int64
	for i := int64(0); i < div; i++ {
		sum += <-rc
	}

	c <- sum
}

func calcTotal(depth int) int64 {
	var total int64 = 0
	var n int64 = 1
	for i := 0; i <= depth; i++ {
		total += n
		n *= 10
	}
	return total
}

func main() {
	depth := 6 // 深度6 = 10^6 = 100万协程

	if len(os.Args) > 1 {
		if d, err := strconv.Atoi(os.Args[1]); err == nil {
			depth = d
		}
	}

	fmt.Println("=== Skynet 测试 ===")
	fmt.Println("深度:", depth)
	fmt.Println("预计协程数:", calcTotal(depth))

	var size int64 = 1
	for i := 0; i < depth; i++ {
		size *= 10
	}

	c := make(chan int64, 1)

	start := time.Now()

	go skynet(c, 0, size, 10)
	result := <-c

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	// 预期结果: 0 + 1 + 2 + ... + (size-1) = size * (size - 1) / 2
	expected := size * (size - 1) / 2

	fmt.Println("结果:", result)
	fmt.Println("预期:", expected)
	fmt.Println("正确:", result == expected)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	fmt.Println("协程数:", calcTotal(depth))
}
