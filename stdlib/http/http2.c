/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2.c - HTTP/2 protocol implementation
 *
 * KEY CONCEPT:
 *   Implements RFC 7540 HTTP/2 protocol with HPACK header compression
 */

#include "http2.h"
#include "../net/tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ========== HPACK Static Table (RFC 7541 Appendix A) ========== */

static const struct {
    const char *name;
    const char *value;
} hpack_static_table[] = {
    {NULL, NULL},  // Index starts from 1
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

#define HPACK_STATIC_TABLE_SIZE (sizeof(hpack_static_table) / sizeof(hpack_static_table[0]) - 1)

/* ========== HPACK Integer Encoding/Decoding ========== */

// Encode integer (RFC 7541 5.1)
static int hpack_encode_int(uint8_t *buf, size_t buf_len, uint64_t value, int prefix_bits) {
    if (buf_len == 0) return -1;
    
    int max_prefix = (1 << prefix_bits) - 1;
    
    if (value < (uint64_t)max_prefix) {
        buf[0] |= (uint8_t)value;
        return 1;
    }
    
    buf[0] |= max_prefix;
    value -= max_prefix;
    int len = 1;
    
    while (value >= 128 && len < (int)buf_len) {
        buf[len++] = (uint8_t)((value & 0x7f) | 0x80);
        value >>= 7;
    }
    
    if (len >= (int)buf_len) return -1;
    buf[len++] = (uint8_t)value;
    
    return len;
}

// Decode integer
static int hpack_decode_int(const uint8_t *buf, size_t buf_len, uint64_t *value, int prefix_bits) {
    if (buf_len == 0) return -1;
    
    int max_prefix = (1 << prefix_bits) - 1;
    *value = buf[0] & max_prefix;
    
    if (*value < (uint64_t)max_prefix) {
        return 1;
    }
    
    int shift = 0;
    size_t i = 1;
    
    while (i < buf_len) {
        *value += ((uint64_t)(buf[i] & 0x7f)) << shift;
        if ((buf[i] & 0x80) == 0) {
            return (int)(i + 1);
        }
        shift += 7;
        i++;
    }
    
    return -1;  // Incomplete
}

/* ========== HPACK String Encoding/Decoding ========== */

// Encode string (without Huffman)
static int hpack_encode_string(uint8_t *buf, size_t buf_len, const char *str, size_t str_len) {
    buf[0] = 0;  // Not using Huffman
    int int_len = hpack_encode_int(buf, buf_len, str_len, 7);
    if (int_len < 0) return -1;
    
    if ((size_t)int_len + str_len > buf_len) return -1;
    
    memcpy(buf + int_len, str, str_len);
    return int_len + (int)str_len;
}

// Decode string
static int hpack_decode_string(const uint8_t *buf, size_t buf_len, 
                               char **str, size_t *str_len) {
    if (buf_len == 0) return -1;
    
    bool huffman = (buf[0] & 0x80) != 0;
    uint64_t len;
    int int_len = hpack_decode_int(buf, buf_len, &len, 7);
    if (int_len < 0) return -1;
    
    if ((size_t)int_len + len > buf_len) return -1;
    
    *str_len = (size_t)len;
    *str = (char*)malloc(len + 1);
    if (!*str) return -1;
    
    if (huffman) {
        // Simplified: Huffman not implemented yet, just copy
        memcpy(*str, buf + int_len, len);
    } else {
        memcpy(*str, buf + int_len, len);
    }
    (*str)[len] = '\0';
    
    return int_len + (int)len;
}

/* ========== HPACK Dynamic Table ========== */

void xr_hpack_init(XrHpackTable *table, size_t max_size) {
    memset(table, 0, sizeof(XrHpackTable));
    table->max_size = max_size;
}

void xr_hpack_free(XrHpackTable *table) {
    XrHpackEntry *entry = table->entries;
    while (entry) {
        XrHpackEntry *next = entry->next;
        free(entry->name);
        free(entry->value);
        free(entry);
        entry = next;
    }
    table->entries = NULL;
    table->size = 0;
    table->count = 0;
}

// Add entry to dynamic table
static void hpack_table_add(XrHpackTable *table, 
                            const char *name, size_t name_len,
                            const char *value, size_t value_len) {
    size_t entry_size = name_len + value_len + 32;  // RFC 7541: 32 bytes overhead
    
    // Evict old entries until enough space
    while (table->size + entry_size > table->max_size && table->entries) {
        // Remove last entry
        XrHpackEntry **pp = &table->entries;
        while (*pp && (*pp)->next) {
            pp = &(*pp)->next;
        }
        if (*pp) {
            XrHpackEntry *last = *pp;
            table->size -= last->name_len + last->value_len + 32;
            table->count--;
            free(last->name);
            free(last->value);
            free(last);
            *pp = NULL;
        }
    }
    
    if (entry_size > table->max_size) {
        // Entry too large, clear table
        xr_hpack_free(table);
        return;
    }
    
    // Create new entry
    XrHpackEntry *entry = (XrHpackEntry*)calloc(1, sizeof(XrHpackEntry));
    if (!entry) return;
    
    entry->name = (char*)malloc(name_len + 1);
    entry->value = (char*)malloc(value_len + 1);
    if (!entry->name || !entry->value) {
        free(entry->name);
        free(entry->value);
        free(entry);
        return;
    }
    
    memcpy(entry->name, name, name_len);
    entry->name[name_len] = '\0';
    entry->name_len = name_len;
    
    memcpy(entry->value, value, value_len);
    entry->value[value_len] = '\0';
    entry->value_len = value_len;
    
    // Insert at table head
    entry->next = table->entries;
    table->entries = entry;
    table->size += entry_size;
    table->count++;
}

// Find dynamic table entry
static XrHpackEntry* hpack_table_get(XrHpackTable *table, int index) {
    index -= HPACK_STATIC_TABLE_SIZE + 1;
    if (index < 0) return NULL;
    
    XrHpackEntry *entry = table->entries;
    while (entry && index > 0) {
        entry = entry->next;
        index--;
    }
    return entry;
}

/* ========== HPACK Encoding ========== */

int xr_hpack_encode(XrHpackTable *table, 
                    const char *name, size_t name_len,
                    const char *value, size_t value_len,
                    uint8_t *buf, size_t buf_len) {
    (void)table;
    // Simplified: use literal without indexing
    if (buf_len < 1) return -1;
    
    buf[0] = 0x00;  // Literal without indexing, new name
    int total = 1;
    
    // Encode name
    int len = hpack_encode_string(buf + total, buf_len - total, name, name_len);
    if (len < 0) return -1;
    total += len;
    
    // Encode value
    len = hpack_encode_string(buf + total, buf_len - total, value, value_len);
    if (len < 0) return -1;
    total += len;
    
    return total;
}

/* ========== HPACK Decoding ========== */

int xr_hpack_decode(XrHpackTable *table,
                    const uint8_t *buf, size_t buf_len,
                    void (*callback)(const char *name, size_t name_len,
                                    const char *value, size_t value_len,
                                    void *user_data),
                    void *user_data) {
    size_t pos = 0;
    
    while (pos < buf_len) {
        uint8_t b = buf[pos];
        char *name = NULL, *value = NULL;
        size_t name_len = 0, value_len = 0;
        bool add_to_table = false;
        
        if (b & 0x80) {
            // Indexed header field (RFC 7541 6.1)
            uint64_t index;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &index, 7);
            if (len < 0) return -1;
            pos += len;
            
            if (index <= HPACK_STATIC_TABLE_SIZE) {
                name = (char*)hpack_static_table[index].name;
                name_len = strlen(name);
                value = (char*)hpack_static_table[index].value;
                value_len = strlen(value);
            } else {
                XrHpackEntry *entry = hpack_table_get(table, (int)index);
                if (!entry) return -1;
                name = entry->name;
                name_len = entry->name_len;
                value = entry->value;
                value_len = entry->value_len;
            }
            
            if (callback) callback(name, name_len, value, value_len, user_data);
            
        } else if (b & 0x40) {
            // Literal with incremental indexing (RFC 7541 6.2.1)
            uint64_t index;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &index, 6);
            if (len < 0) return -1;
            pos += len;
            
            if (index == 0) {
                // New name
                len = hpack_decode_string(buf + pos, buf_len - pos, &name, &name_len);
                if (len < 0) return -1;
                pos += len;
            } else if (index <= HPACK_STATIC_TABLE_SIZE) {
                name = strdup(hpack_static_table[index].name);
                name_len = strlen(name);
            } else {
                XrHpackEntry *entry = hpack_table_get(table, (int)index);
                if (!entry) return -1;
                name = strdup(entry->name);
                name_len = entry->name_len;
            }
            
            len = hpack_decode_string(buf + pos, buf_len - pos, &value, &value_len);
            if (len < 0) { free(name); return -1; }
            pos += len;
            
            add_to_table = true;
            if (callback) callback(name, name_len, value, value_len, user_data);
            
            if (add_to_table) {
                hpack_table_add(table, name, name_len, value, value_len);
            }
            free(name);
            free(value);
            
        } else if (b & 0x20) {
            // Dynamic table size update (RFC 7541 6.3)
            uint64_t new_size;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &new_size, 5);
            if (len < 0) return -1;
            pos += len;
            table->max_size = new_size;
            
        } else {
            // Literal without indexing (RFC 7541 6.2.2/6.2.3)
            uint64_t index;
            int prefix = (b & 0x10) ? 4 : 4;
            int len = hpack_decode_int(buf + pos, buf_len - pos, &index, prefix);
            if (len < 0) return -1;
            pos += len;
            
            if (index == 0) {
                len = hpack_decode_string(buf + pos, buf_len - pos, &name, &name_len);
                if (len < 0) return -1;
                pos += len;
            } else if (index <= HPACK_STATIC_TABLE_SIZE) {
                name = strdup(hpack_static_table[index].name);
                name_len = strlen(name);
            } else {
                XrHpackEntry *entry = hpack_table_get(table, (int)index);
                if (!entry) return -1;
                name = strdup(entry->name);
                name_len = entry->name_len;
            }
            
            len = hpack_decode_string(buf + pos, buf_len - pos, &value, &value_len);
            if (len < 0) { free(name); return -1; }
            pos += len;
            
            if (callback) callback(name, name_len, value, value_len, user_data);
            free(name);
            free(value);
        }
    }
    
    return 0;
}

