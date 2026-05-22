// http_server_fasthttp.go - fasthttp server for benchmark.
//
// Usage:
//   go build -o http_server_fasthttp_bin http_server_fasthttp.go && ./http_server_fasthttp_bin [port]
//
// Default port: 8080

//go:build ignore

package main

import (
	"fmt"
	"log"
	"os"
	"runtime"

	"github.com/valyala/fasthttp"
)

var jsonResp = []byte(`{"message":"Hello, World!"}`)
var plainResp = []byte("Hello, World!")

func requestHandler(ctx *fasthttp.RequestCtx) {
	path := string(ctx.Path())
	switch path {
	case "/plaintext":
		ctx.SetContentType("text/plain")
		ctx.SetBody(plainResp)
	case "/json":
		ctx.SetContentType("application/json")
		ctx.SetBody(jsonResp)
	case "/echo":
		ctx.SetContentType("application/octet-stream")
		ctx.SetBody(ctx.PostBody())
	default:
		ctx.SetStatusCode(fasthttp.StatusNotFound)
		ctx.SetBodyString("Not Found")
	}
}

func main() {
	runtime.GOMAXPROCS(runtime.NumCPU())

	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	fmt.Printf("Go HTTP Server (fasthttp) on port %s\n", port)

	server := &fasthttp.Server{
		Handler:            requestHandler,
		DisableKeepalive:   false,
		TCPKeepalive:       true,
		ReduceMemoryUsage:  false,
	}

	if err := server.ListenAndServe("127.0.0.1:" + port); err != nil {
		log.Fatal("ListenAndServe: ", err)
	}
}
