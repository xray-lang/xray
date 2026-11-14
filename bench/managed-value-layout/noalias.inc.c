/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * noalias.inc.c - Capacity-bounded C11 restrict windows on canonical storage
 *
 * KEY CONCEPT:
 *   Owner and data are separate live allocations. Within each nested block,
 *   modified elements and canonical length use only their respective restricted
 *   pointer. No call, growth, retirement or restricted pointer escapes the block.
 */
static XrMvlArrayWrite mvl_array_push_noalias_range(XrMvlArray *owner, int64_t count) {
    XrMvlStatus status = XR_MVL_OK;
    int64_t *data = NULL;
    size_t length = 0, capacity = 0;
    if (count > 0) {
        int64_t value = 0;
        XrMvlArrayWrite write = xr_mvl_array_access_write(xr_mvl_array_access_start(owner), 0, value, true);
        if (write.status != XR_MVL_OK)
            return (XrMvlArrayWrite) {write.status, xr_mvl_array_access_start(owner), NULL};
        owner = write.access.owner;
        data = write.access.data;
        length = write.access.length;
        capacity = write.access.capacity;
        if (write.previous) xr_mvl_drop(&write.previous->header);
    }
    int64_t next = 1;
    while (next < count) {
        {
            XrMvlArray *restrict block_owner = owner;
            int64_t *restrict block_data = data;
            while (next < count && length < capacity) {
                int64_t value = next;
                block_data[length++] = value;
                block_owner->length = length;
                ++next;
            }
        }
        if (next == count)
            break;
        /* Only unrestricted scalar copies survive the preceding block. A cold
         * write can replace storage; its result starts the next fresh window. */
        int64_t value = next;
        XrMvlArrayAccess access = {owner, data, length, capacity, true};
        XrMvlArrayWrite write = xr_mvl_array_access_write(access, 0, value, true);
        if (write.status != XR_MVL_OK) {
            status = write.status;
            break;
        }
        owner = write.access.owner;
        data = write.access.data;
        length = write.access.length;
        capacity = write.access.capacity;
        if (write.previous) xr_mvl_drop(&write.previous->header);
        ++next;
    }
    return (XrMvlArrayWrite) {status, xr_mvl_array_access_start(owner), NULL};
}