/* ========== Frame Header Parsing/Generation ========== */

int xr_h2_parse_frame_header(const uint8_t *buf, XrH2FrameHeader *header) {
    header->length = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
    header->type = buf[3];
    header->flags = buf[4];
    header->stream_id = ((uint32_t)buf[5] << 24) | ((uint32_t)buf[6] << 16) |
                        ((uint32_t)buf[7] << 8) | buf[8];
    header->stream_id &= 0x7FFFFFFF;  // Clear reserved bit
    return 0;
}

void xr_h2_write_frame_header(uint8_t *buf, const XrH2FrameHeader *header) {
    buf[0] = (header->length >> 16) & 0xFF;
    buf[1] = (header->length >> 8) & 0xFF;
    buf[2] = header->length & 0xFF;
    buf[3] = header->type;
    buf[4] = header->flags;
    buf[5] = (header->stream_id >> 24) & 0x7F;
    buf[6] = (header->stream_id >> 16) & 0xFF;
    buf[7] = (header->stream_id >> 8) & 0xFF;
    buf[8] = header->stream_id & 0xFF;
}

/* ========== HTTP/2 Connection ========== */

XrH2Conn* xr_h2_conn_new_client(int fd, void *tls_conn) {
    XrH2Conn *conn = (XrH2Conn*)calloc(1, sizeof(XrH2Conn));
    if (!conn) return NULL;
    
    conn->fd = fd;
    conn->tls_conn = tls_conn;
    conn->is_client = true;
    conn->next_stream_id = 1;  // Client uses odd stream IDs
    conn->connection_window = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;
    
    // Default settings
    conn->local_settings[XR_H2_SETTINGS_HEADER_TABLE_SIZE] = XR_H2_DEFAULT_HEADER_TABLE_SIZE;
    conn->local_settings[XR_H2_SETTINGS_ENABLE_PUSH] = 0;  // Client disables push
    conn->local_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS] = XR_H2_DEFAULT_MAX_CONCURRENT_STREAMS;
    conn->local_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE] = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->local_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE] = XR_H2_DEFAULT_MAX_FRAME_SIZE;
    
    memcpy(conn->remote_settings, conn->local_settings, sizeof(conn->local_settings));
    
    // Initialize HPACK tables
    xr_hpack_init(&conn->encoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);
    xr_hpack_init(&conn->decoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);
    
    // Allocate receive buffer
    conn->recv_cap = 16384;
    conn->recv_buf = (char*)malloc(conn->recv_cap);
    if (!conn->recv_buf) {
        free(conn);
        return NULL;
    }
    
    return conn;
}

