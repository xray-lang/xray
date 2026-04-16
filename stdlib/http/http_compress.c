/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_compress.c - HTTP compression/decompression implementation
 *
 * KEY CONCEPT:
 *   gzip and deflate compression using zlib
 */

#include "http_compress.h"
#include <stdlib.h>
#include <string.h>

#ifdef XR_ENABLE_ZLIB
#include <zlib.h>
#define ZLIB_AVAILABLE 1
#else
#define ZLIB_AVAILABLE 0
#endif

/* ========== Compression Type Detection ========== */

XrCompressType xr_detect_compress_type(const char *content_encoding) {
    if (!content_encoding) return XR_COMPRESS_NONE;
    
    if (strcasecmp(content_encoding, "gzip") == 0 ||
        strcasecmp(content_encoding, "x-gzip") == 0) {
        return XR_COMPRESS_GZIP;
    }
    
    if (strcasecmp(content_encoding, "deflate") == 0) {
        return XR_COMPRESS_DEFLATE;
    }
    
    return XR_COMPRESS_NONE;
}

bool xr_compress_available(void) {
    return ZLIB_AVAILABLE;
}

/* ========== Decompression Implementation ========== */

#if ZLIB_AVAILABLE

int xr_gzip_decompress(const void *in, size_t in_len,
                        void **out, size_t *out_len) {
    if (!in || in_len == 0 || !out || !out_len) return -1;
    
    // Initial estimate: decompressed size is 4x compressed size
    size_t buf_size = in_len * 4;
    if (buf_size < 1024) buf_size = 1024;
    
    char *buf = (char*)malloc(buf_size);
    if (!buf) return -1;
    
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    
    // 16 + MAX_WBITS indicates gzip format
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        free(buf);
        return -1;
    }
    
    size_t total_out = 0;
    int ret;
    
    do {
        // Ensure enough space
        if (total_out >= buf_size) {
            buf_size *= 2;
            char *new_buf = (char*)realloc(buf, buf_size);
            if (!new_buf) {
                inflateEnd(&strm);
                free(buf);
                return -1;
            }
            buf = new_buf;
        }
        
        strm.next_out = (Bytef*)(buf + total_out);
        strm.avail_out = (uInt)(buf_size - total_out);
        
        ret = inflate(&strm, Z_NO_FLUSH);
        
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || 
            ret == Z_MEM_ERROR || ret == Z_NEED_DICT) {
            inflateEnd(&strm);
            free(buf);
            return -1;
        }
        
        total_out = strm.total_out;
        
    } while (ret != Z_STREAM_END);
    
    inflateEnd(&strm);
    
    // Shrink buffer
    char *final_buf = (char*)realloc(buf, total_out + 1);
    if (final_buf) {
        buf = final_buf;
    }
    buf[total_out] = '\0';
    
    *out = buf;
    *out_len = total_out;
    return 0;
}

int xr_deflate_decompress(const void *in, size_t in_len,
                           void **out, size_t *out_len) {
    if (!in || in_len == 0 || !out || !out_len) return -1;
    
    size_t buf_size = in_len * 4;
    if (buf_size < 1024) buf_size = 1024;
    
    char *buf = (char*)malloc(buf_size);
    if (!buf) return -1;
    
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    
    // -MAX_WBITS indicates raw deflate (no zlib header)
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        // Try deflate with zlib header
        if (inflateInit(&strm) != Z_OK) {
            free(buf);
            return -1;
        }
    }
    
    size_t total_out = 0;
    int ret;
    
    do {
        if (total_out >= buf_size) {
            buf_size *= 2;
            char *new_buf = (char*)realloc(buf, buf_size);
            if (!new_buf) {
                inflateEnd(&strm);
                free(buf);
                return -1;
            }
            buf = new_buf;
        }
        
        strm.next_out = (Bytef*)(buf + total_out);
        strm.avail_out = (uInt)(buf_size - total_out);
        
        ret = inflate(&strm, Z_NO_FLUSH);
        
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || 
            ret == Z_MEM_ERROR || ret == Z_NEED_DICT) {
            inflateEnd(&strm);
            free(buf);
            return -1;
        }
        
        total_out = strm.total_out;
        
    } while (ret != Z_STREAM_END);
    
    inflateEnd(&strm);
    
    char *final_buf = (char*)realloc(buf, total_out + 1);
    if (final_buf) {
        buf = final_buf;
    }
    buf[total_out] = '\0';
    
    *out = buf;
    *out_len = total_out;
    return 0;
}

