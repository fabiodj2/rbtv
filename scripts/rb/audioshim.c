#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* Enforce GLIBC_2.4 versioning for libdl on glibc 2.13 */
__asm__(".symver dlsym, dlsym@GLIBC_2.4");
__asm__(".symver dlopen, dlopen@GLIBC_2.4");
__asm__(".symver dlerror, dlerror@GLIBC_2.4");
__asm__(".symver dlclose, dlclose@GLIBC_2.4");

#ifndef SYS_mmap2
#define SYS_mmap2 __NR_mmap2
#endif

/* rbp's user_space_rtc_init() calls mmap(MAP_SHARED, fd=-1) after /dev/mem open
 * fails (we chmod 000 /dev/mem). That mmap returns MAP_FAILED and leaves a
 * dangling RTC pointer (0x10000023) which later causes SIGSEGV loops -> watchdog.
 * Redirect that broken combination to an anonymous private mapping so it succeeds
 * with zeroed memory and sched_clock/v2_get_cycles read 0 instead of crashing. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (fd < 0 && (flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS)) {
        flags = (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS;
        fd = -1;
        offset = 0;
    }
    return (void *)syscall(SYS_mmap2, addr, length, prot, flags, fd,
                           (unsigned long)offset >> 12);
}

#define LOG_PATH "/tmp/audioshim.log"

static void alog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

/* Opaque ALSA types */
typedef void snd_pcm_t;
typedef void snd_pcm_hw_params_t;
typedef void snd_pcm_sw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_STREAM_CAPTURE  1
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE   2
#define SND_PCM_FORMAT_S24_LE   6

/* Virtual handles for secondary streams */
static int g_h_hp     = 1;
static int g_h_booth  = 2;
static int g_h_dummy  = 3;
static int g_h_cap    = 4;

static snd_pcm_t *g_real_playback = NULL;
static int g_playback_open_count  = 0;

/* Real ALSA function pointers */
static int (*real_snd_pcm_open)(snd_pcm_t **, const char *, int, int) = NULL;
static int (*real_snd_pcm_close)(snd_pcm_t *) = NULL;
static int (*real_snd_pcm_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *) = NULL;
static int (*real_snd_pcm_hw_params_any)(snd_pcm_t *, snd_pcm_hw_params_t *) = NULL;
static int (*real_snd_pcm_hw_params_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int) = NULL;
static int (*real_snd_pcm_hw_params_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int) = NULL;
static int (*real_snd_pcm_hw_params_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int) = NULL;
static int (*real_snd_pcm_hw_params_set_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *) = NULL;
static int (*real_snd_pcm_hw_params_set_period_size_near)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *, int *) = NULL;
static int (*real_snd_pcm_hw_params_set_periods_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *) = NULL;
static int (*real_snd_pcm_sw_params_current)(snd_pcm_t *, snd_pcm_sw_params_t *) = NULL;
static int (*real_snd_pcm_sw_params_get_boundary)(const snd_pcm_sw_params_t *, snd_pcm_uframes_t *) = NULL;
static int (*real_snd_pcm_sw_params_set_silence_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params_set_silence_size)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params_set_start_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params_set_stop_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t) = NULL;
static int (*real_snd_pcm_sw_params)(snd_pcm_t *, snd_pcm_sw_params_t *) = NULL;
static int (*real_snd_pcm_prepare)(snd_pcm_t *) = NULL;
static snd_pcm_sframes_t (*real_snd_pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t) = NULL;
/* Control interface types */
typedef void snd_ctl_t;
typedef void snd_pcm_info_t;

static int (*real_snd_ctl_open)(snd_ctl_t **, const char *, int) = NULL;
static int (*real_snd_ctl_close)(snd_ctl_t *) = NULL;