void xr_h2_conn_free(XrH2Conn *conn) {
    if (!conn) return;
    
    xr_hpack_free(&conn->encoder_table);
    xr_hpack_free(&conn->decoder_table);
    
    // Free stream hash table
    xr_h2_stream_hash_free(&conn->stream_hash);
    
    free(conn->recv_buf);
    free(conn);
}

// Send data
static int h2_send(XrH2Conn *conn, const void *buf, size_t len) {
    // Simplified: use write directly
    return (int)write(conn->fd, buf, len);
}

int xr_h2_conn_init(XrH2Conn *conn) {
    if (!conn) return -1;
    
    // Send connection preface
    if (conn->is_client) {
        if (h2_send(conn, XR_HTTP2_PREFACE, XR_HTTP2_PREFACE_LEN) < 0) {
            return -1;
        }
    }
    
    // Send SETTINGS
    return xr_h2_send_settings(conn);
}

int xr_h2_send_settings(XrH2Conn *conn) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 36];
    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    int payload_len = 0;
    
    // Encode settings parameters
    #define ADD_SETTING(id, val) do { \
        payload[payload_len++] = ((id) >> 8) & 0xFF; \
        payload[payload_len++] = (id) & 0xFF; \
        payload[payload_len++] = ((val) >> 24) & 0xFF; \
        payload[payload_len++] = ((val) >> 16) & 0xFF; \
        payload[payload_len++] = ((val) >> 8) & 0xFF; \
        payload[payload_len++] = (val) & 0xFF; \
    } while(0)
    
    ADD_SETTING(XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS, conn->local_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS]);
    ADD_SETTING(XR_H2_SETTINGS_INITIAL_WINDOW_SIZE, conn->local_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE]);
    ADD_SETTING(XR_H2_SETTINGS_MAX_FRAME_SIZE, conn->local_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE]);
    
    if (conn->is_client) {
        ADD_SETTING(XR_H2_SETTINGS_ENABLE_PUSH, 0);
    }
    
    #undef ADD_SETTING
    
    // Write frame header
    XrH2FrameHeader header = {
        .length = payload_len,
        .type = XR_H2_FRAME_SETTINGS,
        .flags = 0,
        .stream_id = 0
    };
    xr_h2_write_frame_header(frame, &header);
    
    conn->settings_sent = true;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + payload_len);
}

