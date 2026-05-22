const http = require('http');

const server = http.createServer((req, res) => {
    if (req.url === '/json') {
        res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end('{"message":"Hello, World!"}');
    } else {
        res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('Hello, World!');
    }
});

server.listen(8082, () => {
    console.log('Node.js listening on :8082');
});