static void init_real_alsa(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    void *lib = dlopen("libasound.so.2", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) {
        alog("audioshim: failed to dlopen libasound.so.2: %s\n", dlerror());
        return;
    }

    real_snd_pcm_open = dlsym(lib, "snd_pcm_open");
    real_snd_pcm_close = dlsym(lib, "snd_pcm_close");
    real_snd_pcm_hw_params = dlsym(lib, "snd_pcm_hw_params");
    real_snd_pcm_hw_params_any = dlsym(lib, "snd_pcm_hw_params_any");
    real_snd_pcm_hw_params_set_access = dlsym(lib, "snd_pcm_hw_params_set_access");
    real_snd_pcm_hw_params_set_format = dlsym(lib, "snd_pcm_hw_params_set_format");
    real_snd_pcm_hw_params_set_channels = dlsym(lib, "snd_pcm_hw_params_set_channels");
    real_snd_pcm_hw_params_set_rate_near = dlsym(lib, "snd_pcm_hw_params_set_rate_near");
    real_snd_pcm_hw_params_set_period_size_near = dlsym(lib, "snd_pcm_hw_params_set_period_size_near");
    real_snd_pcm_hw_params_set_periods_near = dlsym(lib, "snd_pcm_hw_params_set_periods_near");
    real_snd_pcm_sw_params_current = dlsym(lib, "snd_pcm_sw_params_current");
    real_snd_pcm_sw_params_get_boundary = dlsym(lib, "snd_pcm_sw_params_get_boundary");
    real_snd_pcm_sw_params_set_silence_threshold = dlsym(lib, "snd_pcm_sw_params_set_silence_threshold");
    real_snd_pcm_sw_params_set_silence_size = dlsym(lib, "snd_pcm_sw_params_set_silence_size");
    real_snd_pcm_sw_params_set_start_threshold = dlsym(lib, "snd_pcm_sw_params_set_start_threshold");
    real_snd_pcm_sw_params_set_stop_threshold = dlsym(lib, "snd_pcm_sw_params_set_stop_threshold");
    real_snd_pcm_sw_params = dlsym(lib, "snd_pcm_sw_params");
    real_snd_pcm_prepare = dlsym(lib, "snd_pcm_prepare");
    real_snd_pcm_writei = dlsym(lib, "snd_pcm_writei");
    real_snd_ctl_open = dlsym(lib, "snd_ctl_open");
    real_snd_ctl_close = dlsym(lib, "snd_ctl_close");

    alog("audioshim: real ALSA initialized\n");
}

/* 4-channel audio buffer for Prime GO (S24_LE: 4 bytes per sample, 4 channels = 16 bytes/frame) */
#define MAX_FRAMES 4096
static int32_t g_mix4ch[MAX_FRAMES * 4];
static int16_t g_out4ch[MAX_FRAMES * 4];   /* DDJ-400: 4ch interleaved S16_LE */
static unsigned long g_write_count = 0;

/* ---- Startup mute / fade-in --------------------------------------------
 * rb's first audio buffers contain a full-scale transient (0x800000, the
 * most-negative 24-bit value) that makes a loud pop through the speakers.
 * Hold every output channel at zero for STARTUP_MUTE_MS after the first
 * write, then fade in over STARTUP_FADE_MS so the unmute cannot click.
 *   STARTUP_MUTE_MS=0 -> disabled; defaults 1500 / 300 ms. */
static long g_startup_mute_frames = -1;
static long g_startup_fade_frames = -1;
static unsigned long long g_startup_frames_done = 0;
static int g_startup_logged_mute = 0;
static int g_startup_logged_open = 0;

static long parse_env_ms(const char *name, long fallback)
{
    const char *s = getenv(name);
    unsigned long value = 0;

    if (!s || !*s)
        return fallback;

    while (*s >= '0' && *s <= '9') {
        unsigned digit = (unsigned)(*s - '0');

        if (value > 360000UL)
            return 3600000L;

        value = value * 10UL + digit;
        s++;
    }

    if (*s != '\0')
        return fallback;

    if (value > 3600000UL)
        value = 3600000UL;

    return (long)value;
}

