// echo_server.go - Go WebSocket echo server (gorilla/websocket) for benchmark.
//
// Usage:
//   go build -o echo_server_go echo_server.go && ./echo_server_go [port]
//
// Default port: 9001

//go:build ignore

package main

import (
	"fmt"
	"log"
	"net/http"
	"os"
	"runtime"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	ReadBufferSize:  65536,
	WriteBufferSize: 65536,
	CheckOrigin:     func(r *http.Request) bool { return true },
}

func echoHandler(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("Upgrade error: %v", err)
		return
	}
	defer conn.Close()

	conn.EnableWriteCompression(false)

	for {
		msgType, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}
		if err := conn.WriteMessage(msgType, msg); err != nil {
			break
		}
	}
}

func main() {
	runtime.GOMAXPROCS(runtime.NumCPU())

	port := "9001"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	fmt.Printf("Go WebSocket Echo Server (gorilla/websocket) on port %s\n", port)

	http.HandleFunc("/", echoHandler)
	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		log.Fatal("ListenAndServe: ", err)
	}
}
