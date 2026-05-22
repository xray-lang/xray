// echo_server.js - Node.js WebSocket echo server (ws library) for benchmark.
//
// Usage:
//   node echo_server.js [port]
//
// Default port: 9001
// Requires: npm install ws

const WebSocket = require("ws");

const port = parseInt(process.argv[2] || "9001", 10);

const wss = new WebSocket.Server({
  port: port,
  host: "127.0.0.1",
  perMessageDeflate: false,
  maxPayload: 10 * 1024 * 1024,
});

wss.on("listening", () => {
  console.log(`Node.js WebSocket Echo Server (ws) on port ${port}`);
});

wss.on("connection", (ws) => {
  ws.on("message", (data, isBinary) => {
    ws.send(data, { binary: isBinary });
  });
});

wss.on("error", (err) => {
  console.error("Server error:", err);
  process.exit(1);
});