int xr_h2_send_settings_ack(XrH2Conn *conn) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE];
    XrH2FrameHeader header = {
        .length = 0,
        .type = XR_H2_FRAME_SETTINGS,
        .flags = XR_H2_FLAG_ACK,
        .stream_id = 0
    };
    xr_h2_write_frame_header(frame, &header);
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE);
}

/* ========== Stream Hash Table Implementation ========== */

static inline uint32_t stream_hash_func(uint32_t stream_id) {
    return stream_id % XR_H2_STREAM_HASH_SIZE;
}

void xr_h2_stream_hash_init(XrH2StreamHash *hash) {
    if (!hash) return;
    memset(hash->buckets, 0, sizeof(hash->buckets));
    hash->count = 0;
}

void xr_h2_stream_hash_add(XrH2StreamHash *hash, XrH2Stream *stream) {
    if (!hash || !stream) return;
    uint32_t idx = stream_hash_func(stream->id);
    stream->next = hash->buckets[idx];
    hash->buckets[idx] = stream;
    hash->count++;
}

XrH2Stream* xr_h2_stream_hash_find(XrH2StreamHash *hash, uint32_t stream_id) {
    if (!hash) return NULL;
    uint32_t idx = stream_hash_func(stream_id);
    XrH2Stream *stream = hash->buckets[idx];
    while (stream) {
        if (stream->id == stream_id) return stream;
        stream = stream->next;
    }
    return NULL;
}

