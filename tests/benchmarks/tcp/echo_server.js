// TCP Echo Server for Node.js benchmark
// Usage: node echo_server.js [port]

const net = require('net');

const port = parseInt(process.argv[2]) || 9001;

const server = net.createServer((socket) => {
    socket.on('data', (data) => {
        socket.write(data);
    });
    socket.on('error', () => {
        // silently ignore
    });
});

server.listen(port, () => {
    console.log(`Node.js TCP echo server listening on port ${port}`);
});
