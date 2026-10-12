/* keyshim.c - LD_PRELOAD shim: inject XDJ-RX3 button presses into rbp from a
 * FIFO, so the host-side viewer can map keyboard keys to rb buttons.
 *
 * Mechanism (from Prime GO knobshim2.c, live-verified):
 *   IUiObjManager singleton @ 0x2685f2c; KeyManager = *(obj+100);
 *   vtable word 2 = IKeyManager::sendKey(keycode, op, ch, param, f, l).
 *   op 0 = press, 2 = release, 4 = rotate, 5 = absolute value.
 *
 * Inputs (both live in /tmp, which the chroot bind-mounts to the host /tmp):
 *   /tmp/rb-keys.fifo  12-byte records { int32 key, ch, down }  (buttons)
 *   /tmp/rb-ctrl.fifo  24-byte records { key, ch, op, param, float f, l }
 *                      (controller events: faders, EQ, jog, relative knobs)
 *
 * The control FIFO is what a real MIDI controller bridge (ddj400-bridge.c)
 * feeds.  While such a bridge is active the periodic re-application of the
 * synthetic mixer defaults is disabled, so physical faders are not fought over.
 *
 * Safety (docs/17, docs/18):
 *   K1  every address above is only valid for ONE rbp build.  Before any thread
 *       touches engine memory the shim identifies the player (exact basename
 *       "rbp" in argv) and compares the md5 of its image with RBP_EXPECTED_MD5.
 *       Any mismatch => the shim stays completely inert (no writes, no calls).
 *   K2  UiMain's openDevice() is not idempotent: it is called at most once per
 *       "pump not running" episode, under a lock, with a retry back-off.
 *   K3  object/vtable sanity: the KeyManager, its vtable and the sendKey target
 *       must look right (aligned; vtable inside the rbp image; function inside
 *       an executable rbp mapping) before they are dereferenced/called.
 *   K4  ONE injector thread serves both FIFOs (no concurrent sendKey from the
 *       shim, no cross-thread flags).
 *   K11 kill-switch: KEYSHIM_DISABLE=1, /etc/rbtv/disable-keyshim or
 *       /etc/rbtv/disable-shims (checked inside the chroot).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <sys/syscall.h>

#include "rbtv_common.h"

static int real_open(const char *p, int flags)
{
    return syscall(SYS_openat, AT_FDCWD, p, flags, 0);
}
static ssize_t real_read(int fd, void *buf, size_t n)
{
    return syscall(SYS_read, fd, buf, n);
}
static int real_close(int fd)
{
    return syscall(SYS_close, fd);
}

/* ---- tiny logger (raw syscalls, no printf) ---- */
static void klog_str(const char *s)
{
    int fd = syscall(SYS_openat, AT_FDCWD, "/tmp/keyshim.log",
                     O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        (void)syscall(SYS_write, fd, s, strlen(s));
        (void)syscall(SYS_close, fd);
    }
}
static void klog_hex(const char *label, unsigned int v)
{
    char buf[64];
    int n = 0;
    static const char hex[] = "0123456789abcdef";
    while (*label && n < 32) buf[n++] = *label++;
    for (int i = 7; i >= 0; i--) buf[n++] = hex[(v >> (i * 4)) & 0xf];
    buf[n++] = '\n';
    buf[n] = 0;
    klog_str(buf);
}
static void klog_dec(const char *label, int v)
{
    char buf[64];
    int n = 0, i, neg = 0;
    char tmp[16];
    int t = 0;
    while (*label && n < 32) buf[n++] = *label++;
    if (v < 0) { neg = 1; v = -v; }
    do { tmp[t++] = "0123456789"[v % 10]; v /= 10; } while (v);
    if (neg) tmp[t++] = '-';
    for (i = t - 1; i >= 0; i--) buf[n++] = tmp[i];
    buf[n++] = '\n';
    buf[n] = 0;
    klog_str(buf);
}

