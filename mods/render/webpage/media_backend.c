#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "media_backend.h"
#include "media_byte_queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#else
#include <windows.h>
#endif

#ifndef _WIN32
typedef uint32_t MediaSDL_AudioDeviceID;
typedef struct {
    int freq; uint16_t format; uint8_t channels, silence; uint16_t samples, padding;
    uint32_t size; void (*callback)(void*, uint8_t*, int); void* userdata;
} MediaSDL_AudioSpec;
typedef int (*MediaSDL_InitSubSystemFn)(uint32_t);
typedef MediaSDL_AudioDeviceID (*MediaSDL_OpenAudioDeviceFn)(const char*, int, const MediaSDL_AudioSpec*, MediaSDL_AudioSpec*, int);
typedef void (*MediaSDL_PauseAudioDeviceFn)(MediaSDL_AudioDeviceID, int);
typedef void (*MediaSDL_CloseAudioDeviceFn)(MediaSDL_AudioDeviceID);
#endif

struct MediaBackend {
#ifndef _WIN32
    pid_t audio_pid, video_pid;
    int audio_pcm_fd, frame_fd, frame_meta_fd;
    MediaSDL_AudioDeviceID audio_dev;
#else
    int unused;
#endif
    int width, height, eof;
    double playback_rate;
    int paused;
    double pause_started;
    MediaBackendState state;
    int can_seek;
    double duration;
    size_t frame_bytes, have;
    unsigned char *frame;
#ifndef _WIN32
    /* FFmpeg showinfo reports the presentation timestamp for each raw RGBA frame.
       Keep the metadata side-channel separate from frame bytes; never guess a
       timestamp from renderer polling cadence. */
    char meta_line[4096];
    size_t meta_line_len;
    double pts_queue[256];
    size_t pts_head, pts_count;
    int meta_eof, timing_failed, frame_pts_valid, clock_started;
    double frame_pts, first_pts, last_delivered_pts, clock_start;
    int have_last_pts;
    int first_video_time_valid, video_time_valid;
    double first_video_time, last_video_time, prepared_video_time;
    uint64_t video_frames_dropped;
    int has_audio, audio_external, audio_pcm_eof, audio_process_done, audio_process_ok;
    int audio_failed, audio_clock_fallback;
    _Atomic int audio_clock_started;
    unsigned char *audio_ring;
    size_t audio_ring_capacity;
    _Atomic uint64_t audio_ring_read, audio_ring_write;
    _Atomic uint64_t audio_consumed_bytes, audio_underrun_frames;
    double audio_output_latency_estimate_seconds;
    _Atomic uint32_t audio_pending_bytes;
    _Atomic uint32_t audio_volume_q16; /* callback-side gain, 0..65536 */
    _Atomic int audio_input_complete;
    unsigned char audio_tail[4]; size_t audio_tail_len;
    double media_start_time, audio_fallback_elapsed, audio_fallback_wall;
    int video_process_done, video_process_ok, video_failed;
    /* Stream input fd order: 0 = audio decoder, 1 = video decoder. The queue is
       borrowed from the caller; media_stop() never destroys it. */
    int stream_mode;
    int input_fd[2];
    MediaByteQueue *stream_input;
    unsigned char stream_pending[16384];
    size_t stream_pending_len, stream_pending_off[2];
    int stream_input_eof, stream_input_failed, stream_input_started;
#endif
};

#ifndef _WIN32
#define MEDIA_PTS_QUEUE_CAP 256
#define MEDIA_AUDIO_RATE 44100
#define MEDIA_AUDIO_CHANNELS 2
#define MEDIA_AUDIO_BYTES_PER_FRAME 4u /* signed 16-bit stereo */
#define MEDIA_AUDIO_QUEUE_LIMIT ((size_t)MEDIA_AUDIO_RATE * MEDIA_AUDIO_BYTES_PER_FRAME / 3u)
#define MEDIA_AUDIO_RING_CAPACITY 65536u /* power of two, SPSC */

static double media_monotonic_seconds(void);

typedef struct {
    void *library;
    int attempted, init_ok;
    MediaSDL_InitSubSystemFn init_subsystem;
    MediaSDL_OpenAudioDeviceFn open_device;
    MediaSDL_PauseAudioDeviceFn pause_device;
    MediaSDL_CloseAudioDeviceFn close_device;
} MediaSDLApi;
static MediaSDLApi g_media_sdl;

static int media_sdl_load(void) {
    MediaSDLApi *a = &g_media_sdl;
    if (!a->attempted) {
        a->attempted = 1;
        a->library = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!a->library) a->library = dlopen("libSDL2.so", RTLD_NOW | RTLD_LOCAL);
        if (!a->library) return 0;
#define MEDIA_SDL_SYM(dst, name) do { *(void **)(&(dst)) = dlsym(a->library, (name)); } while (0)
        MEDIA_SDL_SYM(a->init_subsystem, "SDL_InitSubSystem");
        MEDIA_SDL_SYM(a->open_device, "SDL_OpenAudioDevice");
        MEDIA_SDL_SYM(a->pause_device, "SDL_PauseAudioDevice");
        MEDIA_SDL_SYM(a->close_device, "SDL_CloseAudioDevice");
#undef MEDIA_SDL_SYM
        if (!a->init_subsystem || !a->open_device || !a->pause_device || !a->close_device) return 0;
    }
    if (!a->library) return 0;
    if (!a->init_ok) {
        if (a->init_subsystem(0x00000010u) != 0) return 0; /* SDL_INIT_AUDIO */
        a->init_ok = 1;
    }
    return 1;
}

static void media_sdl_audio_callback(void *userdata, uint8_t *stream, int len) {
    MediaBackend *b = (MediaBackend *)userdata;
    if (!stream || len <= 0) return;
    memset(stream, 0, (size_t)len); /* signed 16-bit PCM silence */
    if (!b || !b->audio_ring || !b->audio_ring_capacity) return;

    /* Credit the previous callback's real PCM bytes. The current callback is
       handed to SDL's device after it returns, so this deliberately avoids
       advancing the public clock as soon as bytes merely leave the software ring. */
    uint32_t pending = atomic_load_explicit(&b->audio_pending_bytes, memory_order_relaxed);
    if (pending) atomic_fetch_add_explicit(&b->audio_consumed_bytes, pending, memory_order_relaxed);

    uint64_t r = atomic_load_explicit(&b->audio_ring_read, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&b->audio_ring_write, memory_order_acquire);
    uint64_t available64 = w - r;
    size_t available = available64 > b->audio_ring_capacity ? b->audio_ring_capacity : (size_t)available64;
    size_t wanted = (size_t)len;
    size_t copied = available < wanted ? available : wanted;
    copied &= ~(size_t)(MEDIA_AUDIO_BYTES_PER_FRAME - 1u);
    if (copied) {
        size_t at = (size_t)(r & (uint64_t)(b->audio_ring_capacity - 1u));
        size_t first = copied;
        if (first > b->audio_ring_capacity - at) first = b->audio_ring_capacity - at;
        memcpy(stream, b->audio_ring + at, first);
        if (copied > first) memcpy(stream + first, b->audio_ring, copied - first);
        /* Apply gain at the last software-controlled point. Doing this in the
           callback means volume/mute changes do not need to restart FFmpeg or
           reset the sample clock. SDL is negotiated as S16LSB stereo. */
        uint32_t gain = atomic_load_explicit(&b->audio_volume_q16, memory_order_relaxed);
        if (gain < 65536u) {
            for (size_t i = 0; i + 1 < copied; i += 2) {
                uint32_t u = (uint32_t)stream[i] | ((uint32_t)stream[i + 1] << 8);
                int32_t sample = (u & 0x8000u) ? (int32_t)u - 65536 : (int32_t)u;
                int64_t scaled = ((int64_t)sample * (int64_t)gain) / 65536;
                uint16_t enc = (uint16_t)(int16_t)scaled;
                stream[i] = (uint8_t)(enc & 0xffu);
                stream[i + 1] = (uint8_t)(enc >> 8);
            }
        }
        atomic_store_explicit(&b->audio_ring_read, r + copied, memory_order_release);
        atomic_store_explicit(&b->audio_pending_bytes, (uint32_t)copied, memory_order_release);
        atomic_store_explicit(&b->audio_clock_started, 1, memory_order_release);
    } else {
        atomic_store_explicit(&b->audio_pending_bytes, 0, memory_order_release);
    }
    if ((size_t)len > copied &&
        atomic_load_explicit(&b->audio_clock_started, memory_order_relaxed) &&
        !atomic_load_explicit(&b->audio_input_complete, memory_order_acquire)) {
        uint64_t missing_frames = ((size_t)len - copied) / MEDIA_AUDIO_BYTES_PER_FRAME;
        if (missing_frames) atomic_fetch_add_explicit(&b->audio_underrun_frames, missing_frames, memory_order_relaxed);
    }
}

static MediaSDL_AudioDeviceID media_audio_open_device(MediaBackend *b) {
    if (!b || !media_sdl_load()) return 0;
    b->audio_ring = (unsigned char *)malloc(MEDIA_AUDIO_RING_CAPACITY);
    if (!b->audio_ring) return 0;
    b->audio_ring_capacity = MEDIA_AUDIO_RING_CAPACITY;
    MediaSDL_AudioSpec want, got;
    memset(&want, 0, sizeof want); memset(&got, 0, sizeof got);
    want.freq = MEDIA_AUDIO_RATE;
    want.format = 0x8010; /* AUDIO_S16LSB */
    want.channels = MEDIA_AUDIO_CHANNELS;
    want.samples = 1024;
    want.callback = media_sdl_audio_callback;
    want.userdata = b;
    MediaSDL_AudioDeviceID dev = g_media_sdl.open_device(NULL, 0, &want, &got, 0);
    if (!dev) { free(b->audio_ring); b->audio_ring = NULL; b->audio_ring_capacity = 0; return 0; }
    if (got.freq != want.freq || got.channels != want.channels || got.format != want.format) {
        g_media_sdl.close_device(dev); free(b->audio_ring); b->audio_ring = NULL; b->audio_ring_capacity = 0; return 0;
    }
    /* The negotiated callback period is a conservative low-end estimate of
       output latency. SDL does not expose a portable hardware/DAC timestamp;
       the actual host/device pipeline may be longer. */
    if (got.samples > 0 && got.freq > 0) {
        b->audio_output_latency_estimate_seconds = (double)got.samples / (double)got.freq;
        if (b->audio_output_latency_estimate_seconds > 0.100) b->audio_output_latency_estimate_seconds = 0.100;
    } else {
        b->audio_output_latency_estimate_seconds = (double)want.samples / (double)want.freq;
    }
    /* Opening SDL audio devices starts paused. Unpause only after userdata and
       all atomic state have been initialized. */
    g_media_sdl.pause_device(dev, 0);
    return dev;
}