void xr_h2_stream_hash_remove(XrH2StreamHash *hash, uint32_t stream_id) {
    if (!hash) return;
    uint32_t idx = stream_hash_func(stream_id);
    XrH2Stream **pp = &hash->buckets[idx];
    while (*pp) {
        if ((*pp)->id == stream_id) {
            XrH2Stream *to_remove = *pp;
            *pp = to_remove->next;
            hash->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

void xr_h2_stream_hash_free(XrH2StreamHash *hash) {
    if (!hash) return;
    for (int i = 0; i < XR_H2_STREAM_HASH_SIZE; i++) {
        XrH2Stream *stream = hash->buckets[i];
        while (stream) {
            XrH2Stream *next = stream->next;
            // Free stream resources
            free(stream->headers_buf);
            free(stream->data_buf);
            free(stream->trailers_buf);
            free(stream);
            stream = next;
        }
        hash->buckets[i] = NULL;
    }
    hash->count = 0;
}

/* ========== Stream Management ========== */

XrH2Stream* xr_h2_stream_new(XrH2Conn *conn) {
    if (!conn) return NULL;
    
    XrH2Stream *stream = (XrH2Stream*)calloc(1, sizeof(XrH2Stream));
    if (!stream) return NULL;
    
    stream->id = conn->next_stream_id;
    conn->next_stream_id += 2;  // Skip even numbers (server push)
    stream->state = XR_H2_STREAM_IDLE;
    stream->window_size = conn->remote_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE];
    
    // Add to stream hash table
    xr_h2_stream_hash_add(&conn->stream_hash, stream);
    
    return stream;
}

XrH2Stream* xr_h2_get_stream(XrH2Conn *conn, uint32_t stream_id) {
    if (!conn) return NULL;
    return xr_h2_stream_hash_find(&conn->stream_hash, stream_id);
}

// Receive data
static int h2_recv(XrH2Conn *conn, void *buf, size_t len) {
    if (conn->tls_conn) {
        return xr_tls_conn_read(conn->tls_conn, buf, len);
    }
    return (int)read(conn->fd, buf, len);
}

// HPACK header decode callback: extract :status pseudo-header
static void h2_header_callback(const char *name, size_t name_len,
                                const char *value, size_t value_len,
                                void *user_data) {
    XrH2Stream *stream = (XrH2Stream *)user_data;
    if (name_len == 7 && memcmp(name, ":status", 7) == 0 && value_len > 0) {
        int status = 0;
        for (size_t i = 0; i < value_len && value[i] >= '0' && value[i] <= '9'; i++) {
            status = status * 10 + (value[i] - '0');
        }
        stream->status = status;
    }
}

// Receive and process frame
int xr_h2_recv(XrH2Conn *conn) {
    if (!conn) return -1;
    
    // Receive frame header
    uint8_t frame_header[XR_H2_FRAME_HEADER_SIZE];
    int n = h2_recv(conn, frame_header, XR_H2_FRAME_HEADER_SIZE);
    if (n != XR_H2_FRAME_HEADER_SIZE) return -1;
    
    XrH2FrameHeader header;
    xr_h2_parse_frame_header(frame_header, &header);
    
    // Receive frame payload
    uint8_t *payload = NULL;
    if (header.length > 0) {
        payload = (uint8_t*)malloc(header.length);
        if (!payload) return -1;
        
        n = h2_recv(conn, payload, header.length);
        if (n != (int)header.length) {
            free(payload);
            return -1;
        }
    }
    
    // Process frame
    int result = 0;
    switch (header.type) {
        case XR_H2_FRAME_SETTINGS:
            if (header.flags & XR_H2_FLAG_ACK) {
                conn->settings_acked = true;
            } else {
                // Parse SETTINGS
                for (uint32_t i = 0; i + 6 <= header.length; i += 6) {
                    uint16_t id = (payload[i] << 8) | payload[i + 1];
                    uint32_t val = (payload[i + 2] << 24) | (payload[i + 3] << 16) |
                                   (payload[i + 4] << 8) | payload[i + 5];
                    if (id < 7) conn->remote_settings[id] = val;
                }
                xr_h2_send_settings_ack(conn);
            }
            break;
            
        case XR_H2_FRAME_HEADERS: {
            XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
            if (!stream) {
                stream = (XrH2Stream*)calloc(1, sizeof(XrH2Stream));
                if (stream) {
                    stream->id = header.stream_id;
                    xr_h2_stream_hash_add(&conn->stream_hash, stream);
                }
            }
            // Decode HPACK headers to extract :status pseudo-header
            if (stream && payload && header.length > 0) {
                xr_hpack_decode(&conn->decoder_table, payload, header.length,
                                h2_header_callback, stream);
            }
            if (stream && header.flags & XR_H2_FLAG_END_STREAM) {
                stream->state = XR_H2_STREAM_HALF_CLOSED_REMOTE;
            }
            break;
        }
        
        case XR_H2_FRAME_DATA: {
            XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
            if (stream && payload && header.length > 0) {
                // Append data to stream buffer
                size_t new_len = stream->data_len + header.length;
                char *new_buf = (char*)realloc(stream->data_buf, new_len + 1);
                if (new_buf) {
                    memcpy(new_buf + stream->data_len, payload, header.length);
                    new_buf[new_len] = '\0';
                    stream->data_buf = new_buf;
                    stream->data_len = new_len;
                }
            }
            if (stream && header.flags & XR_H2_FLAG_END_STREAM) {
                stream->state = XR_H2_STREAM_HALF_CLOSED_REMOTE;
            }
            // Send WINDOW_UPDATE
            if (header.length > 0) {
                xr_h2_send_window_update(conn, 0, header.length);
                xr_h2_send_window_update(conn, header.stream_id, header.length);
            }
            break;
        }
        
        case XR_H2_FRAME_GOAWAY:
            conn->goaway_received = true;
            break;
            
        case XR_H2_FRAME_PING:
            if (!(header.flags & XR_H2_FLAG_ACK)) {
                xr_h2_send_ping(conn, payload, true);
            }
            break;
            
        case XR_H2_FRAME_RST_STREAM: {
            XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
            if (stream) {
                stream->state = XR_H2_STREAM_STATE_CLOSED;
                stream->cancelled = true;
            }
            break;
        }
        
        case XR_H2_FRAME_WINDOW_UPDATE:
            // Update window size
            if (header.stream_id == 0) {
                uint32_t inc = (payload[0] << 24) | (payload[1] << 16) |
                               (payload[2] << 8) | payload[3];
                conn->connection_window += inc;
            } else {
                XrH2Stream *stream = xr_h2_get_stream(conn, header.stream_id);
                if (stream) {
                    uint32_t inc = (payload[0] << 24) | (payload[1] << 16) |
                                   (payload[2] << 8) | payload[3];
                    stream->window_size += inc;
                }
            }
            break;
            
        default:
            break;
    }
    
    free(payload);
    return result;
}

/* ========== Send Frames ========== */

int xr_h2_send_headers(XrH2Conn *conn, XrH2Stream *stream,
                       const char **names, const size_t *name_lens,
                       const char **values, const size_t *value_lens,
                       int count, bool end_stream) {
    if (!conn || !stream) return -1;
    
    // Encode headers
    uint8_t headers_buf[16384];
    int headers_len = 0;
    
    for (int i = 0; i < count; i++) {
        int len = xr_hpack_encode(&conn->encoder_table,
                                   names[i], name_lens[i],
                                   values[i], value_lens[i],
                                   headers_buf + headers_len,
                                   sizeof(headers_buf) - headers_len);
        if (len < 0) return -1;
        headers_len += len;
    }
    
    // Send HEADERS frame
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 16384];
    XrH2FrameHeader header = {
        .length = headers_len,
        .type = XR_H2_FRAME_HEADERS,
        .flags = XR_H2_FLAG_END_HEADERS | (end_stream ? XR_H2_FLAG_END_STREAM : 0),
        .stream_id = stream->id
    };
    xr_h2_write_frame_header(frame, &header);
    memcpy(frame + XR_H2_FRAME_HEADER_SIZE, headers_buf, headers_len);
    
    stream->state = XR_H2_STREAM_OPEN;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + headers_len);
}

