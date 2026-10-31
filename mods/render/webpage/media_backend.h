#ifndef MLB_MEDIA_BACKEND_H
#define MLB_MEDIA_BACKEND_H
#include <stddef.h>
#include <stdint.h>
#include "media_byte_queue.h"
typedef struct { int width, height, has_video, has_audio, can_seek; double duration; } MediaInfo;
typedef struct MediaBackend MediaBackend;
typedef enum {
    MEDIA_BACKEND_OPENING = 0,
    MEDIA_BACKEND_RUNNING = 1,
    MEDIA_BACKEND_DRAINING = 2,
    MEDIA_BACKEND_ENDED = 3,
    MEDIA_BACKEND_FAILED = 4
} MediaBackendState;
#define MEDIA_MAX_BUFFERED_RANGES 16
typedef struct {
    MediaBackendState state;
    int can_seek;
    double duration;
    /* Native media clock. current_time advances from consumed SDL PCM samples
       when available, otherwise from the video PTS pacing clock. */
    int has_clock;
    int audio_clock; /* true when current_time is driven by the SDL PCM callback clock */
    double current_time;
    /* Conservative software estimate: one negotiated SDL callback period. This is not a DAC timestamp. */
    double audio_output_latency_estimate_seconds;
    uint64_t audio_frames_consumed; /* stereo sample-frames credited by callback progress */
    uint64_t audio_underrun_frames; /* silence output while more decoded PCM was expected */
    uint64_t video_frames_dropped; /* stale PTS frames skipped to recover A/V sync */
    int has_first_video_frame_time; /* first decoded video frame mapped to media timeline */
    double first_video_frame_time;
    int has_video_frame_time; /* most recently delivered RGBA frame mapped to media timeline */
    double video_frame_time;
    size_t audio_queue_bytes; /* PCM waiting in the software SPSC ring, excludes one in-flight SDL block */
    int live_volume_control; /* true when volume is applied in the SDL callback without decoder restart */
    double audio_volume; /* current callback gain, in [0, 1] */
    size_t buffered_count;
    double buffered_start[MEDIA_MAX_BUFFERED_RANGES];
    double buffered_end[MEDIA_MAX_BUFFERED_RANGES];
} MediaBackendStatus;
int media_probe(const char *source, MediaInfo *out);
/* Return the latest indexed video keyframe at or before target for a seekable local file.
   Falls back to target when keyframe discovery is unavailable. */
double media_fast_seek_target(const char *source, double target);
#ifdef MEDIA_FAST_SEEK_TESTING
/* Test-build-only counter used to assert cache reuse without timing assumptions. */
unsigned media_fast_seek_test_probe_count(void);
#endif
MediaBackend *media_start(const char *source, double start_seconds, double volume, int want_video, int max_width, int max_height, const MediaInfo *known_info);
/* Playback-rate-aware entry point. Rates from 1/16 through 4 are supported by
   FFmpeg atempo for audio and the native PTS scheduler for video. */
MediaBackend *media_start_rate(const char *source, double start_seconds, double volume, double playback_rate, int want_video, int max_width, int max_height, const MediaInfo *known_info);
/* Start decoding a non-seekable encoded byte stream from a bounded queue.
   The caller owns input and must keep it alive until media_stop(). Each poll
   pumps at most 256 KiB into nonblocking demuxer stdin sockets. Input should be
   a streamable container (for MP4, fragmented MP4 with an initialization segment).
   This API does not claim MSE support; it is the native stream-input adapter. */
MediaBackend *media_start_stream(MediaByteQueue *input, double volume, int want_video, int want_audio, int max_width, int max_height);
/* Same stream input, but discard decoded output until start_seconds. The input must
   include its init segment and a random-access fragment preceding that timestamp. */
MediaBackend *media_start_stream_at(MediaByteQueue *input, double volume, int want_video, int want_audio, int max_width, int max_height, double start_seconds);
MediaBackend *media_start_stream_at_rate(MediaByteQueue *input, double volume, int want_video, int want_audio, int max_width, int max_height, double start_seconds, double playback_rate);
/* 1 = frame produced, 0 = no complete frame yet, -1 = end/error. The returned
   RGBA pointer remains owned by backend and is valid until the next call. */
int media_poll(MediaBackend *backend, const unsigned char **rgba, int *width, int *height);
/* Status is deliberately conservative: buffered_count is zero until the backend can
   report exact media-time intervals (decoded wall-clock time is not a buffer range). */
int media_get_status(const MediaBackend *backend, MediaBackendStatus *out);
/* Update native SDL PCM gain atomically without restarting the decoder. Returns 0
   when playback uses the external ffplay fallback or no internal audio device. */
int media_set_volume(MediaBackend *backend, double volume);
/* Pause/resume the same decoder processes and SDL device without discarding PCM or frame state. */
int media_pause(MediaBackend *backend);
int media_resume(MediaBackend *backend);
void media_stop(MediaBackend *backend);
int media_available(void);
#endif