static uint32_t media_audio_queued_bytes(const MediaBackend *b) {
    if (!b || !b->audio_dev || !b->audio_ring_capacity) return 0;
    uint64_t w = atomic_load_explicit(&b->audio_ring_write, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&b->audio_ring_read, memory_order_acquire);
    uint64_t used = w - r;
    if (used > b->audio_ring_capacity) used = b->audio_ring_capacity;
    return (uint32_t)used;
}

static int media_audio_ring_write(MediaBackend *b, const unsigned char *src, size_t len) {
    if (!b || !src || !b->audio_ring || !b->audio_ring_capacity || (len & 3u)) return 0;
    uint64_t w = atomic_load_explicit(&b->audio_ring_write, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&b->audio_ring_read, memory_order_acquire);
    uint64_t used64 = w - r;
    if (used64 > b->audio_ring_capacity) return 0;
    size_t used = (size_t)used64;
    if (len > b->audio_ring_capacity - used) return 0;
    size_t at = (size_t)(w & (uint64_t)(b->audio_ring_capacity - 1u));
    size_t first = len;
    if (first > b->audio_ring_capacity - at) first = b->audio_ring_capacity - at;
    memcpy(b->audio_ring + at, src, first);
    if (len > first) memcpy(b->audio_ring, src + first, len - first);
    atomic_store_explicit(&b->audio_ring_write, w + len, memory_order_release);
    return 1;
}

static double media_audio_output_seconds(const MediaBackend *b) {
    if (!b || !b->audio_dev) return 0.0;
    uint64_t consumed = atomic_load_explicit(&b->audio_consumed_bytes, memory_order_acquire);
    return (double)consumed / ((double)MEDIA_AUDIO_RATE * (double)MEDIA_AUDIO_BYTES_PER_FRAME);
}

/* Audio progress after subtracting the negotiated callback period. The result
   may be negative at startup; the scheduler uses that negative phase to avoid
   presenting the first video frame before the corresponding audio reaches the
   output path. The estimate is deliberately not described as hardware latency. */
static double media_audio_sync_elapsed(const MediaBackend *b) {
    if (!b || !b->audio_dev || !atomic_load_explicit(&b->audio_clock_started, memory_order_acquire)) return 0.0;
    double rate = (isfinite(b->playback_rate) && b->playback_rate >= 0.0625 && b->playback_rate <= 4.0) ? b->playback_rate : 1.0;
    double elapsed = (media_audio_output_seconds(b) - b->audio_output_latency_estimate_seconds) * rate;
    if (b->audio_clock_fallback) {
        double now = b->paused ? b->pause_started : media_monotonic_seconds();
        double add = now - b->audio_fallback_wall;
        if (add < 0.0) add = 0.0;
        elapsed = b->audio_fallback_elapsed + add * rate;
    }
    return elapsed;
}

static double media_audio_media_elapsed(const MediaBackend *b) {
    double elapsed = media_audio_sync_elapsed(b);
    /* currentTime cannot precede the requested start point; video scheduling
       separately uses the signed sync clock above to preserve initial phase. */
    if (!isfinite(elapsed) || elapsed < 0.0) return 0.0;
    return elapsed;
}

static void media_audio_close_device(MediaBackend *b) {
    if (!b) return;
    if (b->audio_dev) {
        /* Pause first, then close: SDL_CloseAudioDevice joins the callback thread
           before the ring buffer can be freed. */
        if (g_media_sdl.pause_device) g_media_sdl.pause_device(b->audio_dev, 1);
        if (g_media_sdl.close_device) g_media_sdl.close_device(b->audio_dev);
        b->audio_dev = 0;
    }
    free(b->audio_ring);
    b->audio_ring = NULL;
    b->audio_ring_capacity = 0;
}

static double media_monotonic_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void media_pts_push(MediaBackend *b, double pts) {
    if (!b) return;
    if (b->pts_count >= MEDIA_PTS_QUEUE_CAP) { b->timing_failed = 1; return; }
    size_t at = (b->pts_head + b->pts_count) % MEDIA_PTS_QUEUE_CAP;
    b->pts_queue[at] = pts;
    b->pts_count++;
}

static void media_parse_meta_line(MediaBackend *b) {
    if (!b || !b->meta_line_len) return;
    b->meta_line[b->meta_line_len] = 0;
    /* Ignore ordinary FFmpeg info/error messages. Only showinfo frame lines
       have both a frame number and pts_time. Queue NaN for explicit N/A PTS so
       metadata and raw frames retain one-to-one correspondence. */
    if (strstr(b->meta_line, "showinfo") && strstr(b->meta_line, "pts_time:")) {
        const char *p = strstr(b->meta_line, "pts_time:") + 9;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        double pts = NAN;
        if (strncmp(p, "N/A", 3) != 0) {
            double v = strtod(p, &end);
            if (end != p && isfinite(v)) pts = v;
        }
        media_pts_push(b, pts);
    }
    b->meta_line_len = 0;
}

