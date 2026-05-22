// http_server.go - Go HTTP server (net/http) for benchmark.
//
// Usage:
//   go build -o http_server_go http_server.go && ./http_server_go [port]
//
// Default port: 8080

//go:build ignore

package main

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"runtime"
)

var jsonResp = []byte(`{"message":"Hello, World!"}`)
var plainResp = []byte("Hello, World!")

func plaintextHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	w.Header().Set("Content-Length", "13")
	w.Write(plainResp)
}

func jsonHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", "27")
	w.Write(jsonResp)
}

func echoHandler(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "read error", 500)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", fmt.Sprintf("%d", len(body)))
	w.Write(body)
}

func main() {
	runtime.GOMAXPROCS(runtime.NumCPU())

	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	fmt.Printf("Go HTTP Server (net/http) on port %s\n", port)

	http.HandleFunc("/plaintext", plaintextHandler)
	http.HandleFunc("/json", jsonHandler)
	http.HandleFunc("/echo", echoHandler)

	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		log.Fatal("ListenAndServe: ", err)
	}
}
