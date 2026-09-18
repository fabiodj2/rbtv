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

/* ---- rbp process check ---- */
static int is_rbp_process(void)
{
    char cmd[128];
    int fd, n;
    fd = real_open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    n = (int)real_read(fd, cmd, sizeof(cmd) - 1);
    real_close(fd);
    if (n <= 0) return 0;
    cmd[n] = '\0';
    for (int i = 0; i < n; i++)
        if (cmd[i] == '\0') cmd[i] = ' ';
    return strstr(cmd, "rbp") != NULL;
}

/* ---- KeyManager injection ---- */
#define UI_OBJ_MGR_GLOBAL 0x2685f2cUL
#define KEY_MANAGER_OFF   100
#define SENDKEY_VTABLE_WORD 2

#define OP_PRESS   0
#define OP_RELEASE 2
#define OP_ROTATE  4
#define OP_VALUE   5

static int rbp_checked = -1;

static void *get_key_manager(void)
{
    void **p;
    void *mgr;
    if (rbp_checked < 0)
        rbp_checked = is_rbp_process();
    if (!rbp_checked)
        return NULL;
    p = (void **)UI_OBJ_MGR_GLOBAL;
    if (!p) return NULL;
    mgr = *p;
    if (!mgr) return NULL;
    return *(void **)((char *)mgr + KEY_MANAGER_OFF);
}

typedef void (*sendkey_fn)(void *km, int keycode, int op, int ch,
                           long param, float f, long l);

static void send_rx_key(int keycode, int op, int ch)
{
    void *km = get_key_manager();
    if (!km) {
        klog_str("send_rx_key: no key manager\n");
        return;
    }
    void **vt = *(void ***)km;
    sendkey_fn fn = (sendkey_fn)vt[SENDKEY_VTABLE_WORD];
    if (!fn) {
        klog_str("send_rx_key: vtable fn null\n");
        return;
    }
    klog_hex("sendkey key=", (unsigned)keycode);
    fn(km, keycode, op, ch, 0, 0.0f, 0);
}

static void send_rx_key_fl(int keycode, int op, int ch, long param, float f, long l)
{
    void *km = get_key_manager();
    if (!km)
        return;
    void **vt = *(void ***)km;
    sendkey_fn fn = (sendkey_fn)vt[SENDKEY_VTABLE_WORD];
    if (fn)
        fn(km, keycode, op, ch, param, f, l);
}

/* Mixer input routing (djengine::MixerRouteMngr) @ 0x01149f50/0x01149f54.
 * On the RX3 the physical DECK/LINE switches assign each mixer channel to a
 * player; with no sub-microcontroller both channels come up pointing at
 * Player 0, so the *channel-2 fader controls deck 1* (both faders move the
 * same sound).  Pin them like PrimeBox's knobshim2 (docs/05 §5):
 *   ch1 -> route object 0x01149f08 (Player 0 / deck 1)
 *   ch2 -> route object 0x01149f10 (Player 1 / deck 2)   */
#define MIX_ROUTE_CH1   0x01149f50UL
#define MIX_ROUTE_CH2   0x01149f54UL
#define MIX_ROUTE_PLAYER0 0x01149f08UL
#define MIX_ROUTE_PLAYER1 0x01149f10UL

