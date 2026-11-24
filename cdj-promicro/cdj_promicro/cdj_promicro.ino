/*
 * CDJ-1000 MK3 painel -> USB MIDI, Arduino Pro Micro 5V (ATmega32U4, 16 MHz)
 *
 * Biblioteca necessaria: "MIDIUSB" (Arduino). Placa: Arduino Leonardo / Pro Micro 5V.
 *
 * LIGACAO (painel e SPI MASTER; a Pro Micro e SLAVE, modo 3, ~1 MHz, 5V direto):
 *   CDJ pino 13 SCLK -> D15 (SCK)
 *   CDJ pino 14 SOUT -> D14 (MISO)    \ A nomenclatura SIN/SOUT do projeto de referencia e
 *   CDJ pino 15 SIN  -> D16 (MOSI)    / ambigua: se nao chegar quadro valido, TROQUE esses dois.
 *   CDJ pino 16 XCS  -> PB0 = pad do LED RX da Pro Micro (SS do hardware + PCINT0)
 *   GND comum. O painel precisa de alimentacao propria (fonte do CDJ), nunca do USB.
 *
 * ATENCAO: no 32U4 o SS do SPI em hardware e o PB0, que nao tem pino no header.
 * Por isso o XCS vai no pad do LED RX (solde um fio ali). O D10 NAO serve.
 */
#include <Arduino.h>
#include <MIDIUSB.h>
#include "protocol.h"
#include "midimap.h"

#define MIDI_CH        1      // Deck 1 = canal 1
#define DEBUG_SERIAL   1      // estatisticas e dump via Serial (USB CDC)
#define JOG_DIV        4      // pulsos do jog por passo de CC
#define PITCH_HYST     16     // histerese do pitch (14 bits)
#define KNOB_HYST      2

// ----------------------------------------------------------------------------
// SPI slave: so move bytes. Sem copias de quadro dentro do ISR por byte.
// ----------------------------------------------------------------------------
static volatile uint8_t rxBuf[2][CDJ_FRAME];
static volatile uint8_t rxWr = 0;           // buffer sendo escrito pelo ISR
static volatile uint8_t rxReady = 0;        // indice do ultimo quadro completo
static volatile uint16_t frameSeq = 0;      // incrementa a cada quadro completo
static volatile uint8_t byteIdx = 0;
static volatile uint8_t curPkg = 0;
static volatile uint16_t badFrames = 0;     // XCS subiu antes de 27 bytes
static uint8_t txShadow[CDJ_NPKG][CDJ_FRAME];  // editado no loop()
static uint8_t txActive[CDJ_FRAME];            // lido pelo ISR

ISR(SPI_STC_vect) {
  uint8_t in = SPDR;
  uint8_t i = byteIdx;
  if (i < CDJ_FRAME) {
    rxBuf[rxWr][i] = in;
    i++;
    byteIdx = i;
    SPDR = (i < CDJ_FRAME) ? txActive[i] : 0;
  } else {
    SPDR = 0;
  }
}

// XCS (PB0): subida = fim do quadro; descida = inicio.
ISR(PCINT0_vect) {
  if (PINB & _BV(0)) {                       // XCS alto: fim do quadro
    if (byteIdx == CDJ_FRAME) {
      rxReady = rxWr; rxWr ^= 1; frameSeq++;
    } else {
      badFrames++;
    }
    byteIdx = 0;
    curPkg = (curPkg + 1) & 7;
    memcpy(txActive, txShadow[curPkg], CDJ_FRAME);   // troca no intervalo entre quadros
    SPDR = txActive[0];
  } else {                                   // XCS baixo: inicio
    byteIdx = 0;
    SPDR = txActive[0];
  }
}

