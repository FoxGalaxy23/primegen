#include "media_byte_queue.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct MediaByteQueue {
    size_t capacity;
    unsigned char *data;
    atomic_uint_fast64_t write_pos;
    atomic_uint_fast64_t read_pos;
    atomic_int closed;
    atomic_int failed;
};

MediaByteQueue *media_byte_queue_create(size_t capacity) {
    if (capacity == 0 || capacity > MEDIA_BYTE_QUEUE_MAX_CAPACITY) return NULL;
    MediaByteQueue *q = (MediaByteQueue *)calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->data = (unsigned char *)malloc(capacity);
    if (!q->data) { free(q); return NULL; }
    q->capacity = capacity;
    atomic_init(&q->write_pos, 0);
    atomic_init(&q->read_pos, 0);
    atomic_init(&q->closed, 0);
    atomic_init(&q->failed, 0);
    return q;
}

void media_byte_queue_destroy(MediaByteQueue *q) {
    if (!q) return;
    free(q->data);
    free(q);
}

int media_byte_queue_write(MediaByteQueue *q, const void *data, size_t length) {
    if (!q || (!data && length != 0)) return -1;
    if (length == 0) return 1;
    if (atomic_load_explicit(&q->closed, memory_order_acquire) ||
        atomic_load_explicit(&q->failed, memory_order_acquire)) return -1;
    if (length > q->capacity) return 0;

    uint64_t w = atomic_load_explicit(&q->write_pos, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&q->read_pos, memory_order_acquire);
    uint64_t used64 = w - r;
    if (used64 > q->capacity) return -1; /* defensive: corrupted queue state */
    size_t used = (size_t)used64;
    if (length > q->capacity - used) return 0;

    size_t at = (size_t)(w % q->capacity);
    size_t first = q->capacity - at;
    if (first > length) first = length;
    memcpy(q->data + at, data, first);
    if (length > first) memcpy(q->data, (const unsigned char *)data + first, length - first);
    /* Publish bytes only after both wrapped copies are complete. */
    atomic_store_explicit(&q->write_pos, w + length, memory_order_release);
    return 1;
}

ptrdiff_t media_byte_queue_read(MediaByteQueue *q, void *out, size_t capacity) {
    if (!q || (!out && capacity != 0) || capacity == 0) return -3;
    uint64_t r = atomic_load_explicit(&q->read_pos, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&q->write_pos, memory_order_acquire);
    uint64_t available64 = w - r;
    if (available64 > q->capacity) return -3;
    if (available64 != 0) {
        size_t count = (size_t)available64;
        if (count > capacity) count = capacity;
        size_t at = (size_t)(r % q->capacity);
        size_t first = q->capacity - at;
        if (first > count) first = count;
        memcpy(out, q->data + at, first);
        if (count > first) memcpy((unsigned char *)out + first, q->data, count - first);
        /* Release consumed slots only after their contents were copied. */
        atomic_store_explicit(&q->read_pos, r + count, memory_order_release);
        return (ptrdiff_t)count;
    }
    if (atomic_load_explicit(&q->failed, memory_order_acquire)) return -2;
    if (atomic_load_explicit(&q->closed, memory_order_acquire)) return 0;
    return -1;
}

void media_byte_queue_close(MediaByteQueue *q) {
    if (q) atomic_store_explicit(&q->closed, 1, memory_order_release);
}

void media_byte_queue_fail(MediaByteQueue *q) {
    if (!q) return;
    atomic_store_explicit(&q->failed, 1, memory_order_release);
    atomic_store_explicit(&q->closed, 1, memory_order_release);
}

size_t media_byte_queue_available(const MediaByteQueue *q) {
    if (!q) return 0;
    uint64_t w = atomic_load_explicit(&q->write_pos, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&q->read_pos, memory_order_acquire);
    uint64_t n = w - r;
    return n > q->capacity ? 0 : (size_t)n;
}

size_t media_byte_queue_free_space(const MediaByteQueue *q) {
    if (!q) return 0;
    size_t n = media_byte_queue_available(q);
    return n > q->capacity ? 0 : q->capacity - n;
}

size_t media_byte_queue_capacity(const MediaByteQueue *q) { return q ? q->capacity : 0; }
int media_byte_queue_is_closed(const MediaByteQueue *q) { return q ? atomic_load_explicit(&q->closed, memory_order_acquire) : 1; }
int media_byte_queue_is_failed(const MediaByteQueue *q) { return q ? atomic_load_explicit(&q->failed, memory_order_acquire) : 1; }