static void media_drain_frame_metadata(MediaBackend *b) {
    if (!b || b->frame_meta_fd < 0 || b->meta_eof) return;
    char buf[8192];
    size_t budget = 65536;
    while (budget) {
        size_t ask = sizeof buf;
        if (ask > budget) ask = budget;
        ssize_t n = read(b->frame_meta_fd, buf, ask);
        if (n > 0) {
            budget -= (size_t)n;
            for (ssize_t i = 0; i < n; i++) {
                char ch = buf[i];
                if (ch == '\n') { media_parse_meta_line(b); continue; }
                if (b->meta_line_len + 1 < sizeof b->meta_line) b->meta_line[b->meta_line_len++] = ch;
                else { b->meta_line_len = 0; /* Ignore oversized non-frame log lines. */ }
            }
            continue;
        }
        if (n == 0) {
            if (b->meta_line_len) media_parse_meta_line(b);
            b->meta_eof = 1;
            close(b->frame_meta_fd); b->frame_meta_fd = -1;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        b->meta_eof = 1;
        close(b->frame_meta_fd); b->frame_meta_fd = -1;
        return;
    }
}

static int media_prepare_frame_deadline(MediaBackend *b) {
    if (!b || b->have != b->frame_bytes) return 0;
    if (!b->frame_pts_valid) {
        if (!b->pts_count) {
            if (b->timing_failed || b->meta_eof) { b->state = MEDIA_BACKEND_FAILED; return -1; }
            return 0; /* Wait for the matching showinfo timestamp. */
        }
        b->frame_pts = b->pts_queue[b->pts_head];
        b->pts_head = (b->pts_head + 1) % MEDIA_PTS_QUEUE_CAP;
        b->pts_count--;
        if (!isfinite(b->frame_pts)) {
            /* Some demuxers omit PTS. Use an explicit conservative fallback,
               anchored to the last delivered PTS, rather than poll-wall-time. */
            b->frame_pts = b->have_last_pts ? b->last_delivered_pts + (1.0 / 30.0) : 0.0;
        }
        /* Hold on to this timestamp even if audio has not queued its first
           PCM block yet. Otherwise the next poll would pop a second PTS for
           the same RGBA frame and shift all later frame/timestamp pairs. */
        b->frame_pts_valid = 1;
    }
    if (!b->clock_started) {
        /* When PCM is available, do not start the video timeline before the
           audio device has received its first sample. This makes consumed
           audio frames, not a second wall clock, the A/V scheduling source. */
        if (b->audio_dev && !atomic_load_explicit(&b->audio_clock_started, memory_order_acquire)) return 0;
        /* For regular-file input seeking, FFmpeg's input-side -ss performs
           accurate seek and keeps the discarded pre-roll offset in showinfo
           PTS (e.g. a seek to 2.15s can yield the first frame at pts_time 0.05).
           Keep that offset relative to the requested timeline instead of
           normalizing the first decoded frame to zero. Stream/MSE input has
           its own replay origin and retains the previous relative baseline. */
        b->first_pts = (!b->stream_mode && b->media_start_time > 0.001) ? 0.0 : b->frame_pts;
        b->clock_start = media_monotonic_seconds();
        b->clock_started = 1;
    }
    double rel = b->frame_pts - b->first_pts;
    if (!isfinite(rel) || rel < 0.0) rel = 0.0;
    b->prepared_video_time = b->media_start_time + rel;
    if (!b->first_video_time_valid) {
        b->first_video_time = b->prepared_video_time;
        b->first_video_time_valid = 1;
    }
    if (!isfinite(rel) || rel < 0.0) rel = 0.0;
    double rate = (isfinite(b->playback_rate) && b->playback_rate >= 0.0625 && b->playback_rate <= 4.0) ? b->playback_rate : 1.0;
    if (b->audio_dev && atomic_load_explicit(&b->audio_clock_started, memory_order_acquire)) {
        /* PTS is compared against samples consumed by SDL's device queue. The
           queue is per MediaBackend, so unrelated WebAudio cannot skew this clock. */
        double audio_elapsed = media_audio_sync_elapsed(b);
        /* Allow half a callback period of scheduling tolerance, capped so an
           unusually large SDL buffer cannot make video visibly lead audio. */
        double tolerance = b->audio_output_latency_estimate_seconds * 0.5;
        if (tolerance < 0.005) tolerance = 0.005;
        if (tolerance > 0.015) tolerance = 0.015;
        return rel <= audio_elapsed + tolerance ? 1 : 0;
    }
    double deadline = b->clock_start + rel / rate;
    return media_monotonic_seconds() + 0.001 >= deadline ? 1 : 0;
}

/* Drop decoded frames that are too stale relative to the master audio clock.
   A bounded lateness threshold avoids spending the rest of playback presenting
   old frames after the renderer or decoder falls behind. This is only applied
   when the SDL PCM clock is active; video-only playback keeps every frame. */
static int media_frame_is_stale(MediaBackend *b) {
    if (!b || !b->audio_dev || !b->clock_started ||
        !atomic_load_explicit(&b->audio_clock_started, memory_order_acquire) ||
        !isfinite(b->frame_pts) || !isfinite(b->first_pts)) return 0;
    double rel = b->frame_pts - b->first_pts;
    if (!isfinite(rel) || rel < 0.0) rel = 0.0;
    double audio_elapsed = media_audio_sync_elapsed(b);
    double lateness = audio_elapsed - rel;
    double limit = 0.080; /* Do not drop frames for ordinary callback jitter. */
    if (b->have_last_pts) {
        double step = b->frame_pts - b->last_delivered_pts;
        if (isfinite(step) && step > 0.001 && step < 1.0) {
            limit = step * 1.5;
            if (limit < 0.080) limit = 0.080;
            if (limit > 0.250) limit = 0.250;
        }
    }
    return lateness > limit;
}

static int media_reap_child(pid_t *slot, int *done, int *ok) {
    if (!slot || !done || !ok) return 0;
    if (*done) return 1;
    if (*slot <= 0) return 0;
    int status = 0; pid_t r;
    do { r = waitpid(*slot, &status, WNOHANG); } while (r < 0 && errno == EINTR);
    if (r == *slot) {
        *slot = -1; *done = 1;
        *ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        return 1;
    }
    if (r < 0 && errno == ECHILD) { *slot = -1; *done = 1; *ok = 0; return 1; }
    return 0;
}

static int media_pump_audio(MediaBackend *b) {
    if (!b || !b->has_audio) return 0;
    if (b->audio_external) {
        (void)media_reap_child(&b->audio_pid, &b->audio_process_done, &b->audio_process_ok);
        if (b->audio_process_done && !b->audio_process_ok) { b->audio_failed = 1; return -1; }
        return 0;
    }
    if (b->audio_pcm_fd >= 0 && b->audio_dev) {
        for (int turn = 0; turn < 4; turn++) {
            uint32_t queued = media_audio_queued_bytes(b);
            if (queued >= MEDIA_AUDIO_QUEUE_LIMIT) break;
            size_t ask = MEDIA_AUDIO_QUEUE_LIMIT - queued;
            if (ask > 16384u) ask = 16384u;
            unsigned char raw[16384];
            ssize_t n = read(b->audio_pcm_fd, raw, ask);
            if (n > 0) {
                unsigned char aligned[16388];
                size_t total = 0;
                if (b->audio_tail_len) { memcpy(aligned, b->audio_tail, b->audio_tail_len); total = b->audio_tail_len; }
                memcpy(aligned + total, raw, (size_t)n); total += (size_t)n;
                size_t usable = total & ~(size_t)3u;
                size_t tail = total - usable;
                if (usable) {
                    if (!media_audio_ring_write(b, aligned, usable)) {
                        b->audio_failed = 1; return -1;
                    }
                }
                if (tail) memcpy(b->audio_tail, aligned + usable, tail);
                b->audio_tail_len = tail;
                continue;
            }
            if (n == 0) {
                close(b->audio_pcm_fd); b->audio_pcm_fd = -1; b->audio_pcm_eof = 1;
                if (b->audio_tail_len) { b->audio_failed = 1; return -1; }
                break;
            }
            if (errno == EINTR) { turn--; continue; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close(b->audio_pcm_fd); b->audio_pcm_fd = -1; b->audio_pcm_eof = 1;
            b->audio_failed = 1; return -1;
        }
    }
    (void)media_reap_child(&b->audio_pid, &b->audio_process_done, &b->audio_process_ok);
    if (b->audio_process_done && !b->audio_process_ok) { b->audio_failed = 1; return -1; }
    if (b->audio_pcm_eof && b->audio_process_done) atomic_store_explicit(&b->audio_input_complete, 1, memory_order_release);
    if (b->audio_pcm_eof && b->audio_process_done && media_audio_queued_bytes(b) == 0 &&
        atomic_load_explicit(&b->audio_pending_bytes, memory_order_acquire) == 0) {
        if (b->frame_bytes && !b->video_process_done && !b->audio_clock_fallback && atomic_load_explicit(&b->audio_clock_started, memory_order_acquire)) {
            b->audio_fallback_elapsed = media_audio_sync_elapsed(b);
            if (!isfinite(b->audio_fallback_elapsed) || b->audio_fallback_elapsed < 0.0) b->audio_fallback_elapsed = 0.0;
            b->audio_fallback_wall = media_monotonic_seconds();
            b->audio_clock_fallback = 1;
        }
    }
    return 0;
}

static int media_audio_finished(const MediaBackend *b) {
    if (!b || !b->has_audio) return 1;
    if (b->audio_external) return b->audio_process_done;
    return b->audio_pcm_eof && b->audio_process_done && media_audio_queued_bytes(b) == 0 && atomic_load_explicit(&b->audio_pending_bytes, memory_order_acquire) == 0;
}

static int run_capture(char *const argv[], char *out, size_t cap) {
    int pp[2]; if (!argv || !argv[0] || !out || cap < 2 || pipe(pp) != 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(pp[0]); dup2(pp[1], STDOUT_FILENO); close(pp[1]);
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        execvp(argv[0], argv); _exit(127);
    }
    close(pp[1]); if (pid < 0) { close(pp[0]); return 0; }
    size_t used = 0; int over = 0;
    for (;;) {
        char buf[1024]; ssize_t n = read(pp[0], buf, sizeof buf);
        if (n > 0) { size_t keep = (size_t)n; if (keep > cap - 1 - used) { keep = cap - 1 - used; over = 1; } if (keep) { memcpy(out + used, buf, keep); used += keep; } }
        else if (n == 0) break; else if (errno == EINTR) continue; else break;
    }
    close(pp[0]); int st = 0; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    out[used] = 0; return !over && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
static char *ff_source(const char *source) {
    if (!source || !*source) return NULL;
    if (!strncmp(source, "file://", 7)) {
        const char *p = source + 7;
        if (!strncmp(p, "localhost/", 10)) p += 9;
        /* ffmpeg accepts ordinary paths more reliably than file:// URIs. */
        char *s = strdup(*p ? p : "/"); if (!s) return NULL;
        /* Minimal percent-decoding for local file URLs. */
        char *r = s, *w = s;
        while (*r) { if (r[0] == '%' && r[1] && r[2]) { char hex[3] = {r[1],r[2],0}; char *e = NULL; long v = strtol(hex,&e,16); if (e && !*e) { *w++ = (char)v; r += 3; continue; } } *w++ = *r++; }
        *w = 0; return s;
    }
    if (!strncmp(source, "http://", 7) || !strncmp(source, "https://", 8) || !strncmp(source, "rtsp://", 7) || !strncmp(source, "rtmp://", 7)) return strdup(source);
    /* Reject non-file custom protocol URLs: pages must not use arbitrary FFmpeg protocols. */
    if (strstr(source, "://")) return NULL;
    return strdup(source);
}
static int media_source_is_seekable(const char *source) {
    /* Report only seekability we can prove. HTTP servers may support Range, but this
       backend does not yet inspect/validate Accept-Ranges and therefore reports no
       seekable range for remote URLs instead of guessing from duration. */
    if (!source || !*source || !strncmp(source, "http://", 7) ||
        !strncmp(source, "https://", 8) || !strncmp(source, "rtsp://", 7) ||
        !strncmp(source, "rtmp://", 7)) return 0;
    char *path = ff_source(source);
    if (!path) return 0;
    struct stat st;
    int ok = stat(path, &st) == 0 && S_ISREG(st.st_mode);
    free(path);
    return ok;
}
static int parse_json_int(const char *start, const char *end, const char *key, int *out) {
    char pat[80]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = start;
    while (p && p < end && (p = strstr(p, pat)) != NULL && p < end) {
        p += strlen(pat); while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p++ != ':') continue;
        while (*p == ' ' || *p == '\t') p++;
        char *ep = NULL; long v = strtol(p, &ep, 10); if (ep != p && ep <= end) { *out = (int)v; return 1; }
    }
    return 0;
}
static double parse_json_double(const char *start, const char *end, const char *key) {
    char pat[80]; snprintf(pat, sizeof pat, "\"%s\"", key); const char *p = start, *found = NULL;
    while (p && p < end && (p = strstr(p, pat)) != NULL && p < end) { found = p; p += strlen(pat); }
    if (!found) return 0;
    p = found + strlen(pat); while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    char *ep = NULL; double v = strtod(p, &ep); return ep != p && isfinite(v) && v >= 0 ? v : 0;
}
int media_available(void) {
    /* ffmpeg decodes video and PCM; SDL2 queues the PCM when available.
       ffplay is only a compatibility fallback, not a required decoder. */
    char buf[8192];
    char *a[] = {"ffprobe", "-version", NULL}; if (!run_capture(a, buf, sizeof buf)) return 0;
    char *b[] = {"ffmpeg", "-version", NULL}; return run_capture(b, buf, sizeof buf);
}
int media_probe(const char *source, MediaInfo *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    char *src = ff_source(source); if (!src) return 0;
    char *argv[] = {"ffprobe", "-v", "error", "-show_entries", "stream=codec_type,width,height:format=duration", "-of", "json", src, NULL};
    char output[8192]; int ok = run_capture(argv, output, sizeof output); free(src);
    if (!ok) return 0;
    const char *v = strstr(output, "\"codec_type\"");
    while (v) {
        const char *line_end = strchr(v, '}'); if (!line_end) line_end = output + strlen(output);
        const char *type = strchr(v, ':');
        if (type && type < line_end && strstr(type, "\"video\"") && strstr(type, "\"video\"") < line_end) {
            out->has_video = 1;
            if (!parse_json_int(type, line_end, "width", &out->width)) out->width = 0;
            if (!parse_json_int(type, line_end, "height", &out->height)) out->height = 0;
            break;
        }
        v = strstr(line_end, "\"codec_type\"");
    }
    out->duration = parse_json_double(output, output + strlen(output), "duration");
    /* This query only emits codec_type values for streams, so audio presence is
       based on ffprobe stream metadata, not filename extension or MIME guesswork. */
    out->has_audio = strstr(output, "\"audio\"") != NULL;
    out->can_seek = media_source_is_seekable(source) && isfinite(out->duration) && out->duration > 0.0;
    if (out->width < 0 || out->width > 16384) out->width = 0;
    if (out->height < 0 || out->height > 16384) out->height = 0;
    return 1;
}
/* Cache bounded windows of demuxer-indexed keyframes for local files. The query
   covers a bounded two-second interval; only windows with a proven next GOP
   boundary are admitted. Entries live in memory and are persisted under the
   per-user cache directory so later browser sessions can reuse prior lookups. */
#define MEDIA_FAST_SEEK_CACHE_ENTRIES 8
#define MEDIA_FAST_SEEK_CACHE_POINTS 1024
typedef struct {
    int valid;
    char source[PATH_MAX];
    dev_t dev;
    ino_t ino;
    off_t size;
    time_t mtime_sec, ctime_sec;
    long mtime_nsec, ctime_nsec;
    double query_start, query_end;
    size_t count;
    double points[MEDIA_FAST_SEEK_CACHE_POINTS];
} MediaFastSeekCache;
static MediaFastSeekCache g_media_fast_seek_cache[MEDIA_FAST_SEEK_CACHE_ENTRIES];
static unsigned g_media_fast_seek_cache_next;
#define MEDIA_FAST_SEEK_INDEX_VERSION "MLFKI1"
#define MEDIA_FAST_SEEK_INDEX_MAX_BYTES (512u * 1024u)

/* Persistent index location is independent of the browser's working directory
   when XDG_CACHE_HOME or HOME is available. A relative local cache is retained
   as a last-resort fallback for restricted/embedded launch environments. */
static int media_fast_seek_mkdirs(const char *path) {
    if (!path || !*path) return 0;
    char copy[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof copy) return 0;
    memcpy(copy, path, n + 1);
    for (char *p = copy + (copy[0] == '/' ? 1 : 0); ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p;
        *p = '\0';
        if (*copy) {
            if (mkdir(copy, 0700) != 0 && errno != EEXIST) { *p = saved; return 0; }
            struct stat st;
            if (lstat(copy, &st) != 0 || !S_ISDIR(st.st_mode)) { *p = saved; return 0; }
        }
        *p = saved;
        if (!saved) break;
    }
    return 1;
}

static int media_fast_seek_index_path(const char *source, char *out, size_t cap, int create_dir) {
    if (!source || !out || cap == 0) return 0;
    char dir[PATH_MAX];
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && xdg[0] == '/') {
        n = snprintf(dir, sizeof dir, "%s/mlbrowser/media-keyframes", xdg);
    } else if (home && home[0] == '/') {
        n = snprintf(dir, sizeof dir, "%s/.cache/mlbrowser/media-keyframes", home);
    } else {
        n = snprintf(dir, sizeof dir, "cache/media-keyframes");
    }
    if (n < 0 || (size_t)n >= sizeof dir) return 0;
    if (create_dir && !media_fast_seek_mkdirs(dir)) return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)source; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    n = snprintf(out, cap, "%s/%016llx.idx", dir, (unsigned long long)hash);
    return n >= 0 && (size_t)n < cap;
}