// Receive until stream closes, return response data
int xr_h2_recv_stream_data(XrH2Conn *conn, XrH2Stream *stream,
                            char **out_data, size_t *out_len) {
    if (!conn || !stream) return -1;
    
    // Receive until stream closes
    while (stream->state != XR_H2_STREAM_STATE_CLOSED &&
           stream->state != XR_H2_STREAM_HALF_CLOSED_REMOTE) {
        if (xr_h2_recv(conn) < 0) return -1;
    }
    
    // Copy response body
    if (out_data && stream->data_buf && stream->data_len > 0) {
        *out_data = (char*)malloc(stream->data_len + 1);
        if (*out_data) {
            memcpy(*out_data, stream->data_buf, stream->data_len);
            (*out_data)[stream->data_len] = '\0';
            if (out_len) *out_len = stream->data_len;
        }
    }
    
    return 0;
}

int xr_h2_send_data(XrH2Conn *conn, XrH2Stream *stream,
                    const void *data, size_t len, bool end_stream) {
    if (!conn || !stream) return -1;
    
    // Send in fragments (respecting frame size limit)
    size_t max_frame = conn->remote_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE];
    size_t sent = 0;
    
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > max_frame) chunk = max_frame;
        
        uint8_t frame[XR_H2_FRAME_HEADER_SIZE];
        XrH2FrameHeader header = {
            .length = (uint32_t)chunk,
            .type = XR_H2_FRAME_DATA,
            .flags = (sent + chunk >= len && end_stream) ? XR_H2_FLAG_END_STREAM : 0,
            .stream_id = stream->id
        };
        xr_h2_write_frame_header(frame, &header);
        
        if (h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE) < 0) return -1;
        if (h2_send(conn, (const char*)data + sent, chunk) < 0) return -1;
        
        sent += chunk;
    }
    
    if (end_stream) {
        stream->state = XR_H2_STREAM_HALF_CLOSED_LOCAL;
    }
    
    return (int)sent;
}

