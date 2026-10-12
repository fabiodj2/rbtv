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
#include <sys/resource.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>

/* Safety notes (docs/17, docs/18):
 *   A1  nothing in the audio path does file I/O: log lines go through a bounded
 *       in-memory ring (trylock, drop on contention) drained by a low-priority
 *       logger thread that also caps the log size.
 *   A2  shared state is lock-protected; the real PCM is reference-counted so
 *       snd_pcm_close() cannot free it under a writer; one-time init is
 *       pthread_once.
 *   A4  24-bit samples are normalised with sign extension before metering,
 *       mixing and fading (the engine appears to leave the top byte clear, so
 *       a negative sample looked like a huge positive one).  Hardware sees the
 *       same low 24 bits.  AUDIOSHIM_SIGNEXT=0 turns it off for A/B tests.
 *   A5  the "headphone/cue has audio" decision is a decaying detector (needs
 *       100 ms above threshold to engage, holds 1.5 s after the last loud block)
 *       instead of a latch that the start-up transient could set forever.
 *   A3  snd_pcm_writei() loops over partial writes, recovers from xruns, and
 *       when the sink is unusable it still PACES the engine (sleeps the
 *       period) and counts the dropped frames instead of returning instantly.
 */

#ifdef __arm__
/* Enforce GLIBC_2.4 versioning for libdl on glibc 2.13 */
__asm__(".symver dlsym, dlsym@GLIBC_2.4");
__asm__(".symver dlopen, dlopen@GLIBC_2.4");
__asm__(".symver dlerror, dlerror@GLIBC_2.4");
__asm__(".symver dlclose, dlclose@GLIBC_2.4");

#ifndef SYS_mmap2
#define SYS_mmap2 __NR_mmap2
#endif
#endif /* __arm__ */

/* rbp's user_space_rtc_init() calls mmap(MAP_SHARED, fd=-1) after /dev/mem open
 * fails (we chmod 000 /dev/mem). That mmap returns MAP_FAILED and leaves a
 * dangling RTC pointer (0x10000023) which later causes SIGSEGV loops -> watchdog.
 * Redirect that broken combination to an anonymous private mapping so it succeeds
 * with zeroed memory and sched_clock/v2_get_cycles read 0 instead of crashing. */
#ifdef __arm__
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
#endif

#define LOG_PATH       "/tmp/audioshim.log"
#ifndef LOG_CAP_BYTES
#define LOG_CAP_BYTES  (256 * 1024)   /* rotate to LOG_PATH.1 beyond this */
#endif
#define LOG_SLOTS      128
#define LOG_LINE       192

static char g_log_ring[LOG_SLOTS][LOG_LINE];
static int g_log_head, g_log_count;
static unsigned long g_log_dropped;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

static void log_flush_lines(int *fd, long *size, const char *buf, size_t len)
{
    ssize_t w;
    if (*fd < 0) {
        *fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (*fd < 0) return;
        *size = (long)lseek(*fd, 0, SEEK_END);
    }
    w = write(*fd, buf, len);
    if (w > 0) *size += w;
    if (*size > LOG_CAP_BYTES) {          /* size cap: keep one old generation */
        close(*fd);
        *fd = -1;
        rename(LOG_PATH, LOG_PATH ".1");
        *size = 0;
    }
}