/* ---- K1/K11: is this the exact rbp this shim was written for? ----
 * RBP_EXPECTED_MD5 is rbp-chromebit (docs/14 §2).  KEYSHIM_RBP_MD5=<32 hex>
 * overrides it when deliberately porting to another build (the addresses below
 * must then be re-checked by hand). */
#define RBP_EXPECTED_MD5 "18a64bc4d0ffd1cbd35f3a6ea447fca8"

static pthread_once_t verify_once = PTHREAD_ONCE_INIT;
static volatile int shim_ok = 0;   /* written once inside verify_once */

/* K3: address ranges of the rbp image, read from /proc/self/maps once the md5
 * matched.  img = any rbp mapping (vtables live in rodata/data), txt = the
 * executable ones (virtual-call targets).  KEYSHIM_NO_RANGE_CHECK=1 turns the
 * range test off (diagnostics only; alignment/NULL checks stay). */
static unsigned long img_lo, img_hi, txt_lo, txt_hi;

static int load_image_ranges(void)
{
    static char maps[65536];
    int fd = real_open("/proc/self/maps", O_RDONLY);
    size_t len = 0;
    ssize_t n;
    char *line, *end;

    if (fd < 0) return 0;
    while (len < sizeof(maps) - 1 &&
           (n = real_read(fd, maps + len, sizeof(maps) - 1 - len)) > 0)
        len += (size_t)n;
    real_close(fd);
    maps[len] = 0;

    for (line = maps; *line; line = end) {
        unsigned long lo, hi;
        char perms[8] = "";
        char *path;
        int off = 0;

        end = strchr(line, '\n');
        if (end) *end++ = 0; else end = line + strlen(line);
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &off) < 3)
            continue;
        path = line + off;
        if (!off || !*path || !rbtv_is_rbp_name(path))
            continue;
        if (!img_hi) { img_lo = lo; img_hi = hi; }
        if (lo < img_lo) img_lo = lo;
        if (hi > img_hi) img_hi = hi;
        if (perms[2] == 'x') {
            if (!txt_hi) { txt_lo = lo; txt_hi = hi; }
            if (lo < txt_lo) txt_lo = lo;
            if (hi > txt_hi) txt_hi = hi;
        }
    }
    return img_hi && txt_hi;
}

static int valid_obj(const void *p)
{
    return p && ((unsigned long)p & 3) == 0 && (unsigned long)p >= 0x10000UL;
}
static int in_img(const void *p)
{
    unsigned long v = (unsigned long)p;
    return valid_obj(p) && v >= img_lo && v < img_hi;
}
static int in_txt(const void *p)
{
    unsigned long v = (unsigned long)p;
    return v >= txt_lo && v < txt_hi;
}

static void do_verify(void)
{
    char path[256], got[33], msg[200];
    const char *want = getenv("KEYSHIM_RBP_MD5");

    if (!want || strlen(want) != 32)
        want = RBP_EXPECTED_MD5;
    if (!rbtv_find_rbp_path(path, sizeof(path)))
        return;                                  /* not the player: stay silent */
    if (rbtv_shim_disabled("keyshim", "KEYSHIM_DISABLE")) {
        klog_str("keyshim: DISABLED by kill-switch (env/flag file); inert\n");
        return;
    }
    if (rbtv_md5_file(path, got) < 0) {
        klog_str("keyshim: cannot hash the rbp image; inert\n");
        return;
    }
    if (strcmp(got, want) != 0) {
        snprintf(msg, sizeof(msg),
                 "keyshim: REFUSING to run: rbp md5 %s, expected %s; inert\n",
                 got, want);
        klog_str(msg);
        return;
    }
    if (getenv("KEYSHIM_NO_RANGE_CHECK")) {
        img_lo = 0; img_hi = ~0UL; txt_lo = 0; txt_hi = ~0UL;
    } else if (!load_image_ranges()) {
        klog_str("keyshim: cannot find the rbp mappings in /proc/self/maps; inert\n");
        return;
    }
    snprintf(msg, sizeof(msg),
             "keyshim: rbp image verified (md5 %s) img=%08lx-%08lx text=%08lx-%08lx\n",
             got, img_lo, img_hi, txt_lo, txt_hi);
    klog_str(msg);
    shim_ok = 1;
}