static void spiSlaveBegin() {
  DDRB &= ~_BV(0); PORTB |= _BV(0);          // PB0 (XCS/SS) entrada com pull-up
  pinMode(15, INPUT);                        // SCK
  pinMode(16, INPUT);                        // MOSI
  pinMode(14, OUTPUT);                       // MISO
  memcpy(txActive, txShadow[0], CDJ_FRAME);
  SPDR = txActive[0];
  SPCR = _BV(SPE) | _BV(SPIE) | _BV(CPOL) | _BV(CPHA);   // slave, modo 3, MSB first
  PCMSK0 = _BV(PCINT0);
  PCIFR |= _BV(PCIF0);
  PCICR |= _BV(PCIE0);
}

// ----------------------------------------------------------------------------
// MIDI
// ----------------------------------------------------------------------------
static void midiNote(uint8_t n, bool on) {
  uint8_t st = (on ? 0x90 : 0x80) | (MIDI_CH - 1);
  midiEventPacket_t e = { (uint8_t)(st >> 4), st, n, (uint8_t)(on ? 127 : 0) };
  MidiUSB.sendMIDI(e);
}
static void midiCC(uint8_t cc, uint8_t v) {
  uint8_t st = 0xB0 | (MIDI_CH - 1);
  midiEventPacket_t e = { 0x0B, st, cc, v };
  MidiUSB.sendMIDI(e);
}

struct BtnMap { uint8_t idx; uint8_t mask; uint8_t note; };
static const BtnMap BTN[] = {
  {RX_BTN_A, B_PLAY,   N_PLAY_PAUSE}, {RX_BTN_A, B_CUE,    N_CUE},
  {RX_BTN_A, B_LIN,    N_LOOP_IN},    {RX_BTN_A, B_LOUT,   N_LOOP_OUT},
  {RX_BTN_A, B_RELOOP, N_RELOOP_EXIT},{RX_BTN_A, B_HCA,    N_HOTCUE_A},
  {RX_BTN_A, B_HCB,    N_HOTCUE_B},   {RX_BTN_A, B_HCC,    N_HOTCUE_C},
  {RX_BTN_B, B_REC,    N_REC_MODE},   {RX_BTN_B, B_TRK_REV,N_TRACK_REV},
  {RX_BTN_B, B_TRK_FWD,N_TRACK_FWD},  {RX_BTN_B, B_SRC_REV,N_SEARCH_REV},
  {RX_BTN_B, B_SRC_FWD,N_SEARCH_FWD},
  {RX_BTN_C, B_FLD_REV,N_FOLDER_REV}, {RX_BTN_C, B_FLD_FWD,N_FOLDER_FWD},
  {RX_BTN_C, B_JOGMODE,N_SLIP_MODE},  {RX_BTN_C, B_TEMPO, N_TEMPO_RANGE},
  {RX_BTN_C, B_MT,     N_MASTER_TEMPO},{RX_BTN_C, B_TRESET,N_TEMPO_RESET},
  {RX_BTN_D, B_CALL_FWD,N_CALL_FWD},  {RX_BTN_D, B_CALL_REV,N_CALL_REV},
  {RX_BTN_D, B_MEMORY, N_MEMORY},     {RX_BTN_D, B_DELETE, N_DELETE},
  {RX_BTN_D, B_EJECT,  N_EJECT},      {RX_BTN_D, B_TIMEMODE,N_TIME_MODE},
  {RX_BTN_D, B_TEXTMODE,N_TEXT_MODE},
};
#define NBTN (sizeof(BTN)/sizeof(BTN[0]))

static uint8_t frame[CDJ_FRAME], prev[CDJ_FRAME];
static bool havePrev = false;
static uint16_t lastSeq = 0, lastPitch = 0, lastJog = 0;
static uint8_t lastBrake = 0, lastRel = 0;
static int32_t jogAcc = 0;                  // posicao do cursor em pulsos
static uint16_t validFrames = 0, invalidFrames = 0;
static bool ledDirty = true;

