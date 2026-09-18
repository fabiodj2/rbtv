#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>

/* Keep this probe buildable against the firmware libraries without requiring
 * host libasound development headers. These are stable public ALSA ABI types. */
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
extern int snd_pcm_open(snd_pcm_t **,const char *,int,int);
extern int snd_pcm_close(snd_pcm_t *);
extern int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **);
extern void snd_pcm_hw_params_free(snd_pcm_hw_params_t *);
extern int snd_pcm_hw_params_any(snd_pcm_t *,snd_pcm_hw_params_t *);
extern int snd_pcm_hw_params_test_channels(snd_pcm_t *,snd_pcm_hw_params_t *,unsigned);
extern int snd_pcm_hw_params_test_rate(snd_pcm_t *,snd_pcm_hw_params_t *,unsigned,int);
extern int snd_pcm_hw_params_test_format(snd_pcm_t *,snd_pcm_hw_params_t *,int);

static int failures;
static void passed(const char *name){printf("PASS %s\n",name);}
static void failed(const char *name,int code){printf("FAIL %s: %s (%d)\n",name,strerror(code),code);failures++;}

static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static int worker_ran;
static void *worker(void *unused){
 (void)unused;
 pthread_mutex_lock(&mutex);worker_ran=1;pthread_mutex_unlock(&mutex);return 0;
}

static void test_threads(void){
 pthread_t thread;pthread_mutex_lock(&mutex);
 int rc=pthread_create(&thread,0,worker,0);
 if(rc){pthread_mutex_unlock(&mutex);failed("pthread-create",rc);return;}
 usleep(20000);pthread_mutex_unlock(&mutex);rc=pthread_join(thread,0);
 if(rc||!worker_ran)failed("pthread-futex",rc?rc:EIO);else passed("pthread-futex");
}

static void test_realtime(void){
 struct sched_param parameter={.sched_priority=1};
 if(sched_setscheduler(0,SCHED_RR,&parameter))failed("sched-rr",errno);
 else {
  passed("sched-rr");
  parameter.sched_priority=0;sched_setscheduler(0,SCHED_OTHER,&parameter);
 }
}

static void test_mlock(void){
 long page=sysconf(_SC_PAGESIZE);if(page<4096)page=4096;
 void *memory=mmap(0,(size_t)page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(memory==MAP_FAILED){failed("mmap",errno);return;}
 memset(memory,0x5a,(size_t)page);
 if(mlock(memory,(size_t)page))failed("mlock",errno);else {passed("mlock");munlock(memory,(size_t)page);}
 munmap(memory,(size_t)page);
}

static void test_mqueue(void){
 char name[64],message[]="rx3-arm32";snprintf(name,sizeof(name),"/rx3-probe-%ld",(long)getpid());
 struct mq_attr attr={.mq_maxmsg=2,.mq_msgsize=32};
 mqd_t queue=mq_open(name,O_CREAT|O_EXCL|O_RDWR,0600,&attr);
 if(queue==(mqd_t)-1){failed("posix-mqueue",errno);return;}
 char received[32]={0};unsigned priority=0;
 int ok=!mq_send(queue,message,sizeof(message),7)&&
        mq_receive(queue,received,sizeof(received),&priority)==(ssize_t)sizeof(message)&&
        !memcmp(message,received,sizeof(message))&&priority==7;
 if(ok)passed("posix-mqueue");else failed("posix-mqueue",errno?errno:EIO);
 mq_close(queue);mq_unlink(name);
}

static void test_alsa(const char *card){
 char device[64];snprintf(device,sizeof(device),"hw:CARD=%s,DEV=0",card);
 snd_pcm_t *pcm=0;snd_pcm_hw_params_t *params=0;
 int rc=snd_pcm_open(&pcm,device,0,1);
 if(rc<0){failed("alsa-open",-rc);return;}
 rc=snd_pcm_hw_params_malloc(&params);
 if(rc>=0)rc=snd_pcm_hw_params_any(pcm,params);
 if(rc>=0)rc=snd_pcm_hw_params_test_channels(pcm,params,4);
 if(rc>=0)rc=snd_pcm_hw_params_test_rate(pcm,params,44100,0);
 if(rc>=0)rc=snd_pcm_hw_params_test_format(pcm,params,2);
 if(rc<0)failed("alsa-4ch-44100-s16",-rc);else passed("alsa-4ch-44100-s16");
 if(params)snd_pcm_hw_params_free(params);
 snd_pcm_close(pcm);
}

int main(int argc,char **argv){
 struct utsname system;
 printf("RX3 ARM32 compatibility probe\n");
 if(sizeof(void*)!=4){printf("FAIL arm32-pointer-size: %u\n",(unsigned)sizeof(void*));failures++;}
 else passed("arm32-pointer-size");
 if(!uname(&system))printf("INFO kernel=%s machine=%s page=%ld uid=%ld gid=%ld\n",
  system.release,system.machine,sysconf(_SC_PAGESIZE),(long)getuid(),(long)getgid());
 test_threads();test_realtime();test_mlock();test_mqueue();
 test_alsa(argc>1?argv[1]:"DDJ400");
 printf("RESULT %s (%d failure%s)\n",failures?"FAIL":"PASS",failures,failures==1?"":"s");
 return failures?1:0;
}
