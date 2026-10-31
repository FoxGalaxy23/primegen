#ifndef MLB_MEDIA_BYTE_QUEUE_H
#define MLB_MEDIA_BYTE_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A bounded single-producer/single-consumer byte ring for streaming media input.
 *
 * The producer may be the MSE append path or a network reader; the consumer is
 * intended to be the demuxer/input adapter. Calls from multiple producers or
 * multiple consumers must be externally serialized. Data is never overwritten.
 *
 * write() is all-or-nothing: 1 = all bytes accepted, 0 = insufficient space,
 * -1 = closed/failed/invalid. This lets appendBuffer fail with quota pressure
 * instead of silently accepting only a prefix of a media segment.
 *
 * read(): >0 bytes read; -1 = temporarily empty/open; 0 = EOF after close and
 * drain; -2 = failed after queued bytes have been drained; -3 = invalid args.
 */
typedef struct MediaByteQueue MediaByteQueue;

#define MEDIA_BYTE_QUEUE_DEFAULT_CAPACITY (8u * 1024u * 1024u)
#define MEDIA_BYTE_QUEUE_MAX_CAPACITY     (64u * 1024u * 1024u)

MediaByteQueue *media_byte_queue_create(size_t capacity);
void media_byte_queue_destroy(MediaByteQueue *queue);
int media_byte_queue_write(MediaByteQueue *queue, const void *data, size_t length);
ptrdiff_t media_byte_queue_read(MediaByteQueue *queue, void *out, size_t capacity);
void media_byte_queue_close(MediaByteQueue *queue);
void media_byte_queue_fail(MediaByteQueue *queue);
size_t media_byte_queue_available(const MediaByteQueue *queue);
size_t media_byte_queue_free_space(const MediaByteQueue *queue);
size_t media_byte_queue_capacity(const MediaByteQueue *queue);
int media_byte_queue_is_closed(const MediaByteQueue *queue);
int media_byte_queue_is_failed(const MediaByteQueue *queue);

#ifdef __cplusplus
}
#endif
#endif
