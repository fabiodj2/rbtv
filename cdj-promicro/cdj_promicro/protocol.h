// Protocolo SPI do painel CDJ-1000 MK3 (quadro de 27 bytes, full-duplex).
// Fonte: reference/protocol_full.txt de fabiodj2/cdj1000mk3-spi-bridge.
// C puro, sem dependencia de Arduino (testavel no PC).
#ifndef CDJ_PROTOCOL_H
#define CDJ_PROTOCOL_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define CDJ_FRAME 27
#define CDJ_NPKG  8

// ---- Painel -> nos (Rbuffer) ----
#define RX_TOUCH_BRAKE 2
#define RX_REL_START   3
#define RX_PITCH_MSB   4   // 14 bits: [msb][lsb<<2]
#define RX_PITCH_LSB   5
#define RX_JOG_PLS_MSB 8   // contador 16 bits, 3600 pulsos/volta
#define RX_JOG_PLS_LSB 9
#define RX_SWITCHES    12
#define RX_BTN_A       14
#define RX_BTN_B       16
#define RX_BTN_C       17
#define RX_BTN_D       18

#define SW_EJECT_UNLOCK (1<<0)
#define SW_FORWARD      (1<<1)   // 1 = frente, 0 = reverso
#define SW_TOUCH_MASK   0x30     // bits 4/5: toque no jog (confirmar no hardware)

// byte 14
#define B_PLAY (1<<0)
#define B_CUE  (1<<1)
#define B_LIN  (1<<2)
#define B_LOUT (1<<3)
#define B_RELOOP (1<<4)
#define B_HCA  (1<<5)
#define B_HCB  (1<<6)
#define B_HCC  (1<<7)
// byte 16
#define B_REC  (1<<0)
#define B_TRK_REV (1<<1)
#define B_TRK_FWD (1<<2)
#define B_SRC_REV (1<<3)
#define B_SRC_FWD (1<<4)
// byte 17
#define B_FLD_REV (1<<0)
#define B_FLD_FWD (1<<1)
#define B_JOGMODE (1<<2)
#define B_TEMPO   (1<<3)
#define B_MT      (1<<4)
#define B_TRESET  (1<<5)
// byte 18
#define B_CALL_FWD (1<<0)
#define B_CALL_REV (1<<1)
#define B_MEMORY   (1<<2)
#define B_DELETE   (1<<3)
#define B_EJECT    (1<<4)
#define B_TIMEMODE (1<<5)
#define B_TEXTMODE (1<<6)

// ---- Nos -> painel (Tbuffer) ----
#define TX_LED0   17   // play,cue,loopin,loopout,reloop,reverse,hotA red/green
#define TX_LED1   18
#define TX_LED2   19
#define TX_CUEPOS 21
#define TX_JOGPOS 25
#define TX_CRC    26
#define PKG7      7

#define L0_PLAY (1<<0)
#define L0_CUE  (1<<1)
#define L0_LIN  (1<<2)
#define L0_LOUT (1<<3)
#define L0_RELOOP (1<<4)
#define L0_REVERSE (1<<5)
#define L0_HCA_R (1<<6)
#define L0_HCA_G (1<<7)
#define L1_HCA_O (1<<0)
#define L1_HCB_R (1<<1)
#define L1_HCB_G (1<<2)
#define L1_HCB_O (1<<3)
#define L1_HCC_R (1<<4)
#define L1_HCC_G (1<<5)
#define L1_HCC_O (1<<6)
#define L2_MT     (1<<3)
#define L2_TRESET (1<<4)
#define L2_VINYL  (1<<5)
#define L2_CDJ    (1<<6)
#define L2_SD     (1<<7)

#define JOG_MIN 1
#define JOG_MAX 135
#define JOG_PULSES_PER_REV 3600

// CRC do Tbuffer = (byte1 + ... + byte25) % 256
static inline uint8_t cdj_tx_crc(const uint8_t *f) {
  uint8_t s = 0;
  for (uint8_t i = 1; i <= 25; i++) s += f[i];
  return s;
}

// Quadro valido: cabecalho 0x01 0x10 e assinatura fixa F1 64 76 nos bytes 20..22.
static inline bool cdj_rx_valid(const uint8_t *f) {
  return f[0] == 0x01 && f[1] == 0x10 &&
         f[20] == 0xF1 && f[21] == 0x64 && f[22] == 0x76;
}

static inline uint16_t cdj_rx_pitch14(const uint8_t *f) {
  return ((uint16_t)f[RX_PITCH_MSB] << 6) | (f[RX_PITCH_LSB] >> 2);
}
static inline uint16_t cdj_rx_jog_counter(const uint8_t *f) {
  return ((uint16_t)f[RX_JOG_PLS_MSB] << 8) | f[RX_JOG_PLS_LSB];
}

// Pacote 7 (LEDs / jog / display) - template do projeto de referencia.
static inline void cdj_tx_init(uint8_t pkg[CDJ_NPKG][CDJ_FRAME]) {
  static const uint8_t hdr[CDJ_NPKG] = {0, 24, 48, 72, 96, 120, 144, 168};
  static const uint8_t p7[CDJ_FRAME] = {
    168, 119, 119, 0, 119, 119, 0, 0, 0, 1,
    176, 0, 0, 0, 0, 0, 0, 0x0C, 0, 0x20,
    0, 0, 0, 88, 0, 1, 0 };
  for (uint8_t p = 0; p < CDJ_NPKG; p++) {
    memset(pkg[p], 0, CDJ_FRAME);
    pkg[p][0] = hdr[p];
    pkg[p][TX_JOGPOS] = JOG_MIN;
  }
  memcpy(pkg[PKG7], p7, CDJ_FRAME);
  for (uint8_t p = 0; p < CDJ_NPKG; p++) pkg[p][TX_CRC] = cdj_tx_crc(pkg[p]);
}

#endif