static void startup_env_init(void)
{
    if (g_startup_mute_frames >= 0) return;
    long mute_ms = parse_env_ms("STARTUP_MUTE_MS", 1500);
    long fade_ms = parse_env_ms("STARTUP_FADE_MS", 300);
    if (mute_ms < 0) mute_ms = 0;
    if (fade_ms < 0) fade_ms = 0;
    g_startup_mute_frames = mute_ms * 44100L / 1000L;
    g_startup_fade_frames = fade_ms * 44100L / 1000L;
    alog("audioshim: startup mute=%ldms fade=%ldms (%ld+%ld frames)\n",
         mute_ms, fade_ms, g_startup_mute_frames, g_startup_fade_frames);
}

static float startup_gain(unsigned long long t, unsigned long long mute,
                          unsigned long long fade)
{
    if (t < mute) return 0.0f;
    if (fade > 0 && t < mute + fade) return (float)(t - mute) / (float)fade;
    return 1.0f;
}


static int16_t s24_to_s16(int32_t sample)
{
    if (sample > 8388607)
        sample = 8388607;
    else if (sample < -8388608)
        sample = -8388608;

    sample >>= 8;

    if (sample > 32767)
        sample = 32767;
    else if (sample < -32768)
        sample = -32768;

    return (int16_t)sample;
}

static inline int is_real(snd_pcm_t *pcm)
{
    return (pcm && pcm == g_real_playback);
}

/* Use ALSA's stable card identifier instead of the enumeration index. */
static const char *audio_device(void)
{
    const char *configured = getenv("RX3_AUDIO_DEVICE");
    return (configured && *configured) ? configured : "hw:CARD=DDJ400,DEV=0";
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode)
{
    init_real_alsa();
    alog("audioshim: snd_pcm_open(name='%s', stream=%d, mode=%d)\n", name ? name : "null", stream, mode);

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        if (g_playback_open_count == 0) {
            /* Output 0: real DDJ-400 four-channel PCM. */
            if (!g_real_playback && real_snd_pcm_open) {
                int real_mode = mode & ~2;
                const char *want = audio_device();
                int err = real_snd_pcm_open(&g_real_playback, want,
                                            SND_PCM_STREAM_PLAYBACK, real_mode);
                alog("audioshim: opened real %s for 4ch Master/Cue "
                     "(mode=%d->%d), res=%d handle=%p\n",
                     want, mode, real_mode, err, g_real_playback);
                if (err < 0 || !g_real_playback) {
                    g_real_playback = NULL;
                    return err < 0 ? err : -ENODEV;
                }
            }
            *pcm = g_real_playback;
            g_playback_open_count++;
            return 0;
        } else if (g_playback_open_count == 1) {
            /* Output 1: Headphone -> Virtual handle */
            alog("audioshim: mapped virtual Headphone device\n");
            *pcm = (snd_pcm_t *)&g_h_hp;
            g_playback_open_count++;
            return 0;
        } else if (g_playback_open_count == 2) {
            /* Output 2: Booth -> Virtual handle */
            alog("audioshim: mapped virtual Booth device\n");
            *pcm = (snd_pcm_t *)&g_h_booth;
            g_playback_open_count++;
            return 0;
        } else {
            alog("audioshim: mapped dummy output device %d\n", g_playback_open_count);
            *pcm = (snd_pcm_t *)&g_h_dummy;
            g_playback_open_count++;
            return 0;
        }
    } else {
        /* Capture / Mic -> Always virtual handle to prevent hardware contention */
        alog("audioshim: mapped virtual Capture device\n");
        *pcm = (snd_pcm_t *)&g_h_cap;
        return 0;
    }
}