int xr_h2_send_goaway(XrH2Conn *conn, uint32_t last_stream_id, XrH2ErrorCode error) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 8];
    XrH2FrameHeader header = {
        .length = 8,
        .type = XR_H2_FRAME_GOAWAY,
        .flags = 0,
        .stream_id = 0
    };
    xr_h2_write_frame_header(frame, &header);
    
    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    payload[0] = (last_stream_id >> 24) & 0x7F;
    payload[1] = (last_stream_id >> 16) & 0xFF;
    payload[2] = (last_stream_id >> 8) & 0xFF;
    payload[3] = last_stream_id & 0xFF;
    payload[4] = (error >> 24) & 0xFF;
    payload[5] = (error >> 16) & 0xFF;
    payload[6] = (error >> 8) & 0xFF;
    payload[7] = error & 0xFF;
    
    conn->goaway_sent = true;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 8);
}

int xr_h2_send_window_update(XrH2Conn *conn, uint32_t stream_id, uint32_t increment) {
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 4];
    XrH2FrameHeader header = {
        .length = 4,
        .type = XR_H2_FRAME_WINDOW_UPDATE,
        .flags = 0,
        .stream_id = stream_id
    };
    xr_h2_write_frame_header(frame, &header);
    
    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    payload[0] = (increment >> 24) & 0x7F;
    payload[1] = (increment >> 16) & 0xFF;
    payload[2] = (increment >> 8) & 0xFF;
    payload[3] = increment & 0xFF;
    
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 4);
}

/* ========== Additional API ========== */

XrH2Conn* xr_h2_conn_new(int fd, void *tls_conn, bool is_client) {
    XrH2Conn *conn = (XrH2Conn*)calloc(1, sizeof(XrH2Conn));
    if (!conn) return NULL;
    
    conn->fd = fd;
    conn->tls_conn = tls_conn;
    conn->is_client = is_client;
    
    // Initialize settings to default values
    conn->local_settings[XR_H2_SETTINGS_HEADER_TABLE_SIZE] = XR_H2_DEFAULT_HEADER_TABLE_SIZE;
    conn->local_settings[XR_H2_SETTINGS_ENABLE_PUSH] = 0;
    conn->local_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS] = XR_H2_DEFAULT_MAX_CONCURRENT_STREAMS;
    conn->local_settings[XR_H2_SETTINGS_INITIAL_WINDOW_SIZE] = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;
    conn->local_settings[XR_H2_SETTINGS_MAX_FRAME_SIZE] = XR_H2_DEFAULT_MAX_FRAME_SIZE;
    conn->local_settings[XR_H2_SETTINGS_MAX_HEADER_LIST_SIZE] = XR_H2_DEFAULT_MAX_HEADER_LIST_SIZE;
    
    memcpy(conn->remote_settings, conn->local_settings, sizeof(conn->remote_settings));
    
    xr_hpack_init(&conn->encoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);
    xr_hpack_init(&conn->decoder_table, XR_H2_DEFAULT_HEADER_TABLE_SIZE);
    
    conn->next_stream_id = is_client ? 1 : 2;
    conn->connection_window = XR_H2_DEFAULT_INITIAL_WINDOW_SIZE;
    
    conn->recv_cap = 65536;
    conn->recv_buf = (char*)malloc(conn->recv_cap);
    if (!conn->recv_buf) {
        xr_hpack_free(&conn->encoder_table);
        xr_hpack_free(&conn->decoder_table);
        free(conn);
        return NULL;
    }
    
    return conn;
}

/* ========== Stream Priority ========== */

int xr_h2_set_priority(XrH2Conn *conn, XrH2Stream *stream, const XrH2Priority *priority) {
    if (!conn || !stream || !priority) return -1;
    
    stream->priority = *priority;
    return xr_h2_send_priority(conn, stream->id, priority);
}

int xr_h2_send_priority(XrH2Conn *conn, uint32_t stream_id, const XrH2Priority *priority) {
    if (!conn || !priority) return -1;
    
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 5];
    XrH2FrameHeader header = {
        .length = 5,
        .type = XR_H2_FRAME_PRIORITY,
        .flags = 0,
        .stream_id = stream_id
    };
    xr_h2_write_frame_header(frame, &header);
    
    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    uint32_t dep = priority->dependency;
    if (priority->exclusive) dep |= 0x80000000;
    
    payload[0] = (dep >> 24) & 0xFF;
    payload[1] = (dep >> 16) & 0xFF;
    payload[2] = (dep >> 8) & 0xFF;
    payload[3] = dep & 0xFF;
    payload[4] = priority->weight - 1;  // weight 1-256 -> 0-255
    
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 5);
}