static void mixer_route(void)
{
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
#define PANEL_PEER_INSTANCE_GLOBAL 0x26870b0UL  /* ui::PanelComPeerLinux::instance */
#define PANEL_PEER_NUM_DEVICES_OFF 0x94         /* +0x94 = nDevices */
#define JUCE_THREAD_OFF            4            /* juce::Thread subobject at +4 */
#define FN_THREAD_IS_RUNNING       0x3b6360UL   /* juce::Thread::isThreadRunning() const */
#define FN_THREAD_START            0x3b6674UL   /* juce::Thread::startThread(int) */
#define FN_OPEN_DEVICE             0x366804UL   /* ui::PanelComPeerLinux::openDevice(int) */

static int ui_pump_running(void)
{
    void *inst = *(void **)PANEL_PEER_INSTANCE_GLOBAL;
    if (!inst)
        return 0;
    return ((int (*)(void *))FN_THREAD_IS_RUNNING)((char *)inst + JUCE_THREAD_OFF);
}

static int ui_pump_ensure(void)
{
    void *inst;
    int n;
    if (ui_pump_running())
        return 1;
    inst = *(void **)PANEL_PEER_INSTANCE_GLOBAL;
    if (!inst)
        return 0;
    n = *(int *)((char *)inst + PANEL_PEER_NUM_DEVICES_OFF);
    if (n <= 0)
        return 0;   /* run() returns immediately while nDevices <= 0 */

    klog_str("keyshim: UiMain pump not running -> openDevice(0)\n");
    ((int (*)(void *, int))FN_OPEN_DEVICE)(inst, 0);

    if (!ui_pump_running()) {
        klog_str("keyshim: openDevice did not start it -> startThread\n");
        ((void (*)(void *, int))FN_THREAD_START)((char *)inst + JUCE_THREAD_OFF, 5);
    }
    if (ui_pump_running()) {
        klog_str("keyshim: UiMain pump started\n");
        return 1;
    }
    klog_str("keyshim: UiMain pump FAILED to start\n");
    return 0;
}

/* ---- clear the stale "USB Error / Remove Device" browse caution ----
 * TotalCnt_CautionTASK sets [0x05a191fc]=0x12d (caution 0xc021) from early USB
 * initialisation.  While non-zero the UI disables browse/touch handling.
 * On the phone no USB mount ever runs the
 * code that clears it, so clear it ourselves (same as knobshim2's usb_auto). */
#define BROWSE_CAUTION_ID 0x05a191fcUL

static void clear_browse_caution(void)
{
    volatile unsigned int *p = (volatile unsigned int *)BROWSE_CAUTION_ID;
    if (*p != 0)
        *p = 0;
}

/* ---- FIFO input ---- */
#define KEY_FIFO  "/tmp/rb-keys.fifo"
#define CTRL_FIFO "/tmp/rb-ctrl.fifo"

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
static volatile int ctrl_surface_active = 0;

static void *key_thread(void *arg)
{
    struct pollfd pfd;
    int fd = -1;
    (void)arg;

    klog_str("keyshim: thread started\n");

    for (int i = 0; i < 300; i++) {
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
    for (int i = 0; i < 150 && !ui_pump_running(); i++)
        ui_pump_ensure();

    /* Unmute the mixer: faders default to 0 on the phone (no subucom).  Send
     * after the pump exists so the messages are actually dispatched. */
    mixer_defaults();
    klog_str("keyshim: mixer defaults sent (ch1->deck1, ch2->deck2, faders up, "
             "trims/EQ/crossfader center)\n");

    for (int i = 0; i < 100; i++) {
        fd = real_open(KEY_FIFO, O_RDWR | O_NONBLOCK);
        if (fd >= 0)
            break;
        usleep(100000);
    }
    if (fd < 0) {
        klog_str("keyshim: fifo open failed\n");
        return NULL;
    }
    klog_str("keyshim: fifo open ok\n");

    pfd.fd = fd;
    pfd.events = POLLIN;

    /* The FIFO record is 12 bytes, but a writer may deliver it in several
     * write()s (this shim's own /tmp injectors do).  Accumulate across reads
     * instead of discarding partial data. */
    unsigned char rec[sizeof(struct key_ev)];
    size_t rec_len = 0;

    time_t last_mix = time(0);
    for (;;) {
        int pr = poll(&pfd, 1, 500);
        clear_browse_caution();   /* keep the USB-error popup dismissed */
        /* The mixer objects are created well after startup, so the first send
         * is often lost; keep re-applying them (idempotent, nothing physical
         * can change them on the phone). */
        time_t now = time(0);
        if (!ctrl_surface_active && now - last_mix >= 10) {
            mixer_defaults();
            last_mix = now;
        }
        if (pr <= 0)
            continue;
        ssize_t n = real_read(fd, rec + rec_len, sizeof(rec) - rec_len);
        if (n > 0) {
            rec_len += (size_t)n;
            if (rec_len == sizeof(rec)) {
                struct key_ev ev;
                memcpy(&ev, rec, sizeof(ev));
                rec_len = 0;

                if (ev.key == 0x7f01) {   /* host trigger: re-send mixer defaults */
                    mixer_defaults();
                    continue;
                }
                if (ev.key == 0x7f02) {   /* host trigger: rotate browse knob */
                    send_rx_key_fl(0x420c, OP_ROTATE, 1, ev.down, 0.0f, 0);
                    continue;
                }
                ui_pump_ensure();   /* restart pump if it ever exits */
                klog_dec("keyshim: got key ch=", ev.ch);
                klog_dec("         down=", ev.down);
                send_rx_key(ev.key, ev.down ? OP_PRESS : OP_RELEASE, ev.ch);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

/* ---- controller FIFO (/tmp/rb-ctrl.fifo) ----
 * 24-byte records, forwarded straight to sendKey(key, op, ch, param, f, l).
 * This is what the DDJ-400 MIDI bridge writes, so a real controller can drive
 * the engine's faders, EQ, jog wheels and relative knobs. */
static void *ctrl_thread(void *arg)
{
    struct pollfd pfd;
    int fd = -1;
    (void)arg;

    klog_str("keyshim: ctrl thread started\n");

    for (int i = 0; i < 300; i++) {
        if (get_key_manager())
            break;
        usleep(100000);
    }
    if (!get_key_manager()) {
        klog_str("keyshim: ctrl thread: key manager never ready\n");
        return NULL;
    }

    for (int i = 0; i < 150 && !ui_pump_running(); i++)
        ui_pump_ensure();

    for (int i = 0; i < 100; i++) {
        fd = real_open(CTRL_FIFO, O_RDWR | O_NONBLOCK);
        if (fd >= 0)
            break;
        usleep(100000);
    }
    if (fd < 0) {
        klog_str("keyshim: ctrl fifo open failed\n");
        return NULL;
    }
    klog_str("keyshim: ctrl fifo open ok\n");

    pfd.fd = fd;
    pfd.events = POLLIN;

    unsigned char rec[sizeof(struct ctrl_ev)];
    size_t rec_len = 0;

    for (;;) {
        int pr = poll(&pfd, 1, 1000);
        clear_browse_caution();
        if (pr <= 0)
            continue;
        ssize_t n = real_read(fd, rec + rec_len, sizeof(rec) - rec_len);
        if (n > 0) {
            rec_len += (size_t)n;
            if (rec_len == sizeof(rec)) {
                struct ctrl_ev ev;
                memcpy(&ev, rec, sizeof(ev));
                rec_len = 0;
                ctrl_surface_active = 1;
                ui_pump_ensure();
                klog_hex("keyshim: ctrl key=", (unsigned)ev.key);
                klog_dec("         op=", ev.op);
                klog_dec("         ch=", ev.ch);
                klog_dec("         param=", ev.param);
                send_rx_key_fl(ev.key, ev.op, ev.ch, ev.param, ev.f, ev.l);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

__attribute__((constructor))
static void keyshim_init(void)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, key_thread, NULL) == 0)
        pthread_detach(tid);
    if (pthread_create(&tid, NULL, ctrl_thread, NULL) == 0)
        pthread_detach(tid);
}
