package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
)

func handleConn(conn net.Conn) {
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

func main() {
	port := 9001
	if len(os.Args) > 1 {
		if p, err := strconv.Atoi(os.Args[1]); err == nil {
			port = p
		}
	}

	addr := fmt.Sprintf(":%d", port)
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "ERROR: %v\n", err)
		os.Exit(1)
	}
	defer listener.Close()

	fmt.Printf("Go TCP echo server listening on port %d\n", port)

	for {
		conn, err := listener.Accept()
		if err != nil {
			continue
		}
		go handleConn(conn)
	}
}
