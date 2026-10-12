/* injecttest.c - host test for keyshim's injector thread (K3 + K4) with a
 * mocked engine: fake IUiObjManager/KeyManager/vtable, fake UiMain pump, real
 * FIFOs.  Built by run-host-tests.sh, which #includes ../keyshim.c. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

/* ---- fake PanelComPeerLinux / pump ---- */
static struct { char pad[0x200]; } fake_inst_mem;
static void *fake_inst_ptr = &fake_inst_mem;
static volatile int open_calls;
static volatile double start_at = 1e18;
static double nowd(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int fake_is_running(void *thr){ return nowd() >= start_at; }
static int fake_open(void *inst,int n){ open_calls++; start_at = nowd()+0.05; return 0; }
static void fake_start(void *thr,int prio){ }

/* ---- fake KeyManager chain ---- */
struct fake_km { void **vt; };
static volatile int in_send, overlap, ncalls;
struct call { int key, op, ch, param; } calls[4096];
static void fake_sendkey(void *km, int key, int op, int ch, long param, float f, long l)
{
    int c;
    if (__sync_add_and_fetch(&in_send, 1) > 1) overlap = 1;
    c = ncalls;
    if (c < 4096) { calls[c].key = key; calls[c].op = op; calls[c].ch = ch; calls[c].param = (int)param; }
    usleep(50);                       /* widen any overlap window */
    ncalls = c + 1;
    __sync_sub_and_fetch(&in_send, 1);
}
static void other_fn(void *km, int key, int op, int ch, long param, float f, long l) { ncalls += 100000; }
static void *good_vt[3] = { 0, 0, (void *)fake_sendkey };
static void *bad_fn_vt[3] = { 0, 0, (void *)other_fn };
static struct fake_km km_obj = { good_vt };
static struct __attribute__((packed)) { char pad[100]; void *km; char pad2[64]; } mgr = { {0}, &km_obj };
static void *mgr_ptr = &mgr;
static unsigned int fake_route[2], fake_caution;

#define UI_OBJ_MGR_GLOBAL ((unsigned long)&mgr_ptr)
#define PANEL_PEER_INSTANCE_GLOBAL ((unsigned long)&fake_inst_ptr)
#define FN_THREAD_IS_RUNNING ((unsigned long)&fake_is_running)
#define FN_THREAD_START ((unsigned long)&fake_start)
#define FN_OPEN_DEVICE ((unsigned long)&fake_open)
#define MIX_ROUTE_CH1 ((unsigned long)&fake_route[0])
#define MIX_ROUTE_CH2 ((unsigned long)&fake_route[1])
#define BROWSE_CAUTION_ID ((unsigned long)&fake_caution)
#define KEY_FIFO  "/tmp/rbtv-test-keys.fifo"
#define CTRL_FIFO "/tmp/rbtv-test-ctrl.fifo"
#include "keyshim.c"

static int fails;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n",msg); fails++; } else printf("ok:   %s\n",msg);}while(0)
static int wait_calls(int n, double secs){ double t=nowd(); while(nowd()-t<secs){ if(ncalls>=n) return 1; usleep(2000);} return ncalls>=n; }

static int kw, cw;
static void put_key(int key,int ch,int down){ struct key_ev e={key,ch,down}; (void)!write(kw,&e,sizeof e); }
static void put_ctrl(int key,int ch,int op,int param){ struct ctrl_ev e={key,ch,op,param,0.5f,0}; (void)!write(cw,&e,sizeof e); }
static void *kspam(void *a){ for(int i=0;i<500;i++){ put_key(0x4101,1,i&1); if(i%32==0) usleep(100);} return NULL; }
static void *cspam(void *a){ for(int i=0;i<500;i++){ put_ctrl(0x501e,2,4,i); if(i%32==0) usleep(100);} return NULL; }

int main(void)
{
    pthread_t th, a, b;
    int base;
    unlink(KEY_FIFO); unlink(CTRL_FIFO);
    mkfifo(KEY_FIFO, 0666); mkfifo(CTRL_FIFO, 0666);
    kw = open(KEY_FIFO, O_RDWR|O_NONBLOCK); cw = open(CTRL_FIFO, O_RDWR|O_NONBLOCK);

    *(int *)((char *)&fake_inst_mem + PANEL_PEER_NUM_DEVICES_OFF) = 1;
    shim_ok = 1;                                          /* K1 is tested elsewhere */
    img_lo = (unsigned long)good_vt - 0x100; img_hi = (unsigned long)bad_fn_vt + 0x100;
    if (img_lo > img_hi) { unsigned long t=img_lo; img_lo=img_hi; img_hi=t; }
    txt_lo = (unsigned long)fake_sendkey; txt_hi = txt_lo + 0x40;   /* other_fn is outside */
    if ((unsigned long)other_fn >= txt_lo && (unsigned long)other_fn < txt_hi) { puts("layout unsuitable"); return 2; }

    pthread_create(&th, NULL, inject_thread, NULL);
    CHECK(wait_calls(11, 3.0), "startup: mixer defaults reach sendKey through the mocked chain");
    CHECK(open_calls == 1, "startup: openDevice called once");
    usleep(200000); base = ncalls;

    put_key(0x4101, 1, 1);
    CHECK(wait_calls(base+1, 1.0) && calls[base].key==0x4101 && calls[base].op==OP_PRESS && calls[base].ch==1, "button record -> sendKey press");
    base = ncalls;
    put_ctrl(0x501e, 1, 4, 512);
    CHECK(wait_calls(base+1, 1.0) && calls[base].key==0x501e && calls[base].op==4 && calls[base].param==512, "controller record -> sendKey rotate");
    CHECK(ctrl_surface_active == 1, "controller activity stops the synthetic mixer defaults");

    base = ncalls;
    { struct ctrl_ev e={0x5019,2,4,300,0.1f,0}; unsigned char *p=(unsigned char*)&e;
      (void)!write(cw,p,5); usleep(20000); (void)!write(cw,p+5,sizeof e-5); }
    CHECK(wait_calls(base+1, 1.0) && calls[base].key==0x5019 && ncalls==base+1, "record split across two write()s is reassembled (one event)");

    base = ncalls;
    pthread_create(&a, NULL, kspam, NULL); pthread_create(&b, NULL, cspam, NULL);
    pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(wait_calls(base+1000, 5.0) && ncalls==base+1000, "1000 records from both FIFOs all delivered");
    CHECK(!overlap, "sendKey never entered concurrently (single injector, K4)");

    /* K3: corrupt the object graph */
    base = ncalls;
    km_obj.vt = (void **)malloc(64);                       /* vtable outside the image */
    ((void **)km_obj.vt)[2] = (void *)fake_sendkey;
    put_key(0x4101, 1, 1); usleep(150000);
    CHECK(ncalls == base, "vtable outside the rbp image -> not called");
    km_obj.vt = bad_fn_vt;                                 /* target outside text */
    put_key(0x4101, 1, 1); usleep(150000);
    CHECK(ncalls == base, "sendKey target outside rbp text -> not called");
    km_obj.vt = good_vt;
    mgr.km = (void *)0x1235;                               /* misaligned/garbage object */
    put_key(0x4101, 1, 1); usleep(150000);
    CHECK(ncalls == base, "misaligned KeyManager pointer -> not called");
    mgr.km = &km_obj;
    put_key(0x4101, 1, 0);
    CHECK(wait_calls(base+1, 1.0), "recovers once the object graph is sane again");

    puts(fails ? "INJECT TESTS FAILED" : "INJECT TESTS PASSED");
    unlink(KEY_FIFO); unlink(CTRL_FIFO);
    return fails != 0;
}
