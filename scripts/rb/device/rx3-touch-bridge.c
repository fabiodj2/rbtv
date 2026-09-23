#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include "pi-controls.h"
struct __attribute__((packed)) report {uint8_t down,pad;uint16_t x,y;};
struct finger {int x,y,down,active,region;long next_repeat;};
static int control;
static volatile sig_atomic_t running=1;
static void stop(int sig){(void)sig;running=0;}
static struct ui_state *state;
static long millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000+t.tv_nsec/1000000;}
static void command(int key,int op,int ch,int value,float a){struct command c={key,op,ch,value,a,0};if(write(control,&c,sizeof(c))!=sizeof(c))perror("control");}
static void button(int i,int down){
 const struct button*b=&buttons[i];if(down)state->pressed|=1u<<i;else state->pressed&=~(1u<<i);
 if(b->scroll){if(down)command(b->key,4,0,b->scroll,0);}else command(b->key,down?0:2,b->channel,0,0);
}
/* Orange Pi 4 LTS port: working canvas is 1920x1080 landscape (was
 * 1920x1200, reached by rotating the raw touch axes 90 degrees for a
 * portrait panel). ASSUMPTION, NOT VERIFIED ON REAL HARDWARE: this
 * assumes the Orange Pi's touch controller reports X along the panel's
 * physical width and Y along its height, matching the landscape
 * orientation directly (no swap needed) - true for most touch
 * controllers sold as landscape units, but not guaranteed. If touch
 * ends up rotated or mirrored on the real screen, swap x<->y and/or
 * flip the "ax.maximum-...-x" subtraction below; see
 * DISPLAY-PORT-NOTES.md. */
