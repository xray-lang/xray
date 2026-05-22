// http_dynamic.go - Go dynamic handler benchmark (matches http_dynamic.xr)
//
// All handlers do real work (no pre-cached byte slices for /plaintext).
//
// Endpoints:
//   GET /plaintext    - returns plain string
//   GET /json         - returns JSON via encoding/json
//   GET /users/:id    - path param extraction + string concat
//
// Usage:
//   go build -o /tmp/http_dynamic_go http_dynamic.go && /tmp/http_dynamic_go [port]

//go:build ignore

package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"runtime"
	"strings"
)

func plaintextHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprint(w, "Hello, World!")
}

func jsonHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"message": "Hello, World!"})
}

func usersHandler(w http.ResponseWriter, r *http.Request) {
	// Extract :id from /users/:id
	path := r.URL.Path
	id := ""
	if strings.HasPrefix(path, "/users/") {
		id = path[7:]
	}
	w.Header().Set("Content-Type", "text/plain")
	fmt.Fprintf(w, "User: %s", id)
}

func main() {
	runtime.GOMAXPROCS(runtime.NumCPU())

	port := "8080"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}

	fmt.Printf("Go Dynamic Handler Benchmark on port %s\n", port)

	http.HandleFunc("/plaintext", plaintextHandler)
	http.HandleFunc("/json", jsonHandler)
	http.HandleFunc("/users/", usersHandler)

	if err := http.ListenAndServe("127.0.0.1:"+port, nil); err != nil {
		log.Fatal("ListenAndServe: ", err)
	}
}
