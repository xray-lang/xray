// 作者：xingleixu@gmail.com
// Chameneos-Redux (CLBG 经典并发 benchmark)
// N 个变色龙在"会合点"两两配对交换颜色

package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

type Color int

const (
	Blue   Color = 0
	Red    Color = 1
	Yellow Color = 2
)

func complement(c1, c2 Color) Color {
	if c1 == c2 {
		return c1
	}
	switch {
	case c1 == Blue && c2 == Red:
		return Yellow
	case c1 == Blue && c2 == Yellow:
		return Red
	case c1 == Red && c2 == Blue:
		return Yellow
	case c1 == Red && c2 == Yellow:
		return Blue
	case c1 == Yellow && c2 == Blue:
		return Red
	case c1 == Yellow && c2 == Red:
		return Blue
	}
	return c1
}

type MeetRequest struct {
	id    int
	color Color
	reply chan Color
}

// broker: pairs up chameneos for N meetings, then signals done
// Mirrors xray: broker closes request_ch, then drains stale requests
func broker(requestCh chan MeetRequest, meetings int) {
	for i := 0; i < meetings; i++ {
		first := <-requestCh
		second := <-requestCh
		first.reply <- second.color
		second.reply <- first.color
	}
	// Close requestCh so chameneo's send will panic (caught by recover)
	close(requestCh)
	// Drain any already-buffered requests and close their reply channels
	// so those chameneos' recv() returns zero-value + !ok
	for {
		select {
		case req, ok := <-requestCh:
			if !ok {
				return
			}
			close(req.reply)
		default:
			return
		}
	}
}

// safeSend attempts to send on requestCh; returns false if channel is closed
// Mirrors xray: try { request_ch.send(...) } catch (e) { break }
func safeSend(requestCh chan<- MeetRequest, req MeetRequest) (ok bool) {
	defer func() {
		if recover() != nil {
			ok = false
		}
	}()
	requestCh <- req
	return true
}

func chameneo(id int, initialColor Color, requestCh chan<- MeetRequest, wg *sync.WaitGroup, results chan<- int) {
	defer wg.Done()
	color := initialColor
	meetings := 0
	reply := make(chan Color, 1)

	for {
		if !safeSend(requestCh, MeetRequest{id: id, color: color, reply: reply}) {
			break // channel closed by broker
		}
		partnerColor, ok := <-reply
		if !ok {
			break
		}
		color = complement(color, partnerColor)
		meetings++
	}

	results <- meetings
}

func main() {
	nMeetings := 600000
	nChameneos := 4
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			nMeetings = n
		}
	}
	if len(os.Args) > 2 {
		if n, err := strconv.Atoi(os.Args[2]); err == nil {
			nChameneos = n
		}
	}

	fmt.Println("=== Chameneos-Redux 测试 ===")
	fmt.Println("会合次数:", nMeetings)
	fmt.Println("变色龙数:", nChameneos)

	start := time.Now()

	requestCh := make(chan MeetRequest, 1)
	results := make(chan int, nChameneos)

	var wg sync.WaitGroup
	for i := 0; i < nChameneos; i++ {
		wg.Add(1)
		go chameneo(i, Color(i%3), requestCh, &wg, results)
	}

	broker(requestCh, nMeetings)
	wg.Wait()

	totalMeetings := 0
	for i := 0; i < nChameneos; i++ {
		m := <-results
		totalMeetings += m
	}

	elapsed := time.Since(start)
	elapsedMs := float64(elapsed) / float64(time.Millisecond)

	fmt.Println("总会合次数:", totalMeetings)
	fmt.Println("预期会合次数:", nMeetings*2)
	fmt.Printf("时间: %.3f ms\n", elapsedMs)
	if elapsed > 0 {
		fmt.Println("吞吐量:", int(float64(totalMeetings)/elapsed.Seconds()), "meetings/sec")
	} else {
		fmt.Println("吞吐量: 太快无法测量")
	}
}
