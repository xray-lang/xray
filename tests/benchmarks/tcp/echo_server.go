package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"sync"
	"time"
)

func serveMessage(conn net.Conn) {
	defer conn.Close()
	buf := make([]byte, 65536)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			if err != io.EOF {
				// read error, silently close
			}
			return
		}
		written := 0
		for written < n {
			w, err := conn.Write(buf[written:n])
			if err != nil {
				return
			}
			written += w
		}
	}
}

func serveDiscard(conn net.Conn) {
	defer conn.Close()
	buf := make([]byte, 65536)
	for {
		if _, err := conn.Read(buf); err != nil {
			if err == io.EOF {
				conn.Write([]byte("OK"))
			}
			return
		}
	}
}

func serveSlowDiscard(conn net.Conn, delayMs int64) {
	defer conn.Close()
	buf := make([]byte, 4096)
	delay := time.Duration(delayMs) * time.Millisecond
	for {
		if _, err := conn.Read(buf); err != nil {
			if err == io.EOF {
				conn.Write([]byte("OK"))
			}
			return
		}
		if delay > 0 {
			time.Sleep(delay)
		}
	}
}

func serveSource(conn net.Conn, totalBytes int64) {
	defer conn.Close()
	chunk := make([]byte, 65536)
	for i := range chunk {
		chunk[i] = 'S'
	}
	remaining := totalBytes
	for remaining > 0 {
		data := chunk
		if remaining < int64(len(chunk)) {
			data = chunk[:int(remaining)]
		}
		n, err := conn.Write(data)
		if err != nil || n <= 0 {
			return
		}
		remaining -= int64(n)
	}
}

func serveProxy(client net.Conn, upstreamPort int) {
	upstream, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", upstreamPort))
	if err != nil {
		client.Close()
		return
	}

	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		io.Copy(upstream, client)
		upstream.Close()
		client.Close()
	}()
	go func() {
		defer wg.Done()
		io.Copy(client, upstream)
		client.Close()
		upstream.Close()
	}()
	wg.Wait()
}

func handleConn(conn net.Conn, mode string, extra int64) {
	switch mode {
	case "stream", "message", "idle":
		serveMessage(conn)
	case "discard":
		serveDiscard(conn)
	case "slow_discard":
		serveSlowDiscard(conn, extra)
	case "source":
		serveSource(conn, extra)
	case "proxy":
		serveProxy(conn, int(extra))
	default:
		serveMessage(conn)
	}
}

func main() {
	port := 9001
	if len(os.Args) > 1 {
		if p, err := strconv.Atoi(os.Args[1]); err == nil {
			port = p
		}
	}
	mode := "message"
	if len(os.Args) > 2 {
		mode = os.Args[2]
	}
	extra := int64(64 * 1024 * 1024)
	if len(os.Args) > 3 {
		if v, err := strconv.ParseInt(os.Args[3], 10, 64); err == nil {
			extra = v
		}
	}

	addr := fmt.Sprintf(":%d", port)
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "ERROR: %v\n", err)
		os.Exit(1)
	}
	defer listener.Close()

	fmt.Printf("Go TCP benchmark server listening on port %d mode=%s\n", port, mode)

	for {
		conn, err := listener.Accept()
		if err != nil {
			continue
		}
		go handleConn(conn, mode, extra)
	}
}
