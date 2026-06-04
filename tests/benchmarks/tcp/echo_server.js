// TCP benchmark server for Node.js
// Usage: node echo_server.js [port mode extra]

const net = require('net');

const port = parseInt(process.argv[2]) || 9001;
const mode = process.argv[3] || 'message';
const extra = parseInt(process.argv[4]) || (64 * 1024 * 1024);
const chunk = Buffer.alloc(65536, 'S');

function serveMessage(socket) {
    socket.on('data', (data) => {
        socket.write(data);
    });
    socket.on('error', () => {});
}

function serveDiscard(socket) {
    socket.on('data', () => {});
    socket.on('end', () => {
        socket.end('OK');
    });
    socket.on('error', () => {});
}

function serveSlowDiscard(socket, delayMs) {
    socket.on('data', () => {
        socket.pause();
        setTimeout(() => socket.resume(), Math.max(0, delayMs));
    });
    socket.on('end', () => {
        socket.end('OK');
    });
    socket.on('error', () => {});
}

function serveSource(socket, totalBytes) {
    let remaining = totalBytes;
    function writeMore() {
        while (remaining > 0) {
            const data = remaining >= chunk.length ? chunk : chunk.subarray(0, remaining);
            remaining -= data.length;
            if (!socket.write(data)) {
                socket.once('drain', writeMore);
                return;
            }
        }
        socket.end();
    }
    socket.on('error', () => {});
    writeMore();
}

function serveProxy(socket, upstreamPort) {
    const upstream = net.createConnection({ host: '127.0.0.1', port: upstreamPort });
    socket.pipe(upstream);
    upstream.pipe(socket);
    socket.on('error', () => upstream.destroy());
    upstream.on('error', () => socket.destroy());
}

const server = net.createServer((socket) => {
    if (mode === 'discard') {
        serveDiscard(socket);
    } else if (mode === 'slow_discard') {
        serveSlowDiscard(socket, extra);
    } else if (mode === 'source') {
        serveSource(socket, extra);
    } else if (mode === 'proxy') {
        serveProxy(socket, extra);
    } else {
        serveMessage(socket);
    }
});

server.listen(port, () => {
    console.log(`Node.js TCP benchmark server listening on port ${port} mode=${mode}`);
});
