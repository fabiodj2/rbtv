# CDJ-1000 MK3 painel → USB MIDI (Arduino Pro Micro 5V)

Firmware para ler o painel de um Pioneer CDJ-1000 MK3 com uma **Pro Micro 5V (ATmega32U4)** e expô-lo como controlador **USB MIDI**.
Protocolo e mapa MIDI vêm de [fabiodj2/cdj1000mk3-spi-bridge](https://github.com/fabiodj2/cdj1000mk3-spi-bridge)
(`reference/protocol_full.txt`, `docs/CDJ1000MK3_MIDI_Mapping.csv`); código escrito do zero. Biblioteca necessária: **MIDIUSB**.

## Ligação
O painel é o **master** SPI (modo 3, ~1 MHz, 5 V); a Pro Micro é **slave**.

| Painel CDJ | Pro Micro |
|---|---|
| 13 SCLK | D15 (SCK) |
| 14 SOUT | D14 (MISO) |
| 15 SIN  | D16 (MOSI) |
| 16 XCS  | **PB0 = pad do LED RX** |
| GND     | GND |

* **XCS no PB0, não no D10.** No 32U4 o SS do hardware é o PB0 (sem pino no header; solde um fio no pad do LED RX). O preset `PRO_MICRO` da biblioteca original usa D10, que não ativa o SPI slave e não tem interrupção.
* Se não chegar nenhum quadro válido, troque os fios 14/15 (a nomenclatura SIN/SOUT é ambígua).
* O painel é alimentado pela fonte do CDJ, **nunca pelo USB**. GND comum.

## Comportamento
* Botões → Notes do CSV (canal 1). Extras sem entrada no CSV (Tempo Reset, Rec, Call, Memory, Delete, Eject, Time, Text) usam notas 0x40–0x48.
* Jog → CC 0x21 (com toque) / CC 0x22 (sem toque), relativo `64+delta`. Pitch 14 bits → CC 0x00 + CC 0x20. Freio/Release → CC 0x08/0x09.
* MIDI de entrada (LEDs): Play, Cue, Loop In/Out, Reloop, Reverse, Master Tempo, Tempo Reset, Slip(Vinyl); hot cues A/B/C com velocity 100+ verde, 64–99 laranja, 1–63 vermelho.
* O anel do jog (135 posições) segue a rotação. Display/waveform: fase futura.
* `DEBUG_SERIAL` (115200): estatísticas por segundo (`ok / invalidos / curtos`); enviar `d` despeja o último quadro.

## Status — leia
* Compila para `atmega32u4` (avr-gcc, com stubs do Arduino) e `test/protocol_test.c` passa no PC (`gcc test/protocol_test.c && ./a.out`).
* **Não testado em hardware.** Pontos a validar: máscara de toque do jog (`SW_TOUCH_MASK`), se o core USB mexe no PB0, e se a interrupção USB atrasa o ISR do SPI (veja `curtos` no serial; a 1 MHz há 8 µs por byte).
