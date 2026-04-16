/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_transport.c - LSP transport layer implementation
 */

#include "xlsp_transport.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#define STDIN_FILENO 0
#else
#include <sys/select.h>
#include "../../base/xmalloc.h"
#endif

#define INITIAL_BUFFER_SIZE 4096
#define HEADER_BUFFER_SIZE 256
#define PARTIAL_HEADER_SIZE 1024

XrLspTransport *xlsp_transport_stdio(void) {
    XrLspTransport *t = xr_calloc(1, sizeof(XrLspTransport));
    if (!t) return NULL;
    
    t->in = stdin;
    t->out = stdout;
    t->buffer_size = INITIAL_BUFFER_SIZE;
    t->read_buffer = xr_malloc(t->buffer_size);
    t->in_fd = STDIN_FILENO;
    
    // Partial message state
    t->partial_header = NULL;
    t->partial_len = 0;
    t->pending_content_length = -1;
    
    // Disable buffering for stdout to ensure messages are sent immediately
    setvbuf(stdout, NULL, _IONBF, 0);
    
    return t;
}

void xlsp_transport_free(XrLspTransport *t) {
    if (!t) return;
    xr_free(t->read_buffer);
    xr_free(t->partial_header);
    xr_free(t);
}

// Get input file descriptor for polling
int xlsp_transport_get_fd(XrLspTransport *t) {
    if (!t) return -1;
    return t->in_fd;
}

// Check if there's data available to read (non-blocking)
int xlsp_transport_has_data(XrLspTransport *t) {
    if (!t || t->in_fd < 0) return -1;
    
#ifdef _WIN32
    // Windows: use _kbhit or PeekNamedPipe
    // Simplified: assume data is available
    return 1;
#else
    fd_set fds;
    struct timeval tv = {0, 0};  // Non-blocking
    
    FD_ZERO(&fds);
    FD_SET(t->in_fd, &fds);
    
    int ret = select(t->in_fd + 1, &fds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    
    return ret > 0 ? 1 : 0;
#endif
}

// Parse Content-Length from headers
static int parse_content_length(const char *header_line) {
    const char *prefix = "Content-Length:";
    size_t prefix_len = strlen(prefix);
    
    if (strncasecmp(header_line, prefix, prefix_len) != 0) {
        return -1;
    }
    
    const char *value = header_line + prefix_len;
    while (*value && isspace((unsigned char)*value)) value++;
    
    return atoi(value);
}

char *xlsp_transport_read(XrLspTransport *t, size_t *out_len) {
    char header_line[HEADER_BUFFER_SIZE];
    int content_length = -1;
    
    // Read headers until empty line
    while (1) {
        if (!fgets(header_line, sizeof(header_line), t->in)) {
            return NULL;  // EOF or error
        }
        
        // Remove trailing \r\n
        size_t len = strlen(header_line);
        while (len > 0 && (header_line[len-1] == '\n' || header_line[len-1] == '\r')) {
            header_line[--len] = '\0';
        }
        
        // Empty line = end of headers
        if (len == 0) {
            break;
        }
        
        // Parse Content-Length
        int cl = parse_content_length(header_line);
        if (cl >= 0) {
            content_length = cl;
        }
    }
    
    if (content_length < 0) {
        return NULL;  // No Content-Length header
    }
    
    // Ensure buffer is large enough
    if ((size_t)content_length + 1 > t->buffer_size) {
        t->buffer_size = content_length + 1;
        t->read_buffer = xr_realloc(t->read_buffer, t->buffer_size);
        if (!t->read_buffer) return NULL;
    }
    
    // Read content
    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        size_t n = fread(t->read_buffer + total_read, 1, 
                        content_length - total_read, t->in);
        if (n == 0) {
            return NULL;  // EOF or error
        }
        total_read += n;
    }
    
    t->read_buffer[content_length] = '\0';
    
    // Return a copy
    char *result = xr_malloc(content_length + 1);
    if (!result) return NULL;
    memcpy(result, t->read_buffer, content_length + 1);
    
    if (out_len) *out_len = content_length;
    return result;
}

void xlsp_transport_write(XrLspTransport *t, const char *json, size_t len) {
    fprintf(t->out, "Content-Length: %zu\r\n\r\n", len);
    fwrite(json, 1, len, t->out);
    fflush(t->out);
}

// Try to read one LSP message without blocking
// This is a simplified implementation that checks for data availability
// before doing a blocking read
char *xlsp_transport_try_read(XrLspTransport *t, size_t *out_len, bool *would_block) {
    if (!t) {
        if (would_block) *would_block = false;
        return NULL;
    }
    
    // Check if there's data available
    int has_data = xlsp_transport_has_data(t);
    if (has_data <= 0) {
        if (would_block) *would_block = (has_data == 0);
        return NULL;
    }
    
    // Data is available, do a normal blocking read
    // (which should return quickly since data is available)
    if (would_block) *would_block = false;
    return xlsp_transport_read(t, out_len);
}