static int shim_enabled(void)
{
    pthread_once(&verify_once, do_verify);
    return shim_ok;
}

/* ---- KeyManager injection ---- */
#ifndef UI_OBJ_MGR_GLOBAL
#define UI_OBJ_MGR_GLOBAL 0x2685f2cUL
#endif
#define KEY_MANAGER_OFF   100
#define SENDKEY_VTABLE_WORD 2

#define OP_PRESS   0
#define OP_RELEASE 2
#define OP_ROTATE  4
#define OP_VALUE   5

static void *get_key_manager(void)
{
    void **p;
    void *mgr;
    if (!shim_enabled())
        return NULL;
    p = (void **)UI_OBJ_MGR_GLOBAL;
    if (!p) return NULL;
    mgr = *p;
    if (!valid_obj(mgr)) return NULL;
    return *(void **)((char *)mgr + KEY_MANAGER_OFF);
}

typedef void (*sendkey_fn)(void *km, int keycode, int op, int ch,
                           long param, float f, long l);

/* Both injector threads (buttons, controller) and mixer_defaults() go through
 * here; serialise them so sendKey() is never entered concurrently from the
 * shim (the engine's own UI thread is outside our control). */
static pthread_mutex_t inject_lock = PTHREAD_MUTEX_INITIALIZER;

static void warn_once(int *flag, const char *msg)
{
    if (!*flag) { *flag = 1; klog_str(msg); }
}

static int send_locked(int keycode, int op, int ch, long param, float f, long l,
                       int verbose)
{
    void *km, **vt;
    sendkey_fn fn;
    int ok = 0;
    static int w_obj, w_vt, w_fn;

    pthread_mutex_lock(&inject_lock);
    km = get_key_manager();
    if (!km) {
        if (verbose) klog_str("send_rx_key: no key manager\n");
    } else if (!valid_obj(km)) {
        warn_once(&w_obj, "keyshim: KeyManager pointer is not a plausible object; not injecting\n");
    } else if (!in_img(vt = *(void ***)km)) {
        warn_once(&w_vt, "keyshim: KeyManager vtable is outside the rbp image; not injecting\n");
    } else if (!in_txt(fn = (sendkey_fn)vt[SENDKEY_VTABLE_WORD])) {
        warn_once(&w_fn, "keyshim: sendKey target is outside rbp text; not injecting\n");
    } else {
        fn(km, keycode, op, ch, param, f, l);
        ok = 1;
    }
    pthread_mutex_unlock(&inject_lock);
    return ok;
}

static void send_rx_key(int keycode, int op, int ch)
{
    klog_hex("sendkey key=", (unsigned)keycode);
    send_locked(keycode, op, ch, 0, 0.0f, 0, 1);
}

static void send_rx_key_fl(int keycode, int op, int ch, long param, float f, long l)
{
    send_locked(keycode, op, ch, param, f, l, 0);
}

/* Mixer input routing (djengine::MixerRouteMngr) @ 0x01149f50/0x01149f54.
 * On the RX3 the physical DECK/LINE switches assign each mixer channel to a
 * player; with no sub-microcontroller both channels come up pointing at
 * Player 0, so the *channel-2 fader controls deck 1* (both faders move the
 * same sound).  Pin them like PrimeBox's knobshim2 (docs/05 §5):
 *   ch1 -> route object 0x01149f08 (Player 0 / deck 1)
 *   ch2 -> route object 0x01149f10 (Player 1 / deck 2)   */
