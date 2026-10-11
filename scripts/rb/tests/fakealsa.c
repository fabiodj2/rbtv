#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int dummy;
int fake_mode = 0;           /* 0 ok, 1 partial, 2 epipe-once, 3 permanent ENODEV, 4 slow(100ms) */
static int epipe_pending;
unsigned long fake_frames, fake_calls, fake_recover_calls, fake_close_calls;
volatile int fake_dropped;
int32_t fake_last[8];
void fake_set_mode(int m){ fake_mode=m; epipe_pending=(m==2); }
int snd_pcm_open(void **p,const char*n,int s,int m){ *p=&dummy; return 0; }
int snd_pcm_close(void *p){ fake_close_calls++; return 0; }
int snd_pcm_prepare(void *p){ return 0; }
int snd_pcm_drop(void *p){ fake_dropped=1; return 0; }
int snd_pcm_recover(void *p,int err,int silent){ fake_recover_calls++; return (err==-EPIPE)?0:err; }
long snd_pcm_writei(void *p,const void*b,unsigned long n){
    fake_calls++;
    switch(fake_mode){
    case 1: { unsigned long k = n>1? n/2 : n; fake_frames+=k; memcpy(fake_last,b,sizeof fake_last); return k; }
    case 2: if (epipe_pending){ epipe_pending=0; return -EPIPE; } break;
    case 3: return -ENODEV;
    case 4: { for(int i=0;i<100 && !fake_dropped;i++) usleep(1000); break; }
    }
    fake_frames+=n; memcpy(fake_last,b,sizeof fake_last); return n;
}