int snd_pcm_close(snd_pcm_t *pcm)
{
    init_real_alsa();
    alog("audioshim: snd_pcm_close(handle=%p)\n", pcm);

    if (is_real(pcm)) {
        if (g_real_playback && real_snd_pcm_close) {
            real_snd_pcm_close(g_real_playback);
            g_real_playback = NULL;
        }
        g_playback_open_count = 0;
    }
    return 0;
}

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    init_real_alsa();
    if (is_real(pcm) && real_snd_pcm_hw_params_any)
        return real_snd_pcm_hw_params_any(g_real_playback, params);
    return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int access)
{
    init_real_alsa();
    alog("audioshim: set_access req=%d\n", access);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_access)
        return real_snd_pcm_hw_params_set_access(g_real_playback, params, access);
    return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int format)
{
    init_real_alsa();
    alog("audioshim: set_format req=%d\n", format);
    /* DDJ-400 hardware output is fixed at interleaved S16_LE. */
    if (is_real(pcm) && real_snd_pcm_hw_params_set_format) {
        int err = real_snd_pcm_hw_params_set_format(g_real_playback, params, SND_PCM_FORMAT_S16_LE);
        alog("audioshim: real set_format(S16_LE=2) res=%d\n", err);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    init_real_alsa();
    alog("audioshim: set_channels req=%u -> setting 4ch on DDJ-400\n", val);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_channels) {
        int err = real_snd_pcm_hw_params_set_channels(g_real_playback, params, 4);
        alog("audioshim: real set_channels(4) res=%d\n", err);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    init_real_alsa();
    alog("audioshim: set_rate_near req=%u\n", val ? *val : 0);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_rate_near) {
        if (val) *val = 44100;
        int err = real_snd_pcm_hw_params_set_rate_near(g_real_playback, params, val, dir);
        alog("audioshim: real set_rate_near res=%d rate=%u\n", err, val ? *val : 0);
        return err;
    }
    if (val) *val = 44100;
    return 0;
}

int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir)
{
    init_real_alsa();
    alog("audioshim: set_period_size_near req=%lu\n", val ? *val : 0);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_period_size_near) {
        int err = real_snd_pcm_hw_params_set_period_size_near(g_real_playback, params, val, dir);
        alog("audioshim: real set_period_size_near res=%d period=%lu\n", err, val ? *val : 0);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    init_real_alsa();
    alog("audioshim: set_periods_near req=%u\n", val ? *val : 0);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_periods_near) {
        int err = real_snd_pcm_hw_params_set_periods_near(g_real_playback, params, val, dir);
        alog("audioshim: real set_periods_near res=%d periods=%u\n", err, val ? *val : 0);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *params, unsigned int *val)
{
    if (val) *val = 2;
    return 0;
}

int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *params, unsigned int *val)
{
    if (val) *val = 2;
    return 0;
}

int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int rate)
{
    return (rate == 44100) ? 0 : -EINVAL;
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    init_real_alsa();
    alog("audioshim: snd_pcm_hw_params(pcm=%p)\n", pcm);
    if (is_real(pcm) && real_snd_pcm_hw_params) {
        int err = real_snd_pcm_hw_params(g_real_playback, params);
        alog("audioshim: real hw_params res=%d\n", err);
        return err;
    }
    return 0;
}

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_current) {
        err = real_snd_pcm_sw_params_current(g_real_playback, params);
    }
    alog("audioshim: snd_pcm_sw_params_current(pcm=%p) res=%d\n", pcm, err);
    return err;
}

int snd_pcm_sw_params_get_boundary(const snd_pcm_sw_params_t *params, snd_pcm_uframes_t *val)
{
    init_real_alsa();
    int err = 0;
    if (real_snd_pcm_sw_params_get_boundary) {
        err = real_snd_pcm_sw_params_get_boundary(params, val);
    }
    if (err != 0 || !val || *val == 0) {
        if (val) *val = 0x40000000;
        err = 0;
    }
    alog("audioshim: get_boundary() res=%d val=%lx\n", err, val ? *val : 0);
    return err;
}