static int media_fast_seek_cache_matches(const MediaFastSeekCache *c, const char *source,
                                         const struct stat *st, double target);

/* On-disk entries contain only bounded numeric frame timestamps, never commands
   or paths supplied by the index file. The file is versioned and keyed by the
   canonical source path; stat identity and nanosecond mtime invalidate stale data. */
static void media_fast_seek_index_load(const char *source, const struct stat *st) {
    char path[PATH_MAX];
    if (!media_fast_seek_index_path(source, path, sizeof path, 0)) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat fs;
    if (fstat(fd, &fs) != 0 || !S_ISREG(fs.st_mode) || fs.st_size <= 0 ||
        (uint64_t)fs.st_size > MEDIA_FAST_SEEK_INDEX_MAX_BYTES) { close(fd); return; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return; }
    char version[16];
    unsigned long long dev = 0, ino = 0;
    long long size = 0, mtime_sec = 0;
    long mtime_nsec = 0, ctime_nsec = 0;
    long long ctime_sec = 0;
    unsigned windows = 0;
    int header_ok = fscanf(f, "%15s %llu %llu %lld %lld %ld %lld %ld %u", version, &dev, &ino, &size,
                           &mtime_sec, &mtime_nsec, &ctime_sec, &ctime_nsec, &windows) == 9 &&
        strcmp(version, MEDIA_FAST_SEEK_INDEX_VERSION) == 0 &&
        dev == (unsigned long long)st->st_dev && ino == (unsigned long long)st->st_ino &&
        size == (long long)st->st_size && mtime_sec == (long long)st->st_mtime &&
        mtime_nsec == st->st_mtim.tv_nsec && ctime_sec == (long long)st->st_ctime &&
        ctime_nsec == st->st_ctim.tv_nsec && windows > 0 && windows <= MEDIA_FAST_SEEK_CACHE_ENTRIES;
    if (!header_ok) { fclose(f); return; }

    MediaFastSeekCache loaded[MEDIA_FAST_SEEK_CACHE_ENTRIES];
    memset(loaded, 0, sizeof loaded);
    unsigned good = 0;
    for (unsigned wi = 0; wi < windows; wi++) {
        char tag = 0;
        double start = -1.0, end = -1.0;
        size_t points = 0;
        if (fscanf(f, " %c %lf %lf %zu", &tag, &start, &end, &points) != 4 || tag != 'W' ||
            !isfinite(start) || !isfinite(end) || start < 0.0 || end <= start ||
            points < 2 || points > MEDIA_FAST_SEEK_CACHE_POINTS) break;
        MediaFastSeekCache *c = &loaded[good];
        snprintf(c->source, sizeof c->source, "%s", source);
        c->dev = st->st_dev; c->ino = st->st_ino; c->size = st->st_size;
        c->mtime_sec = st->st_mtime; c->mtime_nsec = st->st_mtim.tv_nsec;
        c->ctime_sec = st->st_ctime; c->ctime_nsec = st->st_ctim.tv_nsec;
        c->query_start = start; c->query_end = end; c->count = points;
        int valid = 1;
        double previous = -1.0;
        for (size_t pi = 0; pi < points; pi++) {
            double value = -1.0;
            if (fscanf(f, " %lf", &value) != 1 || !isfinite(value) || value < 0.0 ||
                (pi && value + 0.000001 < previous)) { valid = 0; break; }
            c->points[pi] = value;
            previous = value;
        }
        if (!valid) break;
        if (c->points[0] > start + 0.025 || c->points[points - 1] < end - 0.025) break;
        c->valid = 1;
        good++;
    }
    fclose(f);
    /* Reject a truncated or malformed index as a whole; partial cache restores
       make it harder to reason about which windows were durably recorded. */
    if (good != windows) return;
    for (unsigned i = 0; i < good; i++) {
        g_media_fast_seek_cache[g_media_fast_seek_cache_next++ % MEDIA_FAST_SEEK_CACHE_ENTRIES] = loaded[i];
    }
}

static void media_fast_seek_index_save(const char *source, const struct stat *st) {
    MediaFastSeekCache *entries[MEDIA_FAST_SEEK_CACHE_ENTRIES];
    unsigned count = 0;
    for (size_t i = 0; i < MEDIA_FAST_SEEK_CACHE_ENTRIES; i++) {
        MediaFastSeekCache *c = &g_media_fast_seek_cache[i];
        if (c->valid && strcmp(c->source, source) == 0 && c->dev == st->st_dev &&
            c->ino == st->st_ino && c->size == st->st_size && c->mtime_sec == st->st_mtime &&
            c->mtime_nsec == st->st_mtim.tv_nsec && c->ctime_sec == st->st_ctime &&
            c->ctime_nsec == st->st_ctim.tv_nsec && c->count >= 2 &&
            c->count <= MEDIA_FAST_SEEK_CACHE_POINTS && c->query_start >= 0.0 &&
            c->query_end > c->query_start) entries[count++] = c;
    }
    if (!count) return;
    char path[PATH_MAX], temp[PATH_MAX];
    if (!media_fast_seek_index_path(source, path, sizeof path, 1)) return;
    static unsigned temp_serial;
    int n = snprintf(temp, sizeof temp, "%s.tmp.%ld.%u", path, (long)getpid(), ++temp_serial);
    if (n < 0 || (size_t)n >= sizeof temp) return;
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temp); return; }
    int ok = fprintf(f, "%s %llu %llu %lld %lld %ld %lld %ld %u\n", MEDIA_FAST_SEEK_INDEX_VERSION,
                     (unsigned long long)st->st_dev, (unsigned long long)st->st_ino,
                     (long long)st->st_size, (long long)st->st_mtime, st->st_mtim.tv_nsec,
                     (long long)st->st_ctime, st->st_ctim.tv_nsec, count) > 0;
    for (unsigned i = 0; i < count && ok; i++) {
        MediaFastSeekCache *c = entries[i];
        if (fprintf(f, "W %.9f %.9f %zu", c->query_start, c->query_end, c->count) < 0) { ok = 0; break; }
        for (size_t j = 0; j < c->count; j++) {
            if (fprintf(f, " %.9f", c->points[j]) < 0) { ok = 0; break; }
        }
        if (ok && fputc('\n', f) == EOF) ok = 0;
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) {
        if (rename(temp, path) != 0) unlink(temp);
    } else unlink(temp);
}
#ifdef MEDIA_FAST_SEEK_TESTING
static unsigned g_media_fast_seek_probe_count;
unsigned media_fast_seek_test_probe_count(void) { return g_media_fast_seek_probe_count; }
#endif

static int media_fast_seek_cache_matches(const MediaFastSeekCache *c, const char *source,
                                         const struct stat *st, double target) {
    return c && c->valid && strcmp(c->source, source) == 0 &&
           c->dev == st->st_dev && c->ino == st->st_ino && c->size == st->st_size &&
           c->mtime_sec == st->st_mtime && c->mtime_nsec == st->st_mtim.tv_nsec &&
           c->ctime_sec == st->st_ctime && c->ctime_nsec == st->st_ctim.tv_nsec &&
           target >= c->query_start && target <= c->query_end;
}
static double media_fast_seek_cache_pick(const MediaFastSeekCache *c, double target) {
    double best = -1.0;
    for (size_t i = 0; i < c->count; i++) {
        double t = c->points[i];
        if (t > target + 0.000001) break;
        if (t >= 0.0 && t > best) best = t;
    }
    return best;
}

/* Ask the container demuxer's own random-access index for the keyframe before a
   fastSeek target. On unsupported inputs, overlarge responses, or missing
   timestamps, preserve the caller's requested target instead of failing seek. */