#ifndef MIX_ROUTE_CH1
#define MIX_ROUTE_CH1   0x01149f50UL
#endif
#ifndef MIX_ROUTE_CH2
#define MIX_ROUTE_CH2   0x01149f54UL
#endif
#ifndef MIX_ROUTE_PLAYER0
#define MIX_ROUTE_PLAYER0 0x01149f08UL
#endif
#ifndef MIX_ROUTE_PLAYER1
#define MIX_ROUTE_PLAYER1 0x01149f10UL
#endif

static void mixer_route(void)
{
    if (!shim_ok) return;
    *(volatile unsigned int *)MIX_ROUTE_CH1 = MIX_ROUTE_PLAYER0;
    *(volatile unsigned int *)MIX_ROUTE_CH2 = MIX_ROUTE_PLAYER1;
}

/* Without the RX3 sub-microcontroller the mixer controls have no physical
 * position, so the engine keeps its defaults — channel faders 0.0 (= silence)
 * and the crossfader hard left.  Push sane defaults once the KeyManager is up
 * (same idea as the Prime GO knobshim2 mixer init, SESSION_STATE_12).
 * keycodes: FADER 0x501e, TRIM 0x5019, EQH/M/L 0x501a/b/c, XFADER 0x6017. */
static void mixer_defaults(void)
{
    int ch;
    mixer_route();  /* re-assert Input 0->deck1, Input 1->deck2 */
    for (ch = 1; ch <= 2; ch++) {
        send_rx_key_fl(0x501e, OP_ROTATE, ch, 1023, 1.0f, 0); /* channel fader up */
        send_rx_key_fl(0x5019, OP_ROTATE, ch, 512, 0.5f, 0);  /* trim center */
        send_rx_key_fl(0x501a, OP_ROTATE, ch, 512, 0.5f, 0);  /* HI */
        send_rx_key_fl(0x501b, OP_ROTATE, ch, 512, 0.5f, 0);  /* MID */
        send_rx_key_fl(0x501c, OP_ROTATE, ch, 512, 0.5f, 0);  /* LOW */
    }
    send_rx_key_fl(0x6017, OP_ROTATE, 1, 512, 0.5f, 0);       /* crossfader center */
}

/* ---- PanelComPeer "UiMain" message-pump thread ----
 * The uif::MsgManager that IKeyManager::sendKey posts to is the MsgManager
 * sub-object of the PanelComPeerLinux singleton (mainMsgManager @ 0x268612c).
 * Its queue is drained ONLY by PanelComPeerLinux::run() (thread name "UiMain"):
 * it loops takeOutMessage() -> MsgManager::handleMessage().  That thread is
 * started by PanelComPeerLinux::openDevice(); on the phone the subucom open
 * path never runs, so no UiMain thread exists and every injected key sits in
 * the queue forever (IKeyManager::onKey is never reached).  Start it here.
 */
#ifndef PANEL_PEER_INSTANCE_GLOBAL
#define PANEL_PEER_INSTANCE_GLOBAL 0x26870b0UL  /* ui::PanelComPeerLinux::instance */
#endif
#define PANEL_PEER_NUM_DEVICES_OFF 0x94         /* +0x94 = nDevices */
#define JUCE_THREAD_OFF            4            /* juce::Thread subobject at +4 */
#ifndef FN_THREAD_IS_RUNNING
#define FN_THREAD_IS_RUNNING       0x3b6360UL   /* juce::Thread::isThreadRunning() const */
#endif
#ifndef FN_THREAD_START
#define FN_THREAD_START            0x3b6674UL   /* juce::Thread::startThread(int) */
#endif
#ifndef FN_OPEN_DEVICE
#define FN_OPEN_DEVICE             0x366804UL   /* ui::PanelComPeerLinux::openDevice(int) */
#endif

