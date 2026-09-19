#ifndef RX3_FRAME_SCALE_H
#define RX3_FRAME_SCALE_H
#include <stdint.h>
#include <string.h>
/* Orange Pi 4 LTS port: 1920x1080 LANDSCAPE monitor, no rotation needed
 * (the original was 1200x1920 PORTRAIT, rotated 90 from the native image).
 *
 * Native RX3 canvas is 1280x800. 1080/800 = 1728/1280 = 27/20 exactly, so
 * this is still a pixel-exact nearest-neighbour scale, just like the
 * original 1280x800 -> 1920x1200 (3/2) scale was - only the ratio changed
 * (27/20 = 1.35x instead of 3/2 = 1.5x) and rotation was removed. Unlike
 * the original, this does one integer division per output pixel instead
 * of exploiting a period-3 pattern - 27/20 doesn't reduce to a convenient
 * small block, and 1728x1080 (< 2M pixels) is cheap enough per frame that
 * the extra divisions are not worth the added complexity/bug risk.
 *
 * PILLARBOX, not crop: 1280*1.35=1728w x 800*1.35=1080h. That is 192px
 * narrower than the 1920px panel, so there are (1920-1728)/2=96px black
 * bars on each side. Chosen over cropping so no UI element from the
 * native 1280x800 image is ever cut off - see DISPLAY-PORT-NOTES.md for
 * the crop alternative if bezel-to-bezel image matters more than that. */
#define RX3_PANEL_W 1920
#define RX3_PANEL_H 1080
#define RX3_SCALED_W 1728   /* 1280 * 27/20 */
#define RX3_SCALED_H 1080   /*  800 * 27/20 */
#define RX3_BAR_W ((RX3_PANEL_W-RX3_SCALED_W)/2)  /* 96 */
static void rx3_fullscreen_present(unsigned char *dst,unsigned pitch,
 const uint32_t *src,uint32_t *scratch){
 (void)scratch; /* no rotation pass needed on a landscape panel */
 /* Both DRM scanout buffers need initialized bars. Painting the narrow
  * strips per frame also keeps fbdev/DRM reconnects deterministic. */
 for(int y=0;y<RX3_PANEL_H;y++){
  uint32_t *out=(uint32_t*)(dst+y*pitch);
  memset(out,0,RX3_BAR_W*4);
  memset(out+RX3_PANEL_W-RX3_BAR_W,0,RX3_BAR_W*4);
 }
 uint32_t row[RX3_SCALED_W];int previous=-1;
 for(int y=0;y<RX3_SCALED_H;y++){
  int sy=y*20/27;
  if(sy!=previous){
   const uint32_t *in=src+sy*1280;
   for(int x=0;x<RX3_SCALED_W;x++)row[x]=in[x*20/27];
   previous=sy;
  }
  memcpy(dst+y*pitch+RX3_BAR_W*4,row,sizeof(row));
 }
}
#endif
