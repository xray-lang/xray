// http_server.js - Node.js HTTP server for benchmark.
//
// Usage:
//   node http_server.js [port]
//
// Default port: 8080
// No external dependencies needed.

const http = require("http");

const port = parseInt(process.argv[2] || "8080", 10);

const plainResp = Buffer.from("Hello, World!");
const jsonResp = Buffer.from(JSON.stringify({ message: "Hello, World!" }));

const server = http.createServer((req, res) => {
  if (req.url === "/plaintext" && req.method === "GET") {
    res.writeHead(200, {
      "Content-Type": "text/plain",
      "Content-Length": plainResp.length,
    });
    res.end(plainResp);
  } else if (req.url === "/json" && req.method === "GET") {
    res.writeHead(200, {
      "Content-Type": "application/json",
      "Content-Length": jsonResp.length,
    });
    res.end(jsonResp);
  } else if (req.url === "/echo" && req.method === "POST") {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      const body = Buffer.concat(chunks);
      res.writeHead(200, {
        "Content-Type": "application/octet-stream",
        "Content-Length": body.length,
      });
      res.end(body);
    });
  } else {
    res.writeHead(404, { "Content-Type": "text/plain" });
    res.end("Not Found");
  }
});

server.keepAliveTimeout = 30000;

server.listen(port, "127.0.0.1", () => {
  console.log(`Node.js HTTP Server on port ${port}`);
});

server.on("error", (err) => {
  console.error("Server error:", err);
  process.exit(1);
});