static void *log_thread(void *arg)
{
    static char out[LOG_SLOTS * LOG_LINE + 64];   /* only this thread uses it */
    struct sched_param sp;
    int fd = -1;
    long size = 0;
    (void)arg;

    /* never inherit a real-time class from whichever thread logged first */
    memset(&sp, 0, sizeof(sp));
    (void)syscall(SYS_sched_setscheduler, 0, 0 /* SCHED_OTHER */, &sp);
    (void)setpriority(PRIO_PROCESS, 0, 10);

    for (;;) {
        size_t len = 0;
        unsigned long dropped;
        int i;

        usleep(200000);
        pthread_mutex_lock(&g_log_lock);
        for (i = 0; i < g_log_count; i++) {
            const char *l = g_log_ring[(g_log_head + i) % LOG_SLOTS];
            size_t n = strlen(l);
            memcpy(out + len, l, n);
            len += n;
        }
        g_log_head = (g_log_head + g_log_count) % LOG_SLOTS;
        g_log_count = 0;
        dropped = g_log_dropped;
        g_log_dropped = 0;
        pthread_mutex_unlock(&g_log_lock);

        if (dropped) {
            len += (size_t)snprintf(out + len, 64, "audioshim: log dropped %lu line(s)\n", dropped);
        }
        if (len)
            log_flush_lines(&fd, &size, out, len);
    }
    return NULL;
}

static void log_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, log_thread, NULL) == 0)
        pthread_detach(t);
}

/* Safe from any thread, including the audio callback: formats into a local
 * buffer, then try-locks the ring; if the logger holds it or the ring is full
 * the line is dropped (and counted).  Never blocks, never touches a file. */