double media_fast_seek_target(const char *source, double target) {
    if (!isfinite(target) || target < 0.0 || target > 1.0e9 || !media_source_is_seekable(source)) return target;
    char *src = ff_source(source);
    if (!src) return target;
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) { free(src); return target; }
    char canonical[PATH_MAX];
    const char *cache_source = realpath(src, canonical) ? canonical : src;

    for (size_t i = 0; i < MEDIA_FAST_SEEK_CACHE_ENTRIES; i++) {
        if (media_fast_seek_cache_matches(&g_media_fast_seek_cache[i], cache_source, &st, target)) {
            double cached = media_fast_seek_cache_pick(&g_media_fast_seek_cache[i], target);
            if (cached >= 0.0) { free(src); return cached; }
        }
    }
    /* Restore proven GOP windows from the per-user disk index after browser
       restart, then retry the same bounded lookup before spawning ffprobe. */
    media_fast_seek_index_load(cache_source, &st);
    for (size_t i = 0; i < MEDIA_FAST_SEEK_CACHE_ENTRIES; i++) {
        if (media_fast_seek_cache_matches(&g_media_fast_seek_cache[i], cache_source, &st, target)) {
            double cached = media_fast_seek_cache_pick(&g_media_fast_seek_cache[i], target);
            if (cached >= 0.0) { free(src); return cached; }
        }
    }

    char interval[80];
    snprintf(interval, sizeof interval, "%.6f%%+2.000", target);
    char *args[] = {
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-skip_frame", "nokey", "-read_intervals", interval,
        "-show_frames", "-show_entries", "frame=best_effort_timestamp_time",
        "-of", "csv=p=0", src, NULL
    };
    char output[8192];
#ifdef MEDIA_FAST_SEEK_TESTING
    g_media_fast_seek_probe_count++;
#endif
    int ok = run_capture(args, output, sizeof output);
    if (!ok) { free(src); return target; }

    double points[MEDIA_FAST_SEEK_CACHE_POINTS];
    size_t count = 0;
    double best = -1.0, next = -1.0;
    const char *p = output;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *end = NULL;
        double value = strtod(p, &end);
        if (end == p) {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (isfinite(value) && value >= 0.0) {
            if (count < MEDIA_FAST_SEEK_CACHE_POINTS) points[count++] = value;
            if (value <= target + 0.000001 && value > best) best = value;
            if (value > target + 0.000001 && (next < 0.0 || value < next)) next = value;
        }
        p = end;
        while (*p && *p != '\n') p++;
    }
    /* The cache window is only provable when this query also returned the next
       keyframe: without that boundary a longer GOP might be hiding beyond the
       queried interval, so do not reuse the result for later targets. */
    if (count && best >= 0.0 && next >= 0.0) {
        MediaFastSeekCache *c = &g_media_fast_seek_cache[g_media_fast_seek_cache_next++ % MEDIA_FAST_SEEK_CACHE_ENTRIES];
        c->valid = 0;
        snprintf(c->source, sizeof c->source, "%s", cache_source);
        c->dev = st.st_dev; c->ino = st.st_ino; c->size = st.st_size;
        c->mtime_sec = st.st_mtime; c->mtime_nsec = st.st_mtim.tv_nsec;
        c->ctime_sec = st.st_ctime; c->ctime_nsec = st.st_ctim.tv_nsec;
        /* The returned previous keyframe is also a safe lower boundary. Keeping
           the whole [best,next] GOP interval lets bidirectional scrubbers reuse
           the entry; limiting the window to the original query target forced a
           new ffprobe process for every backward movement inside the same GOP. */
        c->query_start = best; c->query_end = next;
        c->count = count;
        memcpy(c->points, points, count * sizeof points[0]);
        c->valid = 1;
        media_fast_seek_index_save(cache_source, &st);
    }
    free(src);
    return best >= 0.0 ? best : target;
}

/* ffplay's atempo filter is part of the audio path, so its media-time rate matches
   the video PTS scheduler. Chain filters because a single atempo stage is limited
   to [0.5, 2.0]. The public browser surface clamps to [1/16, 4]. */
static const char *media_make_atempo(double rate, char out[128]) {
    out[0] = 0;
    if (!isfinite(rate) || rate < 0.0625 || rate > 4.0 || fabs(rate - 1.0) < 0.000001) return NULL;
    double r = rate; size_t used = 0;
    while (r > 2.0 + 1e-9) { int n = snprintf(out + used, 128 - used, "%satempo=2.0", used ? "," : ""); if (n < 0 || (size_t)n >= 128 - used) return NULL; used += (size_t)n; r /= 2.0; }
    while (r < 0.5 - 1e-9) { int n = snprintf(out + used, 128 - used, "%satempo=0.5", used ? "," : ""); if (n < 0 || (size_t)n >= 128 - used) return NULL; used += (size_t)n; r /= 0.5; }
    int n = snprintf(out + used, 128 - used, "%satempo=%.8f", used ? "," : "", r);
    if (n < 0 || (size_t)n >= 128 - used) return NULL;
    return out;
}
static pid_t spawn_player(const char *source, double start, int volume, double rate) {
    int devnull = open("/dev/null", O_WRONLY);
    pid_t pid = fork();
    if (pid == 0) {
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        int dn = open("/dev/null", O_RDONLY); if (dn >= 0) { dup2(dn, STDIN_FILENO); close(dn); }
        char ss[40], vol[16], af[128];
        snprintf(ss, sizeof ss, "%.3f", start > 0 ? start : 0);
        snprintf(vol, sizeof vol, "%d", volume < 0 ? 0 : volume > 100 ? 100 : volume);
        const char *filter = media_make_atempo(rate, af);
        char *args[24]; int n = 0;
        args[n++]="ffplay"; args[n++]="-nodisp"; args[n++]="-autoexit"; args[n++]="-loglevel"; args[n++]="error"; args[n++]="-nostats"; args[n++]="-vn"; args[n++]="-volume"; args[n++]=vol;
        if (filter) { args[n++]="-af"; args[n++]=af; }
        if (start > 0.001) { args[n++]="-ss"; args[n++]=ss; }
        args[n++]=(char*)source; args[n]=NULL; execvp(args[0], args);
        _exit(127);
    }
    if (devnull >= 0) close(devnull);
    return pid;
}
static pid_t spawn_video(const char *source, double start, int w, int h, int *read_fd, int *meta_fd) {
    int outp[2] = {-1, -1}, metap[2] = {-1, -1};
    if (pipe(outp) != 0 || pipe(metap) != 0) {
        if (outp[0] >= 0) { close(outp[0]); close(outp[1]); }
        if (metap[0] >= 0) { close(metap[0]); close(metap[1]); }
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(outp[0]); close(metap[0]);
        if (dup2(outp[1], STDOUT_FILENO) < 0 || dup2(metap[1], STDERR_FILENO) < 0) _exit(126);
        if (outp[1] != STDOUT_FILENO) close(outp[1]);
        if (metap[1] != STDERR_FILENO) close(metap[1]);
        int dn_in = open("/dev/null", O_RDONLY); if (dn_in >= 0) { dup2(dn_in, STDIN_FILENO); if (dn_in != STDIN_FILENO) close(dn_in); }
        char ss[40], dims[128]; snprintf(ss,sizeof ss,"%.6f", start > 0 ? start : 0); snprintf(dims,sizeof dims,"showinfo,scale=%d:%d:flags=fast_bilinear",w,h);
        if (start > 0.001) { char *args[] = {"ffmpeg","-hide_banner","-loglevel","info","-nostdin","-ss",ss,"-i",(char*)source,"-an","-sn","-dn","-vf",dims,"-fps_mode","passthrough","-f","rawvideo","-pix_fmt","rgba","pipe:1",NULL}; execvp(args[0],args); }
        else { char *args[] = {"ffmpeg","-hide_banner","-loglevel","info","-nostdin","-i",(char*)source,"-an","-sn","-dn","-vf",dims,"-fps_mode","passthrough","-f","rawvideo","-pix_fmt","rgba","pipe:1",NULL}; execvp(args[0],args); }
        _exit(127);
    }
    close(outp[1]); close(metap[1]);
    if (pid < 0) { close(outp[0]); close(metap[0]); return -1; }
    int flags = fcntl(outp[0], F_GETFL, 0); if (flags >= 0) fcntl(outp[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(metap[0], F_GETFL, 0); if (flags >= 0) fcntl(metap[0], F_SETFL, flags | O_NONBLOCK);
    *read_fd = outp[0]; *meta_fd = metap[0]; return pid;
}
/* Stream input uses socketpair rather than pipe so send(MSG_NOSIGNAL) can
   report a dead decoder without delivering SIGPIPE to the whole browser. */
static int stream_socketpair(int *child_fd, int *parent_fd) {
    int sv[2];
    if (!child_fd || !parent_fd || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    *child_fd = sv[1]; *parent_fd = sv[0];
    return 1;
}
static void make_parent_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    /* Prevent later decoder children from accidentally keeping another
       decoder's input socket alive across exec (which would hide EOF). */
    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags >= 0) (void)fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
static const char *media_make_audio_filter(double rate, double volume, char out[192]) {
    /* HTML-media PCM is gain-controlled in media_sdl_audio_callback so live
       volume/mute updates do not restart the decoder. Keep the volume argument
       to avoid changing the internal call contract; ffplay fallback has its
       own native volume option and doesn't use this filter. */
    (void)volume;
    char tempo[128]; const char *t = media_make_atempo(rate, tempo);
    if (!t) return NULL;
    int n = snprintf(out, 192, "%s", t);
    return n > 0 && n < 192 ? out : NULL;
}

static pid_t spawn_file_audio_pcm(const char *source, double start_seconds, double volume, double rate, int *read_fd) {
    int outp[2] = {-1,-1};
    if (!source || !read_fd || pipe(outp) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(outp[0]);
        if (dup2(outp[1], STDOUT_FILENO) < 0) _exit(126);
        if (outp[1] != STDOUT_FILENO) close(outp[1]);
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) { dup2(dn, STDERR_FILENO); if (dn != STDERR_FILENO) close(dn); }
        int di = open("/dev/null", O_RDONLY); if (di >= 0) { dup2(di, STDIN_FILENO); if (di != STDIN_FILENO) close(di); }
        char ss[48], af[192]; const char *filter = media_make_audio_filter(rate, volume, af);
        snprintf(ss, sizeof ss, "%.6f", start_seconds > 0.0 ? start_seconds : 0.0);
        char *args[32]; int n = 0;
        args[n++] = "ffmpeg"; args[n++] = "-hide_banner"; args[n++] = "-loglevel"; args[n++] = "error"; args[n++] = "-nostdin";
        if (start_seconds > 0.001) { args[n++] = "-ss"; args[n++] = ss; }
        args[n++] = "-i"; args[n++] = (char *)source; args[n++] = "-vn"; args[n++] = "-sn"; args[n++] = "-dn";
        if (filter) { args[n++] = "-af"; args[n++] = (char *)filter; }
        args[n++] = "-ac"; args[n++] = "2"; args[n++] = "-ar"; args[n++] = "44100"; args[n++] = "-acodec"; args[n++] = "pcm_s16le"; args[n++] = "-f"; args[n++] = "s16le"; args[n++] = "pipe:1"; args[n] = NULL;
        execvp(args[0], args); _exit(127);
    }
    close(outp[1]);
    if (pid < 0) { close(outp[0]); return -1; }
    int flags = fcntl(outp[0], F_GETFL, 0); if (flags >= 0) (void)fcntl(outp[0], F_SETFL, flags | O_NONBLOCK);
    *read_fd = outp[0]; return pid;
}

static pid_t spawn_stream_audio_pcm(int *write_fd, int *read_fd, double volume, double start_seconds, double rate) {
    int child_fd = -1, parent_fd = -1, outp[2] = {-1,-1};
    if (!write_fd || !read_fd || !stream_socketpair(&child_fd, &parent_fd)) return -1;
    if (pipe(outp) != 0) { close(child_fd); close(parent_fd); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(parent_fd); close(outp[0]);
        if (dup2(child_fd, STDIN_FILENO) < 0 || dup2(outp[1], STDOUT_FILENO) < 0) _exit(126);
        if (child_fd != STDIN_FILENO) close(child_fd);
        if (outp[1] != STDOUT_FILENO) close(outp[1]);
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) { dup2(dn, STDERR_FILENO); if (dn != STDERR_FILENO) close(dn); }
        char ss[48], af[192]; const char *filter = media_make_audio_filter(rate, volume, af);
        snprintf(ss, sizeof ss, "%.6f", start_seconds > 0.0 ? start_seconds : 0.0);
        char *args[32]; int n = 0;
        args[n++] = "ffmpeg"; args[n++] = "-hide_banner"; args[n++] = "-loglevel"; args[n++] = "error"; args[n++] = "-nostdin";
        if (start_seconds > 0.001) { args[n++] = "-ss"; args[n++] = ss; }
        args[n++] = "-i"; args[n++] = "pipe:0"; args[n++] = "-vn"; args[n++] = "-sn"; args[n++] = "-dn";
        if (filter) { args[n++] = "-af"; args[n++] = (char *)filter; }
        args[n++] = "-ac"; args[n++] = "2"; args[n++] = "-ar"; args[n++] = "44100"; args[n++] = "-acodec"; args[n++] = "pcm_s16le"; args[n++] = "-f"; args[n++] = "s16le"; args[n++] = "pipe:1"; args[n] = NULL;
        execvp(args[0], args); _exit(127);
    }
    close(child_fd); close(outp[1]);
    if (pid < 0) { close(parent_fd); close(outp[0]); return -1; }
    make_parent_nonblocking(parent_fd);
    int flags = fcntl(outp[0], F_GETFL, 0); if (flags >= 0) (void)fcntl(outp[0], F_SETFL, flags | O_NONBLOCK);
    *write_fd = parent_fd; *read_fd = outp[0]; return pid;
}

