# HTTP 网络测试用例

这些测试用例需要网络连接，不包含在常规回归测试中。

## 测试文件列表

| 文件 | 功能 | 依赖 |
|------|------|------|
| `1300_http_url.xr` | URL 编码/解码 | 无网络 |
| `1301_http_client_basic.xr` | HTTP 基本请求 (GET/POST/PUT/DELETE) | httpbin.org |
| `1302_http_request_options.xr` | http.request() 配置选项 | httpbin.org |
| `1303_http_status_codes.xr` | HTTP 状态码处理 (2xx/3xx/4xx/5xx) | httpbin.org |
| `1304_http_https.xr` | HTTPS/TLS 请求 | httpbin.org |
| `1305_http_response_body.xr` | 响应体格式 (JSON/HTML/Gzip) | httpbin.org |
| `1306_http_redirect.xr` | 重定向处理 | httpbin.org |
| `1307_http_error_handling.xr` | 错误处理 | 无网络 |
| `1308_http_headers.xr` | 请求头/响应头 | httpbin.org |
| `1310_http2_client.xr` | HTTP/2 客户端 | nghttp2.org |
| `1320_websocket.xr` | WebSocket 连接 | postman-echo.com |

## 运行方式

```bash
# 运行单个测试
./build/xray tests/network/1301_http_client_basic.xr

# 运行所有网络测试
for f in tests/network/*.xr; do
    echo "运行: $f"
    ./build/xray "$f"
done
```

## 测试设计原则

1. **优雅降级**：网络不可用时跳过而非失败
2. **公共服务**：使用 httpbin.org 等公共测试 API
3. **全面覆盖**：覆盖 HTTP 库的所有主要功能

## HTTP 库功能清单

### HTTP/1.1 客户端
- `http.request(options)` - 通用请求

### URL 工具
- `http.urlEncode(str)` - URL 编码
- `http.urlDecode(str)` - URL 解码

### HTTP/2 客户端
- `http.h2Request(options)` - HTTP/2 通用请求（通过 `method`/`body`/`headers` 表达 GET/POST 等请求）

### WebSocket (独立 ws 模块)
```xray
import ws

var conn = ws.connect(url)      // 连接 WebSocket，返回连接对象
ws.send(conn, message)          // 发送消息
var msg = ws.recv(conn)         // 接收消息
ws.close(conn)                  // 关闭连接
ws.ping(conn)                   // 发送 ping
conn.state                      // 连接创建/关闭写入的状态字段
conn.error                      // 连接失败时的错误字段
```

### HTTP 服务端
- `http.route(method, path, value)` - 添加静态响应
- `http.routeHandler(method, path, handler)` - 添加 `(Json) -> Json` 动态路由
- `http.listen(port)` - 监听端口并由纯 Xray 控制面调度 HTTP/1.x 请求
- `http.stopServer()` - 停止服务器

WebSocket 服务端入口在独立 `ws` 模块中；`http` 模块不再提供 HTTP upgrade 路由快捷入口。

### HTTP/2 服务端
- 当前不提供公开 HTTP/2 服务端 API；旧 `h2CreateServer`/`h2Listen`/`h2Stop`/`h2Push` 半成品表面已删除。

## 响应对象属性

```xray
var resp = http.request({url: "https://example.com", method: "GET"})

resp.status    // int: HTTP 状态码 (200, 404, 500...)
resp.ok        // bool: 是否成功 (status 在 200-299)
resp.body      // string: 响应体
resp.headers   // Json: 响应头（如果支持）
resp.error     // string: 错误信息（如果有）
```

## 已知问题

- 网络测试依赖外部服务，可能因网络原因失败