static void alog(const char *fmt, ...)
{
    char buf[LOG_LINE];
    va_list ap;

    pthread_once(&g_log_once, log_start);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (pthread_mutex_trylock(&g_log_lock) != 0) {
        g_log_dropped++;                       /* racy counter: diagnostics only */
        return;
    }
    if (g_log_count < LOG_SLOTS) {
        memcpy(g_log_ring[(g_log_head + g_log_count) % LOG_SLOTS], buf, sizeof(buf));
        g_log_count++;
    } else {
        g_log_dropped++;
    }
    pthread_mutex_unlock(&g_log_lock);
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
static int (*real_snd_pcm_recover)(snd_pcm_t *, int, int) = NULL;
static int (*real_snd_pcm_drop)(snd_pcm_t *) = NULL;
static snd_pcm_sframes_t (*real_snd_pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t) = NULL;
/* Control interface types */
typedef void snd_ctl_t;
typedef void snd_pcm_info_t;

static int (*real_snd_ctl_open)(snd_ctl_t **, const char *, int) = NULL;
static int (*real_snd_ctl_close)(snd_ctl_t *) = NULL;

static int g_signext = 1;            /* A4, see s24() below */
static pthread_once_t g_alsa_once = PTHREAD_ONCE_INIT;

static void init_real_alsa_once(void)
{
    {
        const char *e = getenv("AUDIOSHIM_SIGNEXT");
        if (e && e[0] == '0') g_signext = 0;
    }
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
    real_snd_pcm_recover = dlsym(lib, "snd_pcm_recover");
    real_snd_pcm_drop = dlsym(lib, "snd_pcm_drop");
    real_snd_pcm_writei = dlsym(lib, "snd_pcm_writei");
    real_snd_ctl_open = dlsym(lib, "snd_ctl_open");
    real_snd_ctl_close = dlsym(lib, "snd_ctl_close");

    alog("audioshim: real ALSA initialized\n");
}

/* pthread_once: every pointer above is filled before any caller proceeds */
static void init_real_alsa(void)
{
    pthread_once(&g_alsa_once, init_real_alsa_once);
}

/* ---- shared state --------------------------------------------------------
 * g_state_lock : g_real_playback, g_playback_open_count, g_real_refs
 * g_mix_lock   : g_mix4ch, g_peak_hp, cue detector (headphone thread writes,
 *                master thread reads - they may be the same thread or not)
 * Everything else below is touched only by the master writei caller. */
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mix_lock   = PTHREAD_MUTEX_INITIALIZER;
static int g_real_refs = 0;
static int32_t g_peak_hp = 0;
/* A5: cue detector, all under g_mix_lock; times are in master frames */
#define HP_THRESHOLD      100UL      /* 24-bit counts, ~ -98 dBFS */
#define HP_ATTACK_FRAMES  4410UL     /* 100 ms of continuous signal to engage */
#define HP_RELEASE_FRAMES 66150UL    /* 1.5 s hold after the last loud block */
static unsigned long long g_master_frames = 0;
static unsigned long long g_hp_loud_until = 0;
static unsigned long g_hp_loud_run = 0;
static int g_hp_state = 0;           /* last decision, for transition logging */

/* A4 */
static inline int32_t s24(int32_t v)
{
    if (!g_signext) return v;
    return (int32_t)((uint32_t)v << 8) >> 8;     /* sign-extend from bit 23 */
}

/* 4-channel audio buffer for Prime GO (S24_LE: 4 bytes per sample, 4 channels = 16 bytes/frame) */
#define MAX_FRAMES 4096
static int32_t g_mix4ch[MAX_FRAMES * 4];
static int32_t g_out2ch[MAX_FRAMES * 2];   /* Chromebit: HDMI stereo (S24_LE) */
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

static void startup_env_init(void)
{
    if (g_startup_mute_frames >= 0) return;
    long mute_ms = 1500, fade_ms = 300;
    const char *s = getenv("STARTUP_MUTE_MS");
    if (s && *s) mute_ms = atol(s);
    s = getenv("STARTUP_FADE_MS");
    if (s && *s) fade_ms = atol(s);
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

static int is_real(snd_pcm_t *pcm)
{
    int r;
    if (!pcm) return 0;
    pthread_mutex_lock(&g_state_lock);
    r = (pcm == g_real_playback);
    pthread_mutex_unlock(&g_state_lock);
    return r;
}

/* Reference-counted access to the real PCM for the streaming path:
 * snd_pcm_close() waits for the count to drop before freeing the handle. */
static snd_pcm_t *real_acquire(void)
{
    snd_pcm_t *p;
    pthread_mutex_lock(&g_state_lock);
    p = g_real_playback;
    if (p) g_real_refs++;
    pthread_mutex_unlock(&g_state_lock);
    return p;
}

static void real_release(void)
{
    pthread_mutex_lock(&g_state_lock);
    g_real_refs--;
    pthread_mutex_unlock(&g_state_lock);
}

/* Resolve the HDMI codec card by *name* instead of a fixed index.  Card
 * numbering is not stable: a USB audio device (e.g. a DDJ-400 left plugged in)
 * can enumerate before the platform ASoC card and take card 0, which would
 * silently route the mix to the controller instead of the TV.  Prefer a card
 * whose name mentions HDMI, fall back to the VEYRON platform card. */
static int pick_hdmi_device(char *out, size_t n)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    char line[256];
    int card = -1, hdmi = -1, veyron = -1;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        int idx;
        if (sscanf(line, " %d [", &idx) == 1) {
            card = idx;
            continue;
        }
        if (card < 0)
            continue;
        if (strstr(line, "HDMI") && hdmi < 0)
            hdmi = card;
        if (strstr(line, "VEYRON") && veyron < 0)
            veyron = card;
    }
    fclose(f);
    if (hdmi < 0)
        hdmi = veyron;
    if (hdmi < 0)
        return 0;
    snprintf(out, n, "hw:%d,0", hdmi);
    return 1;
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode)
{
    init_real_alsa();
    alog("audioshim: snd_pcm_open(name='%s', stream=%d, mode=%d)\n", name ? name : "null", stream, mode);

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        int idx;
        pthread_mutex_lock(&g_state_lock);
        idx = g_playback_open_count;
        if (idx == 0) {
            /* Output 0: Master -> Real HDMI (resolved by card name, see above) */
            if (!g_real_playback && real_snd_pcm_open) {
                int real_mode = mode & ~2; /* mask out SND_PCM_NONBLOCK so hardware paces audio clock */
                char dev[32];
                const char *want = "hw:0,0";
                if (pick_hdmi_device(dev, sizeof(dev)))
                    want = dev;
                int err = real_snd_pcm_open(&g_real_playback, want, SND_PCM_STREAM_PLAYBACK, real_mode);
                alog("audioshim: opened real %s for Master (mode=%d->%d), res=%d handle=%p\n",
                     want, mode, real_mode, err, g_real_playback);
                if (err < 0) {
                    err = real_snd_pcm_open(&g_real_playback, "default", SND_PCM_STREAM_PLAYBACK, real_mode);
                    alog("audioshim: fallback open 'default' res=%d handle=%p\n", err, g_real_playback);
                    if (err < 0)
                        g_real_playback = NULL;
                }
            }
            *pcm = g_real_playback;
        } else if (idx == 1) {
            /* Output 1: Headphone -> Virtual handle */
            alog("audioshim: mapped virtual Headphone device\n");
            *pcm = (snd_pcm_t *)&g_h_hp;
        } else if (idx == 2) {
            /* Output 2: Booth -> Virtual handle */
            alog("audioshim: mapped virtual Booth device\n");
            *pcm = (snd_pcm_t *)&g_h_booth;
        } else {
            alog("audioshim: mapped dummy output device %d\n", idx);
            *pcm = (snd_pcm_t *)&g_h_dummy;
        }
        g_playback_open_count++;
        pthread_mutex_unlock(&g_state_lock);
        return 0;
    } else {
        /* Capture / Mic -> Always virtual handle to prevent hardware contention */
        alog("audioshim: mapped virtual Capture device\n");
        *pcm = (snd_pcm_t *)&g_h_cap;
        return 0;
    }
}

