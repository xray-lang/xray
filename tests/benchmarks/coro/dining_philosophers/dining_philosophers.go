// 作者：xingleixu@gmail.com
// 哲学家就餐测试 (Dining Philosophers)
// N 个哲学家围坐圆桌，用 channel 模拟叉子

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func philosopher(id, meals int, left, right chan int, wg *sync.WaitGroup, results chan<- int) {
	defer wg.Done()
	first, second := left, right
	if id == cap(results)-1 { // last philosopher reverses order
		first, second = right, left
	}

	for i := 0; i < meals; i++ {
		<-first
		<-second
		// eating (no-op)
		second <- 1
		first <- 1
	}
	results <- meals
}

func main() {
	N := 5
	MEALS := 100000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			MEALS = n
		}
	}

	fmt.Println("=== Dining Philosophers 测试 ===")
	fmt.Println("哲学家数:", N)
	fmt.Println("每人进餐:", MEALS)

	forks := make([]chan int, N)
	for i := range forks {
		forks[i] = make(chan int, 1)
		forks[i] <- 1
	}

	results := make(chan int, N)
	start := time.Now()

	var wg sync.WaitGroup
	for i := 0; i < N; i++ {
		wg.Add(1)
		left := forks[i]
		right := forks[(i+1)%N]
		first, second := left, right
		if i == N-1 {
			first, second = right, left
		}
		go func(id int, f, s chan int) {
			defer wg.Done()
			for j := 0; j < MEALS; j++ {
				<-f
				<-s
				s <- 1
				f <- 1
			}
			results <- MEALS
		}(i, first, second)
	}

	totalMeals := 0
	for i := 0; i < N; i++ {
		totalMeals += <-results
	}

	wg.Wait()
	close(results)
	for _, fork := range forks {
		close(fork)
	}
	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Println("总进餐次数:", totalMeals)
	fmt.Println("预期:", N*MEALS)
	fmt.Println("正确:", totalMeals == N*MEALS)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(totalMeals)/elapsed.Seconds()), "meals/sec")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
	}
}