static int ui_pump_running(void)
{
    void *inst;
    if (!shim_ok)
        return 0;
    inst = *(void **)PANEL_PEER_INSTANCE_GLOBAL;
    if (!inst)
        return 0;
    return ((int (*)(void *))FN_THREAD_IS_RUNNING)((char *)inst + JUCE_THREAD_OFF);
}

/* K2: PanelComPeerLinux::openDevice() is NOT idempotent - a second call on an
 * initialised instance re-runs the epoll/device setup and UiMain then faults at
 * NULL+0x10 (docs/16 §5).  So:
 *   - one attempt per "pump not running" episode (pump_opened_at != 0),
 *   - serialised by pump_lock (both injector threads call this),
 *   - a retry only after PUMP_RETRY_S, never in a tight loop. */
#define PUMP_RETRY_S 5
static pthread_mutex_t pump_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile time_t pump_opened_at = 0;

static int ui_pump_ensure(void)
{
    void *inst;
    int n, i, ok;
    time_t now;

    if (!shim_ok)
        return 0;
    if (ui_pump_running()) {
        pump_opened_at = 0;
        return 1;
    }

    pthread_mutex_lock(&pump_lock);
    if (ui_pump_running()) {                 /* the other thread won */
        pump_opened_at = 0;
        pthread_mutex_unlock(&pump_lock);
        return 1;
    }
    inst = *(void **)PANEL_PEER_INSTANCE_GLOBAL;
    if (!inst) {
        pthread_mutex_unlock(&pump_lock);
        return 0;
    }
    n = *(int *)((char *)inst + PANEL_PEER_NUM_DEVICES_OFF);
    if (n <= 0) {                            /* run() returns while nDevices <= 0 */
        pthread_mutex_unlock(&pump_lock);
        return 0;
    }
    now = time(0);
    if (pump_opened_at && now - pump_opened_at < PUMP_RETRY_S) {
        pthread_mutex_unlock(&pump_lock);    /* attempt in flight / backing off */
        return 0;
    }
    pump_opened_at = now;

    klog_str("keyshim: UiMain pump not running -> openDevice(0)\n");
    ((int (*)(void *, int))FN_OPEN_DEVICE)(inst, 0);
    for (i = 0; i < 10 && !ui_pump_running(); i++)   /* the thread starts async */
        usleep(100000);

    if (!ui_pump_running()) {
        klog_str("keyshim: openDevice did not start it -> startThread\n");
        ((void (*)(void *, int))FN_THREAD_START)((char *)inst + JUCE_THREAD_OFF, 5);
        for (i = 0; i < 10 && !ui_pump_running(); i++)
            usleep(100000);
    }
    ok = ui_pump_running();
    klog_str(ok ? "keyshim: UiMain pump started\n"
                : "keyshim: UiMain pump FAILED to start (retry in 5 s)\n");
    if (ok)
        pump_opened_at = 0;
    pthread_mutex_unlock(&pump_lock);
    return ok;
}

/* ---- clear the stale "USB Error / Remove Device" browse caution ----
 * TotalCnt_CautionTASK sets [0x05a191fc]=0x12d (caution 0xc021) from early USB
 * initialisation.  While non-zero the UI disables browse/touch handling.
 * On the phone no USB mount ever runs the
 * code that clears it, so clear it ourselves (same as knobshim2's usb_auto). */
#ifndef BROWSE_CAUTION_ID
#define BROWSE_CAUTION_ID 0x05a191fcUL
#endif

static void clear_browse_caution(void)
{
    volatile unsigned int *p = (volatile unsigned int *)BROWSE_CAUTION_ID;
    if (!shim_ok)
        return;
    if (*p != 0)
        *p = 0;
}

/* ---- FIFO input ---- */
#ifndef KEY_FIFO
#define KEY_FIFO  "/tmp/rb-keys.fifo"
#endif
#ifndef CTRL_FIFO
#define CTRL_FIFO "/tmp/rb-ctrl.fifo"
#endif

struct key_ev {
    int32_t key;
    int32_t ch;
    int32_t down;
};

