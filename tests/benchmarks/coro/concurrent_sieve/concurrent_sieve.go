// 作者：xingleixu@gmail.com
// 并发素数筛 (Concurrent Sieve of Eratosthenes)
// 经典 CSP 管道模式：每个素数一个协程做过滤器

package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

func generate(ch chan<- int, limit int) {
	for i := 2; i <= limit; i++ {
		ch <- i
	}
	close(ch)
}

func filter(in <-chan int, out chan<- int, prime int) {
	for n := range in {
		if n%prime != 0 {
			out <- n
		}
	}
	close(out)
}

func estimateLimit(n int) int {
	if n < 10 {
		return 30
	}
	return n * 15
}

func main() {
	N := 10000
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			N = n
		}
	}

	fmt.Println("=== 并发素数筛测试 ===")
	fmt.Println("目标素数数量:", N)

	limit := estimateLimit(N)
	start := time.Now()

	ch := make(chan int, 100)
	go generate(ch, limit)

	primes := make([]int, 0, N)
	count := 0

	for count < N {
		prime, ok := <-ch
		if !ok {
			break
		}
		primes = append(primes, prime)
		count++

		if count < N {
			nextCh := make(chan int, 100)
			go filter(ch, nextCh, prime)
			ch = nextCh
		}
	}
	if count >= N {
		for range ch {
		}
	}

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Println("找到素数:", count)
	fmt.Println("最大素数:", primes[len(primes)-1])
	fmt.Println("管道级数:", count)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("速度:", int(float64(count)/elapsed.Seconds()), "primes/sec")
	} else {
		fmt.Println("速度: 太快无法测量")
	}
}