static pid_t spawn_stream_player(int *write_fd, int volume, double start_seconds, double rate) {
    int child_fd = -1, parent_fd = -1;
    if (!stream_socketpair(&child_fd, &parent_fd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(parent_fd);
        if (dup2(child_fd, STDIN_FILENO) < 0) _exit(126);
        if (child_fd != STDIN_FILENO) close(child_fd);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); if (dn != STDERR_FILENO) close(dn); }
        char vol[16], pos[48], af[128]; snprintf(vol, sizeof vol, "%d", volume < 0 ? 0 : volume > 100 ? 100 : volume);
        const char *filter = media_make_atempo(rate, af);
        char *args[24]; int n = 0;
        args[n++]="ffplay"; args[n++]="-nodisp"; args[n++]="-autoexit"; args[n++]="-loglevel"; args[n++]="error"; args[n++]="-nostats"; args[n++]="-vn"; args[n++]="-volume"; args[n++]=vol;
        if (filter) { args[n++]="-af"; args[n++]=af; }
        if (start_seconds > 0.0) { snprintf(pos, sizeof pos, "%.6f", start_seconds); args[n++]="-ss"; args[n++]=pos; }
        args[n++]="-i"; args[n++]="pipe:0"; args[n]=NULL; execvp(args[0], args);
        _exit(127);
    }
    close(child_fd);
    if (pid < 0) { close(parent_fd); return -1; }
    make_parent_nonblocking(parent_fd);
    *write_fd = parent_fd;
    return pid;
}
/* Stream video from the same byte queue as ffplay audio. showinfo emits each
   decoded frame's PTS on a separate stderr pipe; media_poll() schedules RGBA
   delivery against CLOCK_MONOTONIC using that PTS. This paces presentation by
   the media timeline instead of relying on input-rate limiting or poll cadence.
   It is not sample-accurate A/V sync: both external processes still own clocks. */