static void ledSet(uint8_t byteIdx_, uint8_t mask, bool on) {
  if (on) txShadow[PKG7][byteIdx_] |= mask; else txShadow[PKG7][byteIdx_] &= ~mask;
  ledDirty = true;
}
static void setJogRing(uint8_t pos) {
  for (uint8_t p = 0; p < CDJ_NPKG; p++) txShadow[p][TX_JOGPOS] = pos;
  ledDirty = true;
}
static void txCommit() {
  for (uint8_t p = 0; p < CDJ_NPKG; p++) txShadow[p][TX_CRC] = cdj_tx_crc(txShadow[p]);
  ledDirty = false;
}

static void handleFrame() {
  uint8_t sw = frame[RX_SWITCHES];
  if (!havePrev) {
    memcpy(prev, frame, CDJ_FRAME);
    lastPitch = cdj_rx_pitch14(frame); lastJog = cdj_rx_jog_counter(frame);
    lastBrake = frame[RX_TOUCH_BRAKE]; lastRel = frame[RX_REL_START];
    havePrev = true;
    return;
  }
  // Botoes (por borda)
  for (uint8_t i = 0; i < NBTN; i++) {
    bool now = frame[BTN[i].idx] & BTN[i].mask;
    bool was = prev[BTN[i].idx] & BTN[i].mask;
    if (now != was) midiNote(BTN[i].note, now);
  }
  // Chave de direcao (reverse) e toque no jog
  bool rev = !(sw & SW_FORWARD), revWas = !(prev[RX_SWITCHES] & SW_FORWARD);
  if (rev != revWas) midiNote(N_REVERSE, rev);
  bool touch = sw & SW_TOUCH_MASK, touchWas = prev[RX_SWITCHES] & SW_TOUCH_MASK;
  if (touch != touchWas) midiNote(N_JOG_TOUCH, touch);

  // Jog: delta de 16 bits com wrap; CC relativo 64+delta
  uint16_t jc = cdj_rx_jog_counter(frame);
  int16_t d = (int16_t)(jc - lastJog);
  lastJog = jc;
  if (d != 0) {
    int16_t step = d / JOG_DIV;
    if (step == 0) step = (d > 0) ? 1 : -1;
    step = constrain(step, -63, 63);
    midiCC(touch ? CC_JOG_SCRATCH : CC_JOG_BEND, (uint8_t)(64 + step));
    // cursor do anel: 135 posicoes por volta de 3600 pulsos
    jogAcc += (int32_t)d * (JOG_MAX - JOG_MIN + 1);
    int32_t span = (int32_t)JOG_PULSES_PER_REV * (JOG_MAX - JOG_MIN + 1);
    jogAcc %= span; if (jogAcc < 0) jogAcc += span;
    setJogRing(JOG_MIN + (uint8_t)(jogAcc / JOG_PULSES_PER_REV));
  }
  // Pitch 14 bits -> CC0 (MSB) + CC32 (LSB)
  uint16_t p = cdj_rx_pitch14(frame);
  if (abs((int16_t)p - (int16_t)lastPitch) >= PITCH_HYST) {
    lastPitch = p;
    midiCC(CC_PITCH_MSB, (p >> 7) & 0x7F);
    midiCC(CC_PITCH_LSB, p & 0x7F);
  }
  // Knobs de freio / release (8 bits -> 7 bits)
  uint8_t b = frame[RX_TOUCH_BRAKE], r = frame[RX_REL_START];
  if (abs((int)b - (int)lastBrake) >= KNOB_HYST) { lastBrake = b; midiCC(CC_BRAKE, b >> 1); }
  if (abs((int)r - (int)lastRel)   >= KNOB_HYST) { lastRel = r;   midiCC(CC_RELEASE, r >> 1); }
  memcpy(prev, frame, CDJ_FRAME);
}