#else // !ZLIB_AVAILABLE

int xr_gzip_decompress(const void *in, size_t in_len,
                        void **out, size_t *out_len) {
    (void)in; (void)in_len; (void)out; (void)out_len;
    return -1;  // zlib not available
}

int xr_deflate_decompress(const void *in, size_t in_len,
                           void **out, size_t *out_len) {
    (void)in; (void)in_len; (void)out; (void)out_len;
    return -1;  // zlib not available
}

#endif // ZLIB_AVAILABLE

int xr_decompress(XrCompressType type,
                  const void *in, size_t in_len,
                  void **out, size_t *out_len) {
    switch (type) {
        case XR_COMPRESS_GZIP:
            return xr_gzip_decompress(in, in_len, out, out_len);
        case XR_COMPRESS_DEFLATE:
            return xr_deflate_decompress(in, in_len, out, out_len);
        case XR_COMPRESS_NONE:
        default:
            return -1;
    }
}

/* ========== Compression Implementation ========== */

#if ZLIB_AVAILABLE

#include <pthread.h>

int xr_gzip_compress(const void *in, size_t in_len,
                     void **out, size_t *out_len,
                     int level) {
    if (!in || in_len == 0 || !out || !out_len) return -1;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    
    // Estimate compressed size
    size_t buf_size = compressBound(in_len) + 18; // gzip header/trailer
    char *buf = (char*)malloc(buf_size);
    if (!buf) return -1;
    
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = (Bytef*)buf;
    strm.avail_out = (uInt)buf_size;
    
    // 16 + MAX_WBITS indicates gzip format
    if (deflateInit2(&strm, level, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(buf);
        return -1;
    }
    
    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        free(buf);
        return -1;
    }
    
    size_t compressed_len = strm.total_out;
    deflateEnd(&strm);
    
    // Shrink buffer
    char *final_buf = (char*)realloc(buf, compressed_len);
    if (final_buf) {
        buf = final_buf;
    }
    
    *out = buf;
    *out_len = compressed_len;
    return 0;
}

int xr_deflate_compress(const void *in, size_t in_len,
                        void **out, size_t *out_len,
                        int level) {
    if (!in || in_len == 0 || !out || !out_len) return -1;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    
    size_t buf_size = compressBound(in_len);
    char *buf = (char*)malloc(buf_size);
    if (!buf) return -1;
    
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = (Bytef*)buf;
    strm.avail_out = (uInt)buf_size;
    
    // -MAX_WBITS indicates raw deflate
    if (deflateInit2(&strm, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(buf);
        return -1;
    }
    
    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        free(buf);
        return -1;
    }
    
    size_t compressed_len = strm.total_out;
    deflateEnd(&strm);
    
    char *final_buf = (char*)realloc(buf, compressed_len);
    if (final_buf) {
        buf = final_buf;
    }
    
    *out = buf;
    *out_len = compressed_len;
    return 0;
}

/* ========== Compressor Object Pool ========== */

#define COMPRESS_POOL_SIZE 8

typedef struct {
    z_stream strm;
    int level;
    bool in_use;
} XrCompressContext;

static struct {
    XrCompressContext contexts[COMPRESS_POOL_SIZE];
    pthread_mutex_t lock;
    bool initialized;
} g_compress_pool;

void xr_compress_pool_init(void) {
    if (g_compress_pool.initialized) return;
    pthread_mutex_init(&g_compress_pool.lock, NULL);
    memset(g_compress_pool.contexts, 0, sizeof(g_compress_pool.contexts));
    g_compress_pool.initialized = true;
}

void xr_compress_pool_shutdown(void) {
    if (!g_compress_pool.initialized) return;
    
    pthread_mutex_lock(&g_compress_pool.lock);
    for (int i = 0; i < COMPRESS_POOL_SIZE; i++) {
        if (g_compress_pool.contexts[i].level > 0) {
            deflateEnd(&g_compress_pool.contexts[i].strm);
        }
    }
    pthread_mutex_unlock(&g_compress_pool.lock);
    
    pthread_mutex_destroy(&g_compress_pool.lock);
    g_compress_pool.initialized = false;
}