/* controller event: op is one of OP_PRESS/OP_RELEASE/OP_ROTATE/OP_VALUE.
 *   op 4 (ROTATE): param = relative delta, f = speed, l = position/counter
 *   op 5 (VALUE) : param = 10-bit absolute, f = normalised value
 * Same payload layout as the sendKey vtable call, so it can be forwarded
 * verbatim.  Must stay 24 bytes and fixed-size (a FIFO has no framing). */
struct ctrl_ev {
    int32_t key;
    int32_t ch;
    int32_t op;
    int32_t param;
    float   f;
    int32_t l;
};

/* Set once a real control surface starts feeding /tmp/rb-ctrl.fifo: from then
 * on the synthetic mixer defaults are no longer re-applied every 10 s (they
 * would snap the physical faders/EQ back). */
static int ctrl_surface_active = 0;   /* injector thread only (K4) */

/* one 12-byte button record */
static void handle_key_rec(const unsigned char *rec)
{
    struct key_ev ev;
    memcpy(&ev, rec, sizeof(ev));

    if (ev.key == 0x7f01) {   /* host trigger: re-send mixer defaults */
        mixer_defaults();
        return;
    }
    if (ev.key == 0x7f02) {   /* host trigger: rotate browse knob */
        send_rx_key_fl(0x420c, OP_ROTATE, 1, ev.down, 0.0f, 0);
        return;
    }
    ui_pump_ensure();   /* restart pump if it ever exits (guarded, K2) */
    klog_dec("keyshim: got key ch=", ev.ch);
    klog_dec("         down=", ev.down);
    send_rx_key(ev.key, ev.down ? OP_PRESS : OP_RELEASE, ev.ch);
}

/* one 24-byte controller record, forwarded straight to
 * sendKey(key, op, ch, param, f, l) - what the DDJ-400 bridge / rbkeyd write */
static void handle_ctrl_rec(const unsigned char *rec)
{
    struct ctrl_ev ev;
    memcpy(&ev, rec, sizeof(ev));

    ctrl_surface_active = 1;
    ui_pump_ensure();
    klog_hex("keyshim: ctrl key=", (unsigned)ev.key);
    klog_dec("         op=", ev.op);
    klog_dec("         ch=", ev.ch);
    klog_dec("         param=", ev.param);
    send_rx_key_fl(ev.key, ev.op, ev.ch, ev.param, ev.f, ev.l);
}

/* Feed one FIFO: accumulate `want` bytes (a writer may deliver a record in
 * several write()s), dispatch each complete record.  Returns 0 normally, -1 on
 * a hard read error (caller drops the fd). */
static int drain_fifo(int fd, unsigned char *rec, size_t want, size_t *have,
                      void (*handle)(const unsigned char *))
{
    int k;
    for (k = 0; k < 64; k++) {              /* bounded burst per wake-up */
        ssize_t n = real_read(fd, rec + *have, want - *have);
        if (n > 0) {
            *have += (size_t)n;
            if (*have == want) {
                *have = 0;
                handle(rec);
            }
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR)
            return -1;
        break;
    }
    return 0;
}

/* K4: the only thread that talks to the engine.  It owns both FIFOs, so
 * sendKey()/mixer defaults/pump handling are never concurrent with each other
 * and the ctrl_surface_active flag needs no cross-thread synchronisation. */