int snd_pcm_sw_params_set_silence_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_set_silence_threshold) {
        err = real_snd_pcm_sw_params_set_silence_threshold(g_real_playback, params, val);
    }
    alog("audioshim: set_silence_threshold(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params_set_silence_size(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_set_silence_size) {
        err = real_snd_pcm_sw_params_set_silence_size(g_real_playback, params, val);
    }
    alog("audioshim: set_silence_size(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_set_start_threshold) {
        err = real_snd_pcm_sw_params_set_start_threshold(g_real_playback, params, val);
    }
    alog("audioshim: set_start_threshold(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_set_stop_threshold) {
        err = real_snd_pcm_sw_params_set_stop_threshold(g_real_playback, params, val);
    }
    alog("audioshim: set_stop_threshold(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params) {
        err = real_snd_pcm_sw_params(g_real_playback, params);
    }
    alog("audioshim: snd_pcm_sw_params(pcm=%p) res=%d\n", pcm, err);
    return 0; /* always succeed */
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_prepare) {
        err = real_snd_pcm_prepare(g_real_playback);
    }
    alog("audioshim: snd_pcm_prepare(pcm=%p) res=%d\n", pcm, err);
    return 0; /* always succeed */
}

int snd_pcm_link(snd_pcm_t *pcm1, snd_pcm_t *pcm2)
{
    alog("audioshim: snd_pcm_link intercepted -> success\n");
    return 0;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
    init_real_alsa();
    if (!buffer || size == 0) return size;

    if (size > MAX_FRAMES)
        size = MAX_FRAMES;

    const int32_t *src = (const int32_t *)buffer;
    static int32_t s_peak_master = 0;
    static int32_t s_peak_hp = 0;
    static int s_has_hp_audio = 0;

    if (pcm == (snd_pcm_t *)&g_h_hp) {
        /* Headphone stream: interleave into Ch 2 (Left) and Ch 3 (Right) */
        for (snd_pcm_uframes_t i = 0; i < size; i++) {
            int32_t l = src[i * 2 + 0];
            int32_t r = src[i * 2 + 1];
            int32_t al = (l < 0) ? -l : l;
            int32_t ar = (r < 0) ? -r : r;
            if (al > s_peak_hp) s_peak_hp = al;
            if (ar > s_peak_hp) s_peak_hp = ar;
            g_mix4ch[i * 4 + 2] = l;
            g_mix4ch[i * 4 + 3] = r;
        }
        if (s_peak_hp > 100) s_has_hp_audio = 1;
        return size;
    }

    if (pcm == (snd_pcm_t *)&g_h_booth || pcm == (snd_pcm_t *)&g_h_dummy) {
        return size;
    }

    /* Master stream: interleave into Ch 0 (Left) and Ch 1 (Right) */
    for (snd_pcm_uframes_t i = 0; i < size; i++) {
        int32_t l = src[i * 2 + 0];
        int32_t r = src[i * 2 + 1];
        int32_t al = (l < 0) ? -l : l;
        int32_t ar = (r < 0) ? -r : r;
        if (al > s_peak_master) s_peak_master = al;
        if (ar > s_peak_master) s_peak_master = ar;
        g_mix4ch[i * 4 + 0] = l;
        g_mix4ch[i * 4 + 1] = r;
        /* Also mirror Master into Ch 2 & 3 (Headphones) if no explicit Headphone cue audio */
        if (!s_has_hp_audio) {
            g_mix4ch[i * 4 + 2] = l;
            g_mix4ch[i * 4 + 3] = r;
        }
    }

    g_write_count++;
    snd_pcm_sframes_t written = 0;
    /* startup mute + fade-in over all output channels */
    startup_env_init();
    {
        unsigned long long t0   = g_startup_frames_done;
        unsigned long long mute = (unsigned long long)g_startup_mute_frames;
        unsigned long long fade = (unsigned long long)g_startup_fade_frames;
        g_startup_frames_done += size;
        if ((mute > 0 || fade > 0) && t0 < mute + fade) {
            if (!g_startup_logged_mute) {
                g_startup_logged_mute = 1;
                alog("audioshim: startup mute active: %lu+%lu frames\n",
                     (unsigned long)mute, (unsigned long)fade);
            }
            for (snd_pcm_uframes_t i = 0; i < size; i++) {
                float g = startup_gain(t0 + i, mute, fade);
                if (g >= 1.0f) continue;
                for (int c = 0; c < 4; c++)
                    g_mix4ch[i * 4 + c] = (int32_t)((float)g_mix4ch[i * 4 + c] * g);
            }
        }
        if (!g_startup_logged_open && (mute > 0 || fade > 0) && g_startup_frames_done >= mute + fade) {
            g_startup_logged_open = 1;
            alog("audioshim: startup mute released after %lu frames\n",
                 (unsigned long)g_startup_frames_done);
        }
    }

    /* Convert the RX3 S24_LE samples to DDJ-400 S16_LE, preserving:
     * channels 0/1 = master and channels 2/3 = headphone cue. */
    for (snd_pcm_uframes_t i = 0; i < size; i++) {
        for (int c = 0; c < 4; c++)
            g_out4ch[i * 4 + c] = s24_to_s16(g_mix4ch[i * 4 + c]);
    }

    /* Output four interleaved channels to the DDJ-400. */
    if (g_real_playback && real_snd_pcm_writei) {
        written = real_snd_pcm_writei(g_real_playback, g_out4ch, size);
        if (written < 0) {
            if (real_snd_pcm_prepare)
                real_snd_pcm_prepare(g_real_playback);
            written = real_snd_pcm_writei(g_real_playback, g_out4ch, size);
        }
    } else {
        /* Safety sleep to pace real-time audio threads if hardware is delayed */
        usleep(size * 1000000 / 44100);
    }

    if ((g_write_count % 500) == 1) {
        alog("audioshim: writei #%lu frames=%lu written=%ld peak_m=%d peak_hp=%d\n",
             g_write_count, size, (long)written, s_peak_master, s_peak_hp);
        s_peak_master = 0;
        s_peak_hp = 0;
    }

    return size;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size)
{
    /* Always provide clean silence for capture to guarantee zero read errors */
    if (buffer && size > 0)
        memset(buffer, 0, size * 8);
    return size;
}