int snd_pcm_close(snd_pcm_t *pcm)
{
    snd_pcm_t *p = NULL;
    int i, refs;

    init_real_alsa();
    alog("audioshim: snd_pcm_close(handle=%p)\n", pcm);

    pthread_mutex_lock(&g_state_lock);
    if (pcm && pcm == g_real_playback) {
        p = g_real_playback;
        g_real_playback = NULL;        /* new real_acquire() callers get NULL */
        g_playback_open_count = 0;
    }
    pthread_mutex_unlock(&g_state_lock);
    if (!p)
        return 0;

    /* unblock a writer stuck in a blocking write, then wait for it to leave */
    if (real_snd_pcm_drop)
        real_snd_pcm_drop(p);
    for (i = 0; i < 500; i++) {
        pthread_mutex_lock(&g_state_lock);
        refs = g_real_refs;
        pthread_mutex_unlock(&g_state_lock);
        if (refs <= 0) break;
        usleep(1000);
    }
    if (i == 500) {
        alog("audioshim: close: writer still active after 500 ms, leaking the handle\n");
        return 0;
    }
    if (real_snd_pcm_close)
        real_snd_pcm_close(p);
    return 0;
}

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    init_real_alsa();
    if (is_real(pcm) && real_snd_pcm_hw_params_any)
        return real_snd_pcm_hw_params_any(pcm, params);
    return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int access)
{
    init_real_alsa();
    alog("audioshim: set_access req=%d\n", access);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_access)
        return real_snd_pcm_hw_params_set_access(pcm, params, access);
    return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, int format)
{
    init_real_alsa();
    alog("audioshim: set_format req=%d\n", format);
    /* Chromebit HDMI hw:0,0 is stereo and natively supports S24_LE (format 6) */
    if (is_real(pcm) && real_snd_pcm_hw_params_set_format) {
        int err = real_snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S24_LE);
        alog("audioshim: real set_format(S24_LE=6) res=%d\n", err);
        return err;
    }
    return 0;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    init_real_alsa();
    alog("audioshim: set_channels req=%u -> setting 2ch on hw\n", val);
    if (is_real(pcm) && real_snd_pcm_hw_params_set_channels) {
        int err = real_snd_pcm_hw_params_set_channels(pcm, params, 2);
        alog("audioshim: real set_channels(2) res=%d\n", err);
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
        int err = real_snd_pcm_hw_params_set_rate_near(pcm, params, val, dir);
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
        int err = real_snd_pcm_hw_params_set_period_size_near(pcm, params, val, dir);
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
        int err = real_snd_pcm_hw_params_set_periods_near(pcm, params, val, dir);
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
        int err = real_snd_pcm_hw_params(pcm, params);
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
        err = real_snd_pcm_sw_params_current(pcm, params);
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
        err = real_snd_pcm_sw_params_set_silence_threshold(pcm, params, val);
    }
    alog("audioshim: set_silence_threshold(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params_set_silence_size(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_set_silence_size) {
        err = real_snd_pcm_sw_params_set_silence_size(pcm, params, val);
    }
    alog("audioshim: set_silence_size(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_set_start_threshold) {
        err = real_snd_pcm_sw_params_set_start_threshold(pcm, params, val);
    }
    alog("audioshim: set_start_threshold(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params_set_stop_threshold) {
        err = real_snd_pcm_sw_params_set_stop_threshold(pcm, params, val);
    }
    alog("audioshim: set_stop_threshold(%lu) res=%d\n", val, err);
    return 0; /* always succeed */
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_sw_params) {
        err = real_snd_pcm_sw_params(pcm, params);
    }
    alog("audioshim: snd_pcm_sw_params(pcm=%p) res=%d\n", pcm, err);
    return 0; /* always succeed */
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
    init_real_alsa();
    int err = 0;
    if (is_real(pcm) && real_snd_pcm_prepare) {
        err = real_snd_pcm_prepare(pcm);
    }
    alog("audioshim: snd_pcm_prepare(pcm=%p) res=%d\n", pcm, err);
    return 0; /* always succeed */
}

