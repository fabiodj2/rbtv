// Mapa MIDI - identico a docs/CDJ1000MK3_MIDI_Mapping.csv (Deck 1 = canal 1).
// Entradas marcadas EXTRA nao existem no CSV original.
#ifndef CDJ_MIDIMAP_H
#define CDJ_MIDIMAP_H
#define N_PLAY_PAUSE   0x0B
#define N_CUE          0x0C
#define N_LOOP_IN      0x10
#define N_LOOP_OUT     0x11
#define N_RELOOP_EXIT  0x12
#define N_HOTCUE_A     0x1B
#define N_HOTCUE_B     0x1C
#define N_HOTCUE_C     0x1D
#define N_TRACK_REV    0x15
#define N_TRACK_FWD    0x16
#define N_SEARCH_REV   0x17
#define N_SEARCH_FWD   0x18
#define N_FOLDER_REV   0x19
#define N_FOLDER_FWD   0x1A
#define N_MASTER_TEMPO 0x1E
#define N_TEMPO_RANGE  0x1F
#define N_REVERSE      0x20
#define N_JOG_TOUCH    0x36
#define N_SLIP_MODE    0x3F   // botao JOG MODE (do MidiMap.h do projeto)
// EXTRA (sem entrada no CSV):
#define N_TEMPO_RESET  0x40
#define N_REC_MODE     0x41
#define N_CALL_FWD     0x42
#define N_CALL_REV     0x43
#define N_MEMORY       0x44
#define N_DELETE       0x45
#define N_EJECT        0x46
#define N_TIME_MODE    0x47
#define N_TEXT_MODE    0x48

#define CC_PITCH_MSB   0x00
#define CC_PITCH_LSB   0x20
#define CC_JOG_SCRATCH 0x21   // relativo: 64 + delta
#define CC_JOG_BEND    0x22   // relativo: 64 + delta
#define CC_BRAKE       0x08
#define CC_RELEASE     0x09
#endif