static void release_inputs(struct finger *fingers,int out,int native_held,int ux,int uy){
 for(int i=0;i<10;i++)if(fingers[i].active&&fingers[i].region>0&&fingers[i].region<=12)button(fingers[i].region-1,0);
 if(native_held){
  struct report r={0,0,37+(1280-ux)*3976/1280,72+uy*3856/800};
  for(int i=0;i<10;i++){if(write(out,&r,sizeof(r))!=sizeof(r))perror("touch release");usleep(10000);}
 }
 memset(fingers,0,10*sizeof(*fingers));
}
static void retry_pause(void){for(int i=0;i<20&&running;i++)usleep(50000);}
int main(int argc,char**argv){
 if(argc<3)return 2;
 int fullscreen=0,exclusive=0,swap_xy=0,invert_x=0,invert_y=0;
 for(int i=3;i<argc;i++){
  if(!strcmp(argv[i],"--fullscreen"))fullscreen=1;
  else if(!strcmp(argv[i],"--exclusive"))exclusive=1;
  else if(!strcmp(argv[i],"--swap-xy"))swap_xy=1;
  else if(!strcmp(argv[i],"--invert-x"))invert_x=1;
  else if(!strcmp(argv[i],"--invert-y"))invert_y=1;
  else return 2;
 }
 int replay=!strcmp(argv[1],"--replay");int in=-1,out=open(argv[2],O_RDWR|O_NONBLOCK);control=open(ui_control_path(),O_RDWR|O_NONBLOCK);
 if(out<0||control<0){perror("open");return 1;}
 int sf=open(ui_state_path(),O_RDWR|O_CREAT,0600);if(sf<0||ftruncate(sf,sizeof(struct ui_state)))return 1;
 state=mmap(0,sizeof(*state),PROT_READ|PROT_WRITE,MAP_SHARED,sf,0);if(state==MAP_FAILED)return 1;
 if(state->magic!=0x52583332)*state=(struct ui_state){0x52583332,{1,.6,0,1,.5,.5},0,1};
 struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);sigaction(SIGTERM,&action,0);sigaction(SIGINT,&action,0);
 int waiting=0;
 while(running){
 in=replay?0:open(argv[1],O_RDONLY|O_NONBLOCK);
 if(in<0){if(!waiting)perror("waiting for touch device");waiting=1;retry_pause();continue;}
 struct input_absinfo ax={.maximum=1919},ay={.maximum=1079};
 if(!replay&&(ioctl(in,EVIOCGABS(ABS_MT_POSITION_X),&ax)||ioctl(in,EVIOCGABS(ABS_MT_POSITION_Y),&ay))){if(!waiting)perror("touch ranges");waiting=1;close(in);retry_pause();continue;}
 if(exclusive&&!replay&&ioctl(in,EVIOCGRAB,1)){if(!waiting)perror("exclusive touch input");waiting=1;close(in);retry_pause();continue;}
 waiting=0;fprintf(stderr,"touch input connected: %s\n",argv[1]);
 struct finger fingers[10]={0};int slot=0,source=-1,ux=0,uy=0,release=0;int dropped=0;struct input_event e;struct pollfd p={in,POLLIN,0};
 while(running){
 int ready=poll(&p,1,10);if(ready<0){if(errno==EINTR)continue;perror("touch poll");break;}
 if(ready){
  ssize_t n=read(in,&e,sizeof(e));if(n<0&&(errno==EINTR||errno==EAGAIN))continue;if(n!=sizeof(e))break;
  if(e.type==EV_SYN&&e.code==SYN_DROPPED){
   release_inputs(fingers,out,source>=0||release,ux,uy);source=-1;release=0;slot=0;dropped=1;
   fprintf(stderr,"touch events dropped; gestures released, waiting for fresh contact\n");continue;
  }
  if(dropped){if(e.type==EV_SYN&&e.code==SYN_REPORT)dropped=0;continue;}
  if(e.type==EV_ABS){if(e.code==ABS_MT_SLOT)slot=e.value;if(slot>=0&&slot<10){struct finger*f=&fingers[slot];if(e.code==ABS_MT_POSITION_X)f->x=e.value;if(e.code==ABS_MT_POSITION_Y)f->y=e.value;if(e.code==ABS_MT_TRACKING_ID)f->down=e.value>=0;}}
  if(e.type!=EV_SYN||e.code!=SYN_REPORT)continue;
 }
 long now=millis();
 for(int i=0;i<10;i++){
  struct finger*f=&fingers[i];
  int nx=(f->x-ax.minimum)*65535/(ax.maximum-ax.minimum+1);
  int ny=(f->y-ay.minimum)*65535/(ay.maximum-ay.minimum+1);
  if(nx<0)nx=0;if(nx>65535)nx=65535;if(ny<0)ny=0;if(ny>65535)ny=65535;
  if(swap_xy){int n=nx;nx=ny;ny=n;}
  if(invert_x)nx=65535-nx;if(invert_y)ny=65535-ny;
  int lx=nx*1920/65536,ly=ny*1080/65536;
  if(lx<0)lx=0;if(lx>1919)lx=1919;if(ly<0)ly=0;if(ly>1079)ly=1079;
  if(f->down&&!f->active){f->active=1;f->region=-1;
   if(fullscreen){if(source<0){source=i;f->region=0;}}
   else if(ly>=880){f->region=1+(ly-880)/100*6+lx/320;button(f->region-1,1);f->next_repeat=now+400;}
   else if(lx<256||lx>=1664){int si=(lx<256?0:3)+ly/293;if(si>5)si=5;
    if((si==0||si==3)&&ly%293>=40&&ly%293<80){int ch=si==0?1:2;command(0x5020,0,ch,0,0);command(0x5020,2,ch,0,0);state->headphone_cue^=ch==1?1:2;}
    else f->region=20+si;
   }else if(source<0){source=i;f->region=0;}
   fprintf(stderr,"touch begin slot=%d screen=%d,%d region=%d\n",i,lx,ly,f->region);
  }
  if(f->active&&f->down){
   if(f->region>=20&&ready){int si=f->region-20;float a=(265-(ly-(si%3)*293))/180.f;if(a<0)a=0;if(a>1)a=1;state->level[si]=a;command(slider_keys[si],4,slider_channels[si],0,a);}
   else if(f->region>0&&f->region<=12&&buttons[f->region-1].scroll&&now>=f->next_repeat){button(f->region-1,1);f->next_repeat=now+120;}
   else if(f->region==0){
   /* Fullscreen inverse of frame-scale.h's 27/20 pillarbox scale (was
    * 3/2 for the portrait rotate+scale): subtract the 96px bar offset
    * first, then invert. Debug/non-fullscreen inverse of the 11/10
    * preview scale (was 4/5): subtract the 256px preview offset first. */
   ux=fullscreen?(lx-96)*20/27:(lx-256)*10/11;
   uy=fullscreen?ly*20/27:ly*10/11;
   if(ux<0)ux=0;if(ux>1279)ux=1279;if(uy<0)uy=0;if(uy>799)uy=799;}
  }
  if(f->active&&!f->down){if(f->region>0&&f->region<=12)button(f->region-1,0);if(source==i){source=-1;release=10;}f->active=0;}
 }
 if(source>=0||release){struct report r={source>=0,0,37+(1280-ux)*3976/1280,72+uy*3856/800};if(write(out,&r,sizeof(r))!=sizeof(r))perror("touch report");if(source<0)release--;}
 }
 release_inputs(fingers,out,source>=0||release,ux,uy);
 if(!replay)close(in);
 if(replay)break;
 if(running){fprintf(stderr,"touch input lost; reconnecting\n");retry_pause();}
 }
 munmap(state,sizeof(*state));close(sf);close(out);close(control);
 return 0;
}