/* Control interface stubs */

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode)
{
    init_real_alsa();
    alog("audioshim: snd_ctl_open(name='%s', mode=%d)\n", name ? name : "null", mode);
    if (real_snd_ctl_open && name && (strstr(name, "hw:1") || strstr(name, "hw:0") || strstr(name, "default"))) {
        int err = real_snd_ctl_open(ctl, name, mode);
        if (err == 0) return 0;
    }
    if (ctl) *ctl = (snd_ctl_t *)0x12345;
    return 0;
}

int snd_ctl_close(snd_ctl_t *ctl)
{
    init_real_alsa();
    alog("audioshim: snd_ctl_close(handle=%p)\n", ctl);
    if (ctl != (snd_ctl_t *)0x12345 && real_snd_ctl_close) {
        return real_snd_ctl_close(ctl);
    }
    return 0;
}

int snd_ctl_pcm_info(snd_ctl_t *ctl, snd_pcm_info_t *info)
{
    alog("audioshim: snd_ctl_pcm_info(ctl=%p)\n", ctl);
    return 0;
}

/* Scheduler & Affinity stubs to prevent single-core lockup on Rockchip */
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const void *cpuset)
{
    (void)thread; (void)cpusetsize; (void)cpuset;
    return 0;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const void *cpuset)
{
    (void)pid; (void)cpusetsize; (void)cpuset;
    return 0;
}

int sched_setscheduler(pid_t pid, int policy, const void *param)
{
    (void)pid; (void)policy; (void)param;
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const void *param)
{
    (void)thread; (void)policy; (void)param;
    return 0; /* completely disable SCHED_FIFO/SCHED_RR starvation */
}

int pthread_setschedprio(pthread_t thread, int prio)
{
    (void)thread; (void)prio;
    return 0;
}

int pthread_attr_setschedpolicy(void *attr, int policy)
{
    (void)attr; (void)policy;
    return 0;
}

int pthread_attr_setschedparam(void *attr, const void *param)
{
    (void)attr; (void)param;
    return 0;
}