int snd_pcm_link(snd_pcm_t *pcm1, snd_pcm_t *pcm2)
{
    alog("audioshim: snd_pcm_link intercepted -> success\n");
    return 0;
}

/* A3: write `size` frames to the real PCM.  Loops over partial writes and
 * recovers from xruns/suspend.  If the sink cannot take the data the frames are
 * dropped, but never silently and never without pacing: the engine's playhead
 * is clocked by this call (docs/08), so returning instantly would let it run
 * away at 100 % CPU.  Returns the number of frames that were really written. */
static unsigned long g_hw_dropped_frames = 0;
static unsigned long g_hw_recoveries = 0;
static unsigned long g_hw_err_logs = 0;

static snd_pcm_uframes_t hw_write_all(snd_pcm_t *hw, const int32_t *buf,
                                      snd_pcm_uframes_t size)
{
    snd_pcm_uframes_t done = 0;
    int stalls = 0;

    while (done < size) {
        snd_pcm_sframes_t n = real_snd_pcm_writei(hw, buf + done * 2, size - done);
        if (n > 0) {
            done += (snd_pcm_uframes_t)n;
            stalls = 0;
            continue;
        }
        if (n == 0 || n == -EAGAIN || n == -EINTR) {
            if (++stalls > 2000) break;          /* ~2 s without progress */
            if (n != -EINTR) usleep(1000);
            continue;
        }
        /* real error: -EPIPE (xrun), -ESTRPIPE (suspend), -ENODEV, -EIO ... */
        g_hw_recoveries++;
        if (real_snd_pcm_recover) {
            if (real_snd_pcm_recover(hw, (int)n, 1) < 0) break;
        } else if (real_snd_pcm_prepare) {
            if (real_snd_pcm_prepare(hw) < 0) break;
        } else {
            break;
        }
        if (++stalls > 8) break;                 /* recover loop is not converging */
    }

    if (done < size) {
        snd_pcm_uframes_t lost = size - done;
        g_hw_dropped_frames += lost;
        if ((g_hw_err_logs++ % 100) == 0)
            alog("audioshim: HDMI sink not accepting audio: dropped %lu frame(s) total, "
                 "%lu recovery attempt(s)\n", g_hw_dropped_frames, g_hw_recoveries);
        usleep((useconds_t)(lost * 1000000UL / 44100UL));   /* keep the engine paced */
    }
    return done;
}

/* AUDIOSHIM_DUMP=1: classify the raw 32-bit words once (docs/18 A4: are the
 * 24-bit samples sign-extended?) */