// MIDI de entrada: LEDs do painel. Velocity 127 verde, 64 laranja, 1..63 vermelho (hot cues).
static void hotCue(uint8_t which, uint8_t vel) {
  static const uint8_t r[3] = {L0_HCA_R, L1_HCB_R, L1_HCC_R};
  static const uint8_t g[3] = {L0_HCA_G, L1_HCB_G, L1_HCC_G};
  static const uint8_t o[3] = {L1_HCA_O, L1_HCB_O, L1_HCC_O};
  uint8_t bgi = (which == 0) ? TX_LED0 : TX_LED1;
  ledSet(bgi, r[which], false); ledSet(bgi, g[which], false);
  ledSet(TX_LED1, o[which], false);
  if (vel == 0) return;
  if (vel >= 100) ledSet(bgi, g[which], true);
  else if (vel >= 64) ledSet(TX_LED1, o[which], true);
  else ledSet(bgi, r[which], true);
}

static void handleMidiIn() {
  midiEventPacket_t m;
  while ((m = MidiUSB.read()).header != 0) {
    uint8_t type = m.byte1 & 0xF0, ch = (m.byte1 & 0x0F) + 1;
    if (ch != MIDI_CH || (type != 0x90 && type != 0x80)) continue;
    uint8_t n = m.byte2, v = (type == 0x90) ? m.byte3 : 0;
    bool on = v > 0;
    switch (n) {
      case N_PLAY_PAUSE:  ledSet(TX_LED0, L0_PLAY, on); break;
      case N_CUE:         ledSet(TX_LED0, L0_CUE, on); break;
      case N_LOOP_IN:     ledSet(TX_LED0, L0_LIN, on); break;
      case N_LOOP_OUT:    ledSet(TX_LED0, L0_LOUT, on); break;
      case N_RELOOP_EXIT: ledSet(TX_LED0, L0_RELOOP, on); break;
      case N_REVERSE:     ledSet(TX_LED0, L0_REVERSE, on); break;
      case N_HOTCUE_A:    hotCue(0, v); break;
      case N_HOTCUE_B:    hotCue(1, v); break;
      case N_HOTCUE_C:    hotCue(2, v); break;
      case N_MASTER_TEMPO:ledSet(TX_LED2, L2_MT, on); break;
      case N_TEMPO_RESET: ledSet(TX_LED2, L2_TRESET, on); break;
      case N_SLIP_MODE:   ledSet(TX_LED2, L2_VINYL, on); break;
      default: break;
    }
  }
}

void setup() {
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif
  cdj_tx_init(txShadow);
  ledSet(TX_LED2, L2_VINYL, true);               // estado inicial: vinyl aceso
  txCommit();
  spiSlaveBegin();
}

void loop() {
  DDRB &= ~_BV(0);                               // o core USB pode reconfigurar PB0 (LED RX)

  if (ledDirty) {
    // recalcula CRC no shadow; o ISR so copia entre quadros
    txCommit();
  }
  handleMidiIn();

  uint16_t seq; uint8_t idx;
  noInterrupts(); seq = frameSeq; idx = rxReady; interrupts();
  if (seq != lastSeq) {
    lastSeq = seq;
    noInterrupts();
    memcpy(frame, (const void*)rxBuf[idx], CDJ_FRAME);
    interrupts();
    if (cdj_rx_valid(frame)) { validFrames++; handleFrame(); } else { invalidFrames++; }
    MidiUSB.flush();
  }

#if DEBUG_SERIAL
  static uint32_t t0 = 0;
  if (millis() - t0 > 1000) {
    t0 = millis();
    Serial.print(F("frames ok=")); Serial.print(validFrames);
    Serial.print(F(" invalidos=")); Serial.print(invalidFrames);
    Serial.print(F(" curtos=")); Serial.println(badFrames);
  }
  if (Serial.available() && Serial.read() == 'd') {   // 'd' = dump do ultimo quadro
    for (uint8_t i = 0; i < CDJ_FRAME; i++) { if (frame[i] < 16) Serial.print('0'); Serial.print(frame[i], HEX); Serial.print(' '); }
    Serial.println();
  }
#endif
}
