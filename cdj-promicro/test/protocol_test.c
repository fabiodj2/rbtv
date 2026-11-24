#include <assert.h>
#include <stdio.h>
#include "../cdj_promicro/protocol.h"
int main(void) {
  uint8_t pkg[CDJ_NPKG][CDJ_FRAME];
  cdj_tx_init(pkg);
  for (int p = 0; p < CDJ_NPKG; p++) assert(pkg[p][TX_CRC] == cdj_tx_crc(pkg[p]));
  assert(pkg[0][0]==0 && pkg[1][0]==24 && pkg[7][0]==168);
  uint8_t f[CDJ_FRAME] = {0};
  assert(!cdj_rx_valid(f));
  f[0]=1; f[1]=0x10; f[20]=0xF1; f[21]=0x64; f[22]=0x76;
  assert(cdj_rx_valid(f));
  f[RX_PITCH_MSB]=0xFF; f[RX_PITCH_LSB]=0xFC; assert(cdj_rx_pitch14(f)==16383);
  f[RX_JOG_PLS_MSB]=0x12; f[RX_JOG_PLS_LSB]=0x34; assert(cdj_rx_jog_counter(f)==0x1234);
  pkg[7][TX_LED0] |= L0_PLAY; assert(pkg[7][TX_CRC] != cdj_tx_crc(pkg[7]));
  puts("ok"); return 0;
}