static XrCompressContext* pool_acquire(int level) {
    pthread_mutex_lock(&g_compress_pool.lock);
    
    // Find idle context with matching level
    for (int i = 0; i < COMPRESS_POOL_SIZE; i++) {
        if (!g_compress_pool.contexts[i].in_use && 
            g_compress_pool.contexts[i].level == level) {
            g_compress_pool.contexts[i].in_use = true;
            pthread_mutex_unlock(&g_compress_pool.lock);
            deflateReset(&g_compress_pool.contexts[i].strm);
            return &g_compress_pool.contexts[i];
        }
    }
    
    // Find empty slot to create new context
    for (int i = 0; i < COMPRESS_POOL_SIZE; i++) {
        if (!g_compress_pool.contexts[i].in_use && 
            g_compress_pool.contexts[i].level == 0) {
            XrCompressContext *ctx = &g_compress_pool.contexts[i];
            memset(&ctx->strm, 0, sizeof(ctx->strm));
            if (deflateInit2(&ctx->strm, level, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                ctx->level = level;
                ctx->in_use = true;
                pthread_mutex_unlock(&g_compress_pool.lock);
                return ctx;
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&g_compress_pool.lock);
    return NULL;
}

static void pool_release(XrCompressContext *ctx) {
    if (!ctx) return;
    pthread_mutex_lock(&g_compress_pool.lock);
    ctx->in_use = false;
    pthread_mutex_unlock(&g_compress_pool.lock);
}

int xr_gzip_compress_pooled(const void *in, size_t in_len,
                            void **out, size_t *out_len,
                            int level) {
    if (!g_compress_pool.initialized) {
        xr_compress_pool_init();
    }
    
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    
    XrCompressContext *ctx = pool_acquire(level);
    if (!ctx) {
        // Pool full, use non-pooled version
        return xr_gzip_compress(in, in_len, out, out_len, level);
    }
    
    size_t buf_size = compressBound(in_len) + 18;
    char *buf = (char*)malloc(buf_size);
    if (!buf) {
        pool_release(ctx);
        return -1;
    }
    
    ctx->strm.next_in = (Bytef*)in;
    ctx->strm.avail_in = (uInt)in_len;
    ctx->strm.next_out = (Bytef*)buf;
    ctx->strm.avail_out = (uInt)buf_size;
    
    int ret = deflate(&ctx->strm, Z_FINISH);
    size_t compressed_len = ctx->strm.total_out;
    
    pool_release(ctx);
    
    if (ret != Z_STREAM_END) {
        free(buf);
        return -1;
    }
    
    char *final_buf = (char*)realloc(buf, compressed_len);
    if (final_buf) buf = final_buf;
    
    *out = buf;
    *out_len = compressed_len;
    return 0;
}

#else // !ZLIB_AVAILABLE

int xr_gzip_compress(const void *in, size_t in_len,
                     void **out, size_t *out_len, int level) {
    (void)in; (void)in_len; (void)out; (void)out_len; (void)level;
    return -1;
}

int xr_deflate_compress(const void *in, size_t in_len,
                        void **out, size_t *out_len, int level) {
    (void)in; (void)in_len; (void)out; (void)out_len; (void)level;
    return -1;
}

void xr_compress_pool_init(void) {}
void xr_compress_pool_shutdown(void) {}

int xr_gzip_compress_pooled(const void *in, size_t in_len,
                            void **out, size_t *out_len, int level) {
    (void)in; (void)in_len; (void)out; (void)out_len; (void)level;
    return -1;
}

#endif // ZLIB_AVAILABLE

int xr_compress(XrCompressType type, int level,
                const void *in, size_t in_len,
                void **out, size_t *out_len) {
    switch (type) {
        case XR_COMPRESS_GZIP:
            return xr_gzip_compress(in, in_len, out, out_len, level);
        case XR_COMPRESS_DEFLATE:
            return xr_deflate_compress(in, in_len, out, out_len, level);
        case XR_COMPRESS_NONE:
        default:
            return -1;
    }
}