static pid_t spawn_stream_video(int *write_fd, int *read_fd, int *meta_fd, int w, int h, double start_seconds) {
    int child_fd = -1, parent_fd = -1, outp[2] = {-1,-1}, metap[2] = {-1,-1};
    if (!stream_socketpair(&child_fd, &parent_fd)) return -1;
    if (pipe(outp) != 0 || pipe(metap) != 0) {
        close(child_fd); close(parent_fd);
        if (outp[0] >= 0) { close(outp[0]); close(outp[1]); }
        if (metap[0] >= 0) { close(metap[0]); close(metap[1]); }
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(parent_fd); close(outp[0]); close(metap[0]);
        if (dup2(child_fd, STDIN_FILENO) < 0 || dup2(outp[1], STDOUT_FILENO) < 0 || dup2(metap[1], STDERR_FILENO) < 0) _exit(126);
        if (child_fd != STDIN_FILENO) close(child_fd);
        if (outp[1] != STDOUT_FILENO) close(outp[1]);
        if (metap[1] != STDERR_FILENO) close(metap[1]);
        char dims[128], pos[48]; snprintf(dims, sizeof dims, "showinfo,scale=%d:%d:flags=fast_bilinear", w, h);
        if (start_seconds > 0.0) {
            snprintf(pos, sizeof pos, "%.6f", start_seconds);
            char *args[] = {"ffmpeg", "-hide_banner", "-loglevel", "info", "-nostdin", "-ss", pos, "-i", "pipe:0", "-an", "-sn", "-dn", "-vf", dims, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1", NULL};
            execvp(args[0], args);
        } else {
            char *args[] = {"ffmpeg", "-hide_banner", "-loglevel", "info", "-nostdin", "-i", "pipe:0", "-an", "-sn", "-dn", "-vf", dims, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1", NULL};
            execvp(args[0], args);
        }
        _exit(127);
    }
    close(child_fd); close(outp[1]); close(metap[1]);
    if (pid < 0) { close(parent_fd); close(outp[0]); close(metap[0]); return -1; }
    make_parent_nonblocking(parent_fd);
    make_parent_nonblocking(outp[0]);
    make_parent_nonblocking(metap[0]);
    *write_fd = parent_fd; *read_fd = outp[0]; *meta_fd = metap[0];
    return pid;
}
/* Non-blocking SPSC queue -> decoder stdin pump. It consumes at most 16 * 16 KiB
   per renderer poll and does not discard a byte until every live decoder accepted it. */
static void media_stream_pump(MediaBackend *b) {
    if (!b || !b->stream_mode || !b->stream_input || b->stream_input_eof || b->stream_input_failed) return;
    for (int block = 0; block < 16; block++) {
        if (b->stream_pending_len == 0) {
            ptrdiff_t n = media_byte_queue_read(b->stream_input, b->stream_pending, sizeof b->stream_pending);
            if (n > 0) {
                b->stream_pending_len = (size_t)n;
                b->stream_pending_off[0] = b->input_fd[0] < 0 ? (size_t)n : 0;
                b->stream_pending_off[1] = b->input_fd[1] < 0 ? (size_t)n : 0;
            } else if (n == 0) {
                b->stream_input_eof = 1;
                for (int i = 0; i < 2; i++) if (b->input_fd[i] >= 0) { close(b->input_fd[i]); b->input_fd[i] = -1; }
                return;
            } else if (n == -1) {
                return; /* producer has not published more bytes yet */
            } else {
                b->stream_input_failed = 1;
                for (int i = 0; i < 2; i++) if (b->input_fd[i] >= 0) { close(b->input_fd[i]); b->input_fd[i] = -1; }
                return;
            }
        }
        int progressed = 0;
        for (int i = 0; i < 2; i++) {
            if (b->input_fd[i] < 0 || b->stream_pending_off[i] >= b->stream_pending_len) continue;
            size_t remain = b->stream_pending_len - b->stream_pending_off[i];
#ifdef MSG_NOSIGNAL
            ssize_t n = send(b->input_fd[i], b->stream_pending + b->stream_pending_off[i], remain, MSG_DONTWAIT | MSG_NOSIGNAL);
#else
            ssize_t n = send(b->input_fd[i], b->stream_pending + b->stream_pending_off[i], remain, MSG_DONTWAIT);
#endif
            if (n > 0) { b->stream_pending_off[i] += (size_t)n; b->stream_input_started = 1; progressed = 1; }
            else if (n < 0 && errno == EINTR) { progressed = 1; }
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { /* leave suffix queued */ }
            else {
                close(b->input_fd[i]); b->input_fd[i] = -1;
                b->stream_pending_off[i] = b->stream_pending_len;
                b->stream_input_failed = 1;
                progressed = 1;
            }
        }
        if (b->stream_pending_off[0] >= b->stream_pending_len && b->stream_pending_off[1] >= b->stream_pending_len) {
            b->stream_pending_len = 0;
            continue;
        }
        if (!progressed) return; /* pipe/socket backpressure: resume on next poll */
    }
}
#else
int media_available(void) { return 0; }
int media_probe(const char *source, MediaInfo *out) { (void)source; if (out) memset(out, 0, sizeof *out); return 0; }
double media_fast_seek_target(const char *source, double target) { (void)source; return target; }
#endif

MediaBackend *media_start_rate(const char *source, double start_seconds, double volume, double playback_rate, int want_video, int max_width, int max_height, const MediaInfo *known_info) {
#ifdef _WIN32
    (void)source; (void)start_seconds; (void)volume; (void)playback_rate; (void)want_video; (void)max_width; (void)max_height; (void)known_info; return NULL;
#else
    if (!isfinite(playback_rate) || playback_rate < 0.0625 || playback_rate > 4.0 || !isfinite(start_seconds) || start_seconds < 0.0) return NULL;
    if (!isfinite(volume)) volume = 1.0;
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;
    char *src = ff_source(source); if (!src) return NULL;
    MediaInfo info; memset(&info, 0, sizeof info);
    if (known_info) info = *known_info; else (void)media_probe(src, &info);
    MediaBackend *b = calloc(1, sizeof *b); if (!b) { free(src); return NULL; }
    b->audio_pid = b->video_pid = -1; b->audio_pcm_fd = b->frame_fd = b->frame_meta_fd = -1;
    b->input_fd[0] = b->input_fd[1] = -1;
    atomic_init(&b->audio_clock_started, 0);
    atomic_init(&b->audio_ring_read, 0);
    atomic_init(&b->audio_ring_write, 0);
    atomic_init(&b->audio_consumed_bytes, 0);
    atomic_init(&b->audio_underrun_frames, 0);
    atomic_init(&b->audio_pending_bytes, 0);
    atomic_init(&b->audio_volume_q16, (uint32_t)(volume * 65536.0 + 0.5));
    atomic_init(&b->audio_input_complete, 0);
    b->width = info.width; b->height = info.height;
    b->state = MEDIA_BACKEND_OPENING; b->can_seek = info.can_seek; b->duration = info.duration; b->playback_rate = playback_rate;
    b->media_start_time = start_seconds;
    b->has_audio = info.has_audio;
    /* Decode audio to bounded PCM and play it through this backend's own SDL
       device. The device queue is the master A/V clock. Keep ffplay as a fallback
       only for machines with no usable SDL audio device. */
    if (info.has_audio) {
        b->audio_dev = media_audio_open_device(b);
        if (b->audio_dev) b->audio_pid = spawn_file_audio_pcm(src, start_seconds, volume, playback_rate, &b->audio_pcm_fd);
        if (!b->audio_dev || b->audio_pid < 0) {
            if (b->audio_pcm_fd >= 0) { close(b->audio_pcm_fd); b->audio_pcm_fd = -1; }
            if (b->audio_pid > 0) { kill(b->audio_pid, SIGTERM); while (waitpid(b->audio_pid, NULL, 0) < 0 && errno == EINTR) {} }
            media_audio_close_device(b);
            b->audio_pid = spawn_player(src, start_seconds, (int)lrint(volume * 100.0), playback_rate);
            b->audio_external = b->audio_pid > 0;
        }
    }
    if (want_video && info.has_video) {
        int w = max_width > 0 ? max_width : info.width, h = max_height > 0 ? max_height : info.height;
        if (w < 1) w = 320;
        if (h < 1) h = 180;
        if (w > 960) { h = (int)((double)h * 960.0 / w); w = 960; }
        if (h > 540) { w = (int)((double)w * 540.0 / h); h = 540; }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        b->width = w; b->height = h; b->frame_bytes = (size_t)w * (size_t)h * 4u;
        if (b->frame_bytes <= 960u * 540u * 4u) b->frame = malloc(b->frame_bytes);
        if (b->frame) b->video_pid = spawn_video(src, start_seconds, w, h, &b->frame_fd, &b->frame_meta_fd);
    }
    free(src);
    if ((b->has_audio && b->audio_pid < 0) || (want_video && info.has_video && b->video_pid < 0) || (b->video_pid < 0 && !b->has_audio)) {
        if (b->audio_pid > 0) { kill(b->audio_pid, SIGTERM); while (waitpid(b->audio_pid, NULL, 0) < 0 && errno == EINTR) {} }
        if (b->video_pid > 0) { kill(b->video_pid, SIGTERM); while (waitpid(b->video_pid, NULL, 0) < 0 && errno == EINTR) {} }
        if (b->audio_pcm_fd >= 0) close(b->audio_pcm_fd);
        if (b->frame_fd >= 0) close(b->frame_fd);
        if (b->frame_meta_fd >= 0) close(b->frame_meta_fd);
        media_audio_close_device(b); free(b->frame); free(b); return NULL;
    }
    return b;
#endif
}
MediaBackend *media_start(const char *source, double start_seconds, double volume, int want_video, int max_width, int max_height, const MediaInfo *known_info) {
    return media_start_rate(source, start_seconds, volume, 1.0, want_video, max_width, max_height, known_info);
}
MediaBackend *media_start_stream_at_rate(MediaByteQueue *input, double volume, int want_video, int want_audio, int max_width, int max_height, double start_seconds, double playback_rate) {
#ifdef _WIN32
    (void)input; (void)volume; (void)want_video; (void)want_audio; (void)max_width; (void)max_height; (void)start_seconds; (void)playback_rate;
    return NULL;
#else
    if (!input || (!want_video && !want_audio) || !isfinite(start_seconds) || start_seconds < 0.0 || !isfinite(playback_rate) || playback_rate < 0.0625 || playback_rate > 4.0) return NULL;
    if (!isfinite(volume)) volume = 1.0;
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;
    MediaBackend *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->audio_pid = b->video_pid = -1; b->audio_pcm_fd = b->frame_fd = b->frame_meta_fd = -1;
    b->input_fd[0] = b->input_fd[1] = -1;
    atomic_init(&b->audio_clock_started, 0);
    atomic_init(&b->audio_ring_read, 0);
    atomic_init(&b->audio_ring_write, 0);
    atomic_init(&b->audio_consumed_bytes, 0);
    atomic_init(&b->audio_underrun_frames, 0);
    atomic_init(&b->audio_pending_bytes, 0);
    atomic_init(&b->audio_volume_q16, (uint32_t)(volume * 65536.0 + 0.5));
    atomic_init(&b->audio_input_complete, 0);
    b->stream_mode = 1; b->stream_input = input;
    b->state = MEDIA_BACKEND_OPENING; b->can_seek = 0; b->duration = 0.0; b->playback_rate = playback_rate;
    b->media_start_time = start_seconds; b->has_audio = !!want_audio;
    if (want_audio) {
        b->audio_dev = media_audio_open_device(b);
        if (b->audio_dev) b->audio_pid = spawn_stream_audio_pcm(&b->input_fd[0], &b->audio_pcm_fd, volume, start_seconds, playback_rate);
        if (!b->audio_dev || b->audio_pid < 0) {
            if (b->audio_pcm_fd >= 0) { close(b->audio_pcm_fd); b->audio_pcm_fd = -1; }
            if (b->audio_pid > 0) { kill(b->audio_pid, SIGTERM); while (waitpid(b->audio_pid, NULL, 0) < 0 && errno == EINTR) {} }
            if (b->input_fd[0] >= 0) { close(b->input_fd[0]); b->input_fd[0] = -1; }
            media_audio_close_device(b);
            b->audio_pid = spawn_stream_player(&b->input_fd[0], (int)lrint(volume * 100.0), start_seconds, playback_rate);
            b->audio_external = b->audio_pid > 0;
        }
    }
    if (want_video) {
        int w = max_width > 0 ? max_width : 320, h = max_height > 0 ? max_height : 180;
        if (w > 960) { h = (int)((double)h * 960.0 / w); w = 960; }
        if (h > 540) { w = (int)((double)w * 540.0 / h); h = 540; }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        b->width = w; b->height = h; b->frame_bytes = (size_t)w * (size_t)h * 4u;
        if (b->frame_bytes <= 960u * 540u * 4u) b->frame = malloc(b->frame_bytes);
        if (b->frame) b->video_pid = spawn_stream_video(&b->input_fd[1], &b->frame_fd, &b->frame_meta_fd, w, h, start_seconds);
        if (!b->frame || b->video_pid < 0) b->stream_input_failed = 1;
    }
    if ((want_audio && b->audio_pid < 0) || (want_video && b->video_pid < 0) || (!want_audio && !want_video)) {
        for (int i = 0; i < 2; i++) if (b->input_fd[i] >= 0) close(b->input_fd[i]);
        if (b->audio_pcm_fd >= 0) close(b->audio_pcm_fd);
        if (b->frame_fd >= 0) close(b->frame_fd);
        if (b->frame_meta_fd >= 0) close(b->frame_meta_fd);
        if (b->audio_pid > 0) { kill(b->audio_pid, SIGTERM); while (waitpid(b->audio_pid, NULL, 0) < 0 && errno == EINTR) {} }
        if (b->video_pid > 0) { kill(b->video_pid, SIGTERM); while (waitpid(b->video_pid, NULL, 0) < 0 && errno == EINTR) {} }
        media_audio_close_device(b); free(b->frame); free(b); return NULL;
    }
    return b;
#endif
}
MediaBackend *media_start_stream_at(MediaByteQueue *input, double volume, int want_video, int want_audio, int max_width, int max_height, double start_seconds) {
    return media_start_stream_at_rate(input, volume, want_video, want_audio, max_width, max_height, start_seconds, 1.0);
}
MediaBackend *media_start_stream(MediaByteQueue *input, double volume, int want_video, int want_audio, int max_width, int max_height) {
    return media_start_stream_at_rate(input, volume, want_video, want_audio, max_width, max_height, 0.0, 1.0);
}

int media_poll(MediaBackend *b, const unsigned char **rgba, int *width, int *height) {
#ifdef _WIN32
    (void)b; (void)rgba; (void)width; (void)height; return -1;
#else
    if (!b) return -1;
    if (b->state == MEDIA_BACKEND_ENDED || b->state == MEDIA_BACKEND_FAILED) return -1;
    /* Do not drain decoder pipes or pump stream bytes while explicitly paused. */
    if (b->paused) return 0;
    if (b->stream_mode) {
        media_stream_pump(b);
        if (b->stream_input_failed) { b->state = MEDIA_BACKEND_FAILED; return -1; }
    }
    if (media_pump_audio(b) < 0 || b->audio_failed) { b->state = MEDIA_BACKEND_FAILED; return -1; }
    media_drain_frame_metadata(b);
    if (b->timing_failed) { b->state = MEDIA_BACKEND_FAILED; return -1; }
    if (b->frame_bytes == 0) {
        if (!b->has_audio) { b->state = MEDIA_BACKEND_ENDED; return -1; }
        if (b->stream_mode && !b->stream_input_started && !b->audio_process_done) return 0;
        if (media_audio_finished(b)) { b->state = MEDIA_BACKEND_ENDED; return -1; }
        b->state = MEDIA_BACKEND_RUNNING;
        return 0;
    }
    if (b->video_pid <= 0 && !b->video_process_done) {
        if (b->state != MEDIA_BACKEND_DRAINING) b->state = MEDIA_BACKEND_FAILED;
        return -1;
    }
    if (!b->eof && b->frame_fd >= 0) {
        while (b->have < b->frame_bytes) {
            ssize_t n = read(b->frame_fd, b->frame + b->have, b->frame_bytes - b->have);
            if (n > 0) { b->have += (size_t)n; continue; }
            if (n == 0) { b->eof = 1; break; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            b->eof = 1; b->state = MEDIA_BACKEND_FAILED; break;
        }
    }
    if (b->have == b->frame_bytes) {
        int due = media_prepare_frame_deadline(b);
        if (due < 0) return -1;
        if (due == 0) { b->state = MEDIA_BACKEND_RUNNING; return 0; }
        if (media_frame_is_stale(b)) {
            /* Preserve timeline progression for missing-PTS fallback even when
               a frame is dropped. Consume exactly one raw frame per poll so a
               long backlog cannot monopolize the main browser loop. */
            b->last_delivered_pts = b->frame_pts;
            b->have_last_pts = 1;
            b->frame_pts_valid = 0;
            b->have = 0;
            b->video_frames_dropped++;
            b->state = MEDIA_BACKEND_RUNNING;
            return 0;
        }
        b->last_delivered_pts = b->frame_pts;
        b->have_last_pts = 1;
        b->last_video_time = b->prepared_video_time;
        b->video_time_valid = 1;
        b->frame_pts_valid = 0;
        b->have = 0; b->state = MEDIA_BACKEND_RUNNING;
        if (rgba) *rgba = b->frame;
        if (width) *width = b->width;
        if (height) *height = b->height;
        return 1;
    }
    if (b->eof) {
        if (b->have != 0) { b->state = MEDIA_BACKEND_FAILED; (void)media_reap_child(&b->video_pid, &b->video_process_done, &b->video_process_ok); return -1; }
        b->state = MEDIA_BACKEND_DRAINING;
        (void)media_reap_child(&b->video_pid, &b->video_process_done, &b->video_process_ok);
        if (b->video_process_done && !b->video_process_ok) { b->state = MEDIA_BACKEND_FAILED; return -1; }
        if (b->video_process_done && media_audio_finished(b)) { b->state = MEDIA_BACKEND_ENDED; return -1; }
        return 0;
    }
    (void)media_reap_child(&b->video_pid, &b->video_process_done, &b->video_process_ok);
    if (b->video_process_done && !b->video_process_ok) { b->state = MEDIA_BACKEND_FAILED; return -1; }
    return 0;
#endif
}
int media_get_status(const MediaBackend *b, MediaBackendStatus *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!b) { out->state = MEDIA_BACKEND_FAILED; return 0; }
    out->state = b->state;
    out->can_seek = b->can_seek;
    out->duration = b->duration;
    out->buffered_count = 0;
#ifndef _WIN32
    double elapsed = 0.0; int has_clock = 0;
    out->audio_clock = b->audio_dev && atomic_load_explicit(&b->audio_clock_started, memory_order_acquire) && !b->audio_external;
    out->audio_frames_consumed = atomic_load_explicit(&b->audio_consumed_bytes, memory_order_acquire) / MEDIA_AUDIO_BYTES_PER_FRAME;
    out->audio_underrun_frames = atomic_load_explicit(&b->audio_underrun_frames, memory_order_acquire);
    out->video_frames_dropped = b->video_frames_dropped;
    out->has_first_video_frame_time = b->first_video_time_valid;
    out->first_video_frame_time = b->first_video_time_valid ? b->first_video_time : 0.0;
    out->has_video_frame_time = b->video_time_valid;
    out->video_frame_time = b->video_time_valid ? b->last_video_time : 0.0;
    out->audio_output_latency_estimate_seconds = b->audio_dev ? b->audio_output_latency_estimate_seconds : 0.0;
    out->audio_queue_bytes = media_audio_queued_bytes(b);
    out->live_volume_control = b->audio_dev && !b->audio_external;
    out->audio_volume = (double)atomic_load_explicit(&b->audio_volume_q16, memory_order_acquire) / 65536.0;
    if (out->audio_clock) { elapsed = media_audio_media_elapsed(b); has_clock = 1; }
    else if (b->clock_started) {
        double rate = (isfinite(b->playback_rate) && b->playback_rate >= 0.0625 && b->playback_rate <= 4.0) ? b->playback_rate : 1.0;
        double now = b->paused ? b->pause_started : media_monotonic_seconds();
        elapsed = (now - b->clock_start) * rate;
        if (elapsed < 0.0) elapsed = 0.0;
        has_clock = 1;
    }
    if (has_clock) {
        out->has_clock = 1;
        out->current_time = b->media_start_time + elapsed;
        if (out->current_time < 0.0) out->current_time = 0.0;
        if (isfinite(b->duration) && b->duration > 0.0 && out->current_time > b->duration) out->current_time = b->duration;
    }
#endif
    return 1;
}
int media_set_volume(MediaBackend *b, double volume) {
    if (!b || !isfinite(volume) || volume < 0.0 || volume > 1.0) return 0;
#ifdef _WIN32
    return 0;
#else
    if (!b->audio_dev || b->audio_external) return 0;
    uint32_t q16 = (uint32_t)(volume * 65536.0 + 0.5);
    if (q16 > 65536u) q16 = 65536u;
    atomic_store_explicit(&b->audio_volume_q16, q16, memory_order_release);
    return 1;
#endif
}
int media_pause(MediaBackend *b) {
    if (!b) return 0;
#ifdef _WIN32
    return 0;
#else
    if (b->paused) return 1;
    if (b->state == MEDIA_BACKEND_ENDED || b->state == MEDIA_BACKEND_FAILED) return 0;
    pid_t ids[2] = { b->audio_pid, b->video_pid };
    int stopped[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (ids[i] <= 0) continue;
        if (kill(ids[i], SIGSTOP) == 0) stopped[i] = 1;
        else if (errno != ESRCH) {
            for (int j = 0; j < i; j++) if (stopped[j]) (void)kill(ids[j], SIGCONT);
            return 0;
        }
    }
    double now = media_monotonic_seconds();
    /* Stop the SDL callback before freezing its pending sample accounting. */
    if (b->audio_dev && g_media_sdl.pause_device) {
        g_media_sdl.pause_device(b->audio_dev, 1);
        uint32_t pending = atomic_exchange_explicit(&b->audio_pending_bytes, 0, memory_order_acq_rel);
        if (pending) atomic_fetch_add_explicit(&b->audio_consumed_bytes, pending, memory_order_relaxed);
    }
    if (b->audio_clock_fallback) {
        b->audio_fallback_elapsed = media_audio_sync_elapsed(b);
        if (!isfinite(b->audio_fallback_elapsed) || b->audio_fallback_elapsed < 0.0) b->audio_fallback_elapsed = 0.0;
        b->audio_fallback_wall = now;
    }
    b->pause_started = now;
    b->paused = 1;
    return 1;
#endif
}

int media_resume(MediaBackend *b) {
    if (!b) return 0;
#ifdef _WIN32
    return 0;
#else
    if (!b->paused) return 1;
    pid_t ids[2] = { b->audio_pid, b->video_pid };
    int continued[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (ids[i] <= 0) continue;
        if (kill(ids[i], SIGCONT) == 0) continued[i] = 1;
        else if (errno != ESRCH) {
            for (int j = 0; j < i; j++) if (continued[j]) (void)kill(ids[j], SIGSTOP);
            return 0;
        }
    }
    double now = media_monotonic_seconds();
    double pause_duration = now - b->pause_started;
    if (pause_duration < 0.0 || !isfinite(pause_duration)) pause_duration = 0.0;
    /* Video-only playback uses a monotonic deadline clock; shift its origin by
       the paused interval. Audio-clock playback is frozen by the SDL device. */
    if (b->clock_started && !(b->audio_dev && atomic_load_explicit(&b->audio_clock_started, memory_order_acquire)))
        b->clock_start += pause_duration;
    if (b->audio_clock_fallback) b->audio_fallback_wall = now;
    b->paused = 0;
    b->pause_started = 0.0;
    if (b->audio_dev && g_media_sdl.pause_device) g_media_sdl.pause_device(b->audio_dev, 0);
    return 1;
#endif
}

void media_stop(MediaBackend *b) {
    if (!b) return;
#ifndef _WIN32
    for (int i = 0; i < 2; i++) if (b->input_fd[i] >= 0) { close(b->input_fd[i]); b->input_fd[i] = -1; }
    if (b->audio_pcm_fd >= 0) close(b->audio_pcm_fd);
    if (b->frame_fd >= 0) close(b->frame_fd);
    if (b->frame_meta_fd >= 0) close(b->frame_meta_fd);
    pid_t ids[2] = { b->audio_pid, b->video_pid };
    /* A paused child must be continued after SIGTERM, otherwise waitpid can
       block forever on a process that has not had a chance to handle SIGTERM. */
    for (int i = 0; i < 2; i++) if (ids[i] > 0) { (void)kill(ids[i], SIGTERM); (void)kill(ids[i], SIGCONT); }
    for (int i = 0; i < 2; i++) if (ids[i] > 0) { int st; while (waitpid(ids[i], &st, 0) < 0 && errno == EINTR) {} }
    media_audio_close_device(b);
#endif
    free(b->frame); free(b);
}