static int g_dump_enabled = -1;
static int g_dump_done = 0;

static void dump_sample_format(const int32_t *src, snd_pcm_uframes_t size)
{
    unsigned long top00 = 0, topff = 0, other = 0, neg_unext = 0, nz = 0;
    snd_pcm_uframes_t i, first = size;

    for (i = 0; i < size * 2; i++) {
        uint32_t v = (uint32_t)src[i], top = v >> 24;
        if (!v) continue;
        nz++;
        if (first == size) first = i / 2;
        if (top == 0x00) { top00++; if (v & 0x800000u) neg_unext++; }
        else if (top == 0xff) topff++;
        else other++;
    }
    if (!nz) return;
    g_dump_done++;
    alog("audioshim: DUMP #%d words=%lu nonzero=%lu top00=%lu topFF=%lu other=%lu "
         "neg24_without_signext=%lu\n", g_dump_done, (unsigned long)(size * 2), nz,
         top00, topff, other, neg_unext);
    alog("audioshim: DUMP first nonzero frame %lu: %08x %08x %08x %08x\n",
         (unsigned long)first, (unsigned)src[first * 2], (unsigned)src[first * 2 + 1],
         (unsigned)src[(first + 1 < size ? first + 1 : first) * 2],
         (unsigned)src[(first + 1 < size ? first + 1 : first) * 2 + 1]);
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
    init_real_alsa();
    if (!buffer || size == 0) return size;

    if (size > MAX_FRAMES)
        size = MAX_FRAMES;

    const int32_t *src = (const int32_t *)buffer;
    static int32_t s_peak_master = 0;    /* master caller only */

    if (pcm == (snd_pcm_t *)&g_h_hp) {
        /* Headphone stream: interleave into Ch 2 (Left) and Ch 3 (Right) */
        int32_t pk = 0;
        pthread_mutex_lock(&g_mix_lock);
        for (snd_pcm_uframes_t i = 0; i < size; i++) {
            int32_t l = s24(src[i * 2 + 0]);
            int32_t r = s24(src[i * 2 + 1]);
            int32_t al = (l < 0) ? -l : l;
            int32_t ar = (r < 0) ? -r : r;
            if (al > pk) pk = al;
            if (ar > pk) pk = ar;
            g_mix4ch[i * 4 + 2] = l;
            g_mix4ch[i * 4 + 3] = r;
        }
        if (pk > g_peak_hp) g_peak_hp = pk;
        if ((unsigned long)pk > HP_THRESHOLD) {
            g_hp_loud_run += size;
            if (g_hp_loud_run >= HP_ATTACK_FRAMES)
                g_hp_loud_until = g_master_frames + HP_RELEASE_FRAMES;
        } else {
            g_hp_loud_run = 0;
        }
        pthread_mutex_unlock(&g_mix_lock);
        return size;
    }

    if (pcm == (snd_pcm_t *)&g_h_booth || pcm == (snd_pcm_t *)&g_h_dummy) {
        return size;
    }

    /* Master stream: interleave into Ch 0 (Left) and Ch 1 (Right), then build
     * the HDMI stereo pair: the headphone/cue pair when it has audio, otherwise
     * the master pair. */
    g_write_count++;
    int hp_changed = 0, hp_now = 0;
    pthread_mutex_lock(&g_mix_lock);
    {
        int hp = g_master_frames < g_hp_loud_until;
        g_master_frames += size;
        if (hp != g_hp_state) { g_hp_state = hp; hp_changed = 1; }
        hp_now = hp;
        for (snd_pcm_uframes_t i = 0; i < size; i++) {
            int32_t l = s24(src[i * 2 + 0]);
            int32_t r = s24(src[i * 2 + 1]);
            int32_t al = (l < 0) ? -l : l;
            int32_t ar = (r < 0) ? -r : r;
            if (al > s_peak_master) s_peak_master = al;
            if (ar > s_peak_master) s_peak_master = ar;
            g_mix4ch[i * 4 + 0] = l;
            g_mix4ch[i * 4 + 1] = r;
            if (!hp) {
                /* mirror Master into Ch 2 & 3 (Headphones) if no explicit cue audio */
                g_mix4ch[i * 4 + 2] = l;
                g_mix4ch[i * 4 + 3] = r;
            }
            g_out2ch[i * 2 + 0] = hp ? g_mix4ch[i * 4 + 2] : l;
            g_out2ch[i * 2 + 1] = hp ? g_mix4ch[i * 4 + 3] : r;
        }
    }
    pthread_mutex_unlock(&g_mix_lock);
    if (hp_changed)
        alog("audioshim: cue/headphone pair %s the HDMI output\n",
             hp_now ? "now feeds" : "released");

    if (g_dump_enabled < 0) {
        const char *e = getenv("AUDIOSHIM_DUMP");
        g_dump_enabled = (e && e[0] == '1');
    }
    if (g_dump_enabled && g_dump_done < 3)
        dump_sample_format(src, size);

    /* startup mute + fade-in over all output channels */
    startup_env_init();
    {
        unsigned long long t0   = g_startup_frames_done;
        unsigned long long mute = (unsigned long long)g_startup_mute_frames;
        unsigned long long fade = (unsigned long long)g_startup_fade_frames;
        g_startup_frames_done += size;
        if (mute > 0 && t0 < mute + fade) {
            if (!g_startup_logged_mute) {
                g_startup_logged_mute = 1;
                alog("audioshim: startup mute active: %llu+%llu frames (~%.1fs)\n",
                     mute, fade, (double)(mute + fade) / 44100.0);
            }
            for (snd_pcm_uframes_t i = 0; i < size; i++) {
                float g = startup_gain(t0 + i, mute, fade);
                if (g >= 1.0f) continue;
                for (int c = 0; c < 2; c++)
                    g_out2ch[i * 2 + c] = (int32_t)((float)g_out2ch[i * 2 + c] * g);
            }
        }
        if (!g_startup_logged_open && mute > 0 && g_startup_frames_done >= mute + fade) {
            g_startup_logged_open = 1;
            alog("audioshim: startup mute released after %llu frames\n", g_startup_frames_done);
        }
    }

    /* Output stereo to real HDMI hardware */
    snd_pcm_uframes_t written = 0;
    snd_pcm_t *hw = real_acquire();
    if (hw && real_snd_pcm_writei) {
        written = hw_write_all(hw, g_out2ch, size);
    } else {
        /* Safety sleep to pace real-time audio threads if hardware is delayed */
        usleep(size * 1000000 / 44100);
        written = size;
    }
    if (hw)
        real_release();

    if ((g_write_count % 500) == 1) {
        int32_t hp_pk;
        pthread_mutex_lock(&g_mix_lock);
        hp_pk = g_peak_hp;
        g_peak_hp = 0;
        pthread_mutex_unlock(&g_mix_lock);
        alog("audioshim: writei #%lu frames=%lu written=%lu peak_m=%d peak_hp=%d\n",
             g_write_count, (unsigned long)size, (unsigned long)written,
             s_peak_master, hp_pk);
        s_peak_master = 0;
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
int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize, const cpu_set_t *cpuset)
{
    (void)thread; (void)cpusetsize; (void)cpuset;
    return 0;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *cpuset)
{
    (void)pid; (void)cpusetsize; (void)cpuset;
    return 0;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
    (void)pid; (void)policy; (void)param;
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
{
    (void)thread; (void)policy; (void)param;
    return 0; /* completely disable SCHED_FIFO/SCHED_RR starvation */
}

int pthread_setschedprio(pthread_t thread, int prio)
{
    (void)thread; (void)prio;
    return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
    (void)attr; (void)policy;
    return 0;
}

int pthread_attr_setschedparam(pthread_attr_t *attr, const struct sched_param *param)
{
    (void)attr; (void)param;
    return 0;
}


