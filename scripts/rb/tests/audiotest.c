#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/stat.h>
int snd_pcm_open(void **, const char *, int, int);
int snd_pcm_close(void *);
long snd_pcm_writei(void *, const void *, unsigned long);
int snd_pcm_hw_params_set_access(void *, void *, int);
static void (*set_mode)(int);
static unsigned long *frames, *recov, *closes; static volatile int *dropped;
static int fails;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n",msg); fails++; } else printf("ok:   %s\n",msg);}while(0)
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int32_t buf[240*2];
static void *writer(void *a){ void *m=a; snd_pcm_writei(m,buf,240); return NULL; }
int main(void){
    void *m=NULL,*hp=NULL;
    snd_pcm_open(&m,"x",0,0);   /* master (also triggers dlopen of fake libasound) */
    snd_pcm_open(&hp,"y",0,0);
    set_mode=dlsym(RTLD_DEFAULT,"fake_set_mode"); frames=dlsym(RTLD_DEFAULT,"fake_frames");
    recov=dlsym(RTLD_DEFAULT,"fake_recover_calls"); closes=dlsym(RTLD_DEFAULT,"fake_close_calls");
    dropped=dlsym(RTLD_DEFAULT,"fake_dropped");
    int32_t *last=dlsym(RTLD_DEFAULT,"fake_last");
    if(!set_mode||!frames){ puts("fake lib not found"); return 2; }
    for(int i=0;i<480;i++) buf[i]=100+i;

    set_mode(0); *frames=0;
    long r=snd_pcm_writei(m,buf,240);
    CHECK(r==240 && *frames==240,"full write: returns 240, hw got 240");
    CHECK(last[0]==100 && last[1]==101,"master samples pass through unmodified (mute disabled)");

    set_mode(1); *frames=0;
    r=snd_pcm_writei(m,buf,240);
    CHECK(r==240 && *frames==240,"partial writes are looped until all 240 frames are written");

    set_mode(2); *frames=0; *recov=0;
    r=snd_pcm_writei(m,buf,240);
    CHECK(r==240 && *frames==240 && *recov==1,"EPIPE -> snd_pcm_recover once, then the data is written");

    set_mode(3); *frames=0;
    double t0=now(); r=snd_pcm_writei(m,buf,240); double dt=now()-t0;
    CHECK(r==240 && *frames==0,"dead sink: returns size, nothing reached the hw");
    CHECK(dt>=0.004 && dt<0.5,"dead sink: call is PACED (took ~one period, not instant)");
    printf("      dead-sink call took %.1f ms\n",dt*1000);

    /* close while a writer is blocked inside the real write */
    set_mode(4); *dropped=0; *frames=0;
    pthread_t th; pthread_create(&th,NULL,writer,m); usleep(20000);
    t0=now(); snd_pcm_close(m); dt=now()-t0; pthread_join(th,NULL);
    CHECK(*closes==1,"close: real pcm closed exactly once after the writer left");
    CHECK(*dropped==1,"close: snd_pcm_drop used to unblock the writer");
    printf("      close took %.1f ms\n",dt*1000);

    /* log hygiene: flood, then check cap + rotation */
    for(int i=0;i<30000;i++){ snd_pcm_hw_params_set_access(hp,NULL,3); if(i%50==0) usleep(2000); }
    sleep(1);
    struct stat st; int e=stat("/tmp/audioshim.log",&st);
    CHECK(e==0 && st.st_size<=16*1024,"log exists and stays <= cap (+1 flush)");
    printf("      log size %ld bytes\n",(long)st.st_size);
    CHECK(stat("/tmp/audioshim.log.1",&st)==0,"log rotated to .1");
    printf("%s\n",fails?"SOME TESTS FAILED":"ALL PASSED");
    return fails!=0;
}