static void *inject_thread(void *arg)
{
    struct pollfd pfd[2];
    unsigned char krec[sizeof(struct key_ev)], crec[sizeof(struct ctrl_ev)];
    size_t klen = 0, clen = 0;
    int kfd = -1, cfd = -1, i;
    time_t last_mix;
    (void)arg;

    if (!shim_enabled())
        return NULL;                          /* K1/K11: stay inert */
    klog_str("keyshim: thread started\n");

    for (i = 0; i < 300; i++) {
        if (get_key_manager())
            break;
        usleep(100000);
    }
    if (!get_key_manager()) {
        klog_str("keyshim: key manager never ready\n");
        return NULL;
    }
    klog_hex("keyshim: key manager ready km=", (unsigned)(uintptr_t)get_key_manager());

    /* Dismiss the stale USB-error caution early so browse/touch isn't gated. */
    clear_browse_caution();

    /* Wait for the panel devices to be registered, then make sure the UiMain
     * message-pump thread exists (it is what dispatches posted keys). */
    for (i = 0; i < 150; i++) {
        if (ui_pump_ensure())
            break;
        usleep(100000);                       /* never spin on openDevice() */
    }

    /* Unmute the mixer: faders default to 0 on the phone (no subucom).  Send
     * after the pump exists so the messages are actually dispatched. */
    mixer_defaults();
    klog_str("keyshim: mixer defaults sent (ch1->deck1, ch2->deck2, faders up, "
             "trims/EQ/crossfader center)\n");

    for (i = 0; i < 100 && (kfd < 0 || cfd < 0); i++) {
        if (kfd < 0) kfd = real_open(KEY_FIFO, O_RDWR | O_NONBLOCK);
        if (cfd < 0) cfd = real_open(CTRL_FIFO, O_RDWR | O_NONBLOCK);
        if (kfd < 0 || cfd < 0)
            usleep(100000);
    }
    if (kfd < 0 && cfd < 0) {
        klog_str("keyshim: fifo open failed\n");
        return NULL;
    }
    klog_str(kfd >= 0 ? "keyshim: fifo open ok\n" : "keyshim: key fifo open FAILED\n");
    klog_str(cfd >= 0 ? "keyshim: ctrl fifo open ok\n" : "keyshim: ctrl fifo open FAILED\n");

    last_mix = time(0);
    for (;;) {
        time_t now;
        int pr;

        pfd[0].fd = kfd; pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = cfd; pfd[1].events = POLLIN; pfd[1].revents = 0;
        pr = poll(pfd, 2, 500);               /* fd == -1 entries are ignored */

        clear_browse_caution();   /* keep the USB-error popup dismissed */
        /* The mixer objects are created well after startup, so the first send
         * is often lost; keep re-applying them (idempotent, nothing physical
         * can change them on the phone). */
        now = time(0);
        if (!ctrl_surface_active && now - last_mix >= 10) {
            mixer_defaults();
            last_mix = now;
        }
        if (pr <= 0)
            continue;

        if (kfd >= 0 && (pfd[0].revents & (POLLIN | POLLERR | POLLNVAL)) &&
            drain_fifo(kfd, krec, sizeof(krec), &klen, handle_key_rec) < 0) {
            klog_str("keyshim: key fifo read error; closing it\n");
            real_close(kfd); kfd = -1; klen = 0;
        }
        if (cfd >= 0 && (pfd[1].revents & (POLLIN | POLLERR | POLLNVAL)) &&
            drain_fifo(cfd, crec, sizeof(crec), &clen, handle_ctrl_rec) < 0) {
            klog_str("keyshim: ctrl fifo read error; closing it\n");
            real_close(cfd); cfd = -1; clen = 0;
        }
        if (kfd < 0 && cfd < 0) {
            klog_str("keyshim: both fifos gone; injector exiting\n");
            return NULL;
        }
    }
    return NULL;
}

__attribute__((constructor))
static void keyshim_init(void)
{
    pthread_t tid;
    char path[256];

    /* Cheap pre-checks only (no hashing, no logging here): do nothing in
     * processes that are not the player (children of system() inherit
     * LD_PRELOAD) or when the kill-switch is set. */
    if (!rbtv_find_rbp_path(path, sizeof(path)))
        return;
    if (rbtv_shim_disabled("keyshim", "KEYSHIM_DISABLE"))
        return;
    if (pthread_create(&tid, NULL, inject_thread, NULL) == 0)
        pthread_detach(tid);
}