/* ========== Stream Cancellation ========== */

int xr_h2_cancel_stream(XrH2Conn *conn, XrH2Stream *stream, XrH2ErrorCode error) {
    if (!conn || !stream) return -1;
    
    stream->cancelled = true;
    stream->state = XR_H2_STREAM_STATE_CLOSED;
    
    return xr_h2_send_rst_stream(conn, stream->id, error);
}

int xr_h2_send_rst_stream(XrH2Conn *conn, uint32_t stream_id, XrH2ErrorCode error) {
    if (!conn) return -1;
    
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 4];
    XrH2FrameHeader header = {
        .length = 4,
        .type = XR_H2_FRAME_RST_STREAM,
        .flags = 0,
        .stream_id = stream_id
    };
    xr_h2_write_frame_header(frame, &header);
    
    uint8_t *payload = frame + XR_H2_FRAME_HEADER_SIZE;
    payload[0] = (error >> 24) & 0xFF;
    payload[1] = (error >> 16) & 0xFF;
    payload[2] = (error >> 8) & 0xFF;
    payload[3] = error & 0xFF;
    
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 4);
}

/* ========== Trailers ========== */

int xr_h2_send_trailers(XrH2Conn *conn, XrH2Stream *stream,
                         const char **names, const size_t *name_lens,
                         const char **values, const size_t *value_lens,
                         int count) {
    if (!conn || !stream) return -1;
    
    // Encode trailer headers
    uint8_t headers_buf[16384];
    int headers_len = 0;
    
    for (int i = 0; i < count; i++) {
        int len = xr_hpack_encode(&conn->encoder_table,
                                   names[i], name_lens[i],
                                   values[i], value_lens[i],
                                   headers_buf + headers_len,
                                   sizeof(headers_buf) - headers_len);
        if (len < 0) return -1;
        headers_len += len;
    }
    
    // Send HEADERS frame with END_STREAM
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 16384];
    XrH2FrameHeader header = {
        .length = headers_len,
        .type = XR_H2_FRAME_HEADERS,
        .flags = XR_H2_FLAG_END_HEADERS | XR_H2_FLAG_END_STREAM,
        .stream_id = stream->id
    };
    xr_h2_write_frame_header(frame, &header);
    memcpy(frame + XR_H2_FRAME_HEADER_SIZE, headers_buf, headers_len);
    
    stream->state = XR_H2_STREAM_HALF_CLOSED_LOCAL;
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + headers_len);
}

/* ========== PING ========== */

int xr_h2_send_ping(XrH2Conn *conn, const uint8_t data[8], bool ack) {
    if (!conn) return -1;
    
    uint8_t frame[XR_H2_FRAME_HEADER_SIZE + 8];
    XrH2FrameHeader header = {
        .length = 8,
        .type = XR_H2_FRAME_PING,
        .flags = ack ? XR_H2_FLAG_ACK : 0,
        .stream_id = 0
    };
    xr_h2_write_frame_header(frame, &header);
    
    if (data) {
        memcpy(frame + XR_H2_FRAME_HEADER_SIZE, data, 8);
    } else {
        memset(frame + XR_H2_FRAME_HEADER_SIZE, 0, 8);
    }
    
    return h2_send(conn, frame, XR_H2_FRAME_HEADER_SIZE + 8);
}

/* ========== h2c (HTTP/2 Cleartext) ========== */

int xr_h2_upgrade_from_http1(XrH2Conn *conn, const char *settings_payload, size_t len) {
    if (!conn) return -1;
    
    // Parse Base64 encoded SETTINGS
    (void)settings_payload;
    (void)len;
    
    // Send server connection preface
    if (!conn->is_client) {
        if (xr_h2_send_settings(conn) < 0) return -1;
    }
    
    conn->settings_sent = true;
    return 0;
}

int xr_h2_start_h2c(XrH2Conn *conn) {
    if (!conn) return -1;
    
    // h2c Prior Knowledge: send connection preface directly
    if (conn->is_client) {
        if (h2_send(conn, XR_HTTP2_PREFACE, XR_HTTP2_PREFACE_LEN) < 0) {
            return -1;
        }
    }
    
    // Send SETTINGS frame
    if (xr_h2_send_settings(conn) < 0) {
        return -1;
    }
    
    conn->settings_sent = true;
    return 0;
}
