#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <time.h>
static struct { char pad[0x200]; } fake_inst_mem;
static void *fake_inst_ptr = &fake_inst_mem;
static volatile int open_calls, start_calls;
static volatile double start_at = 1e18;
static double nowd(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int fake_is_running(void *thr){ return nowd() >= start_at; }
static int fake_open(void *inst,int n){ open_calls++; usleep(50000); start_at = nowd()+0.3; return 0; }
static void fake_start(void *thr,int prio){ start_calls++; }
#define PANEL_PEER_INSTANCE_GLOBAL ((unsigned long)&fake_inst_ptr)
#define FN_THREAD_IS_RUNNING ((unsigned long)&fake_is_running)
#define FN_THREAD_START ((unsigned long)&fake_start)
#define FN_OPEN_DEVICE ((unsigned long)&fake_open)
#include <unistd.h>
#include "keyshim.c"
#include <pthread.h>
static void *hammer(void *a){ for(int i=0;i<2000;i++) ui_pump_ensure(); return NULL; }
int main(void){
    int fails=0;
    shim_ok = 1;                                   /* skip K1 (tested separately) */
    *(int *)((char *)&fake_inst_mem + PANEL_PEER_NUM_DEVICES_OFF) = 1;
    pthread_t th[4];
    for(int i=0;i<4;i++) pthread_create(&th[i],NULL,hammer,NULL);
    for(int i=0;i<4;i++) pthread_join(th[i],NULL);
    printf("4 threads x 2000 ensure() calls -> openDevice calls = %d (want 1), pump running = %d\n", open_calls, fake_is_running(0));
    if (open_calls != 1 || !fake_is_running(0)) { puts("FAIL"); fails++; }
    /* pump dies later: a retry is allowed, but only once per episode and not in <5 s */
    start_at = 1e18; pump_opened_at = 0;
    int before = open_calls;
    for(int i=0;i<1000;i++) ui_pump_ensure();
    printf("pump died: 1000 rapid calls -> %d new openDevice call(s) (want 1)\n", open_calls-before);
    if (open_calls-before != 1) { puts("FAIL"); fails++; }
    puts(fails?"SOME FAILED":"PUMP TESTS PASSED");
    return fails;
}
