# RK3399 DDJ-400 — KEY e STEMS funcionais

Baseline validado em hardware em 23 de setembro de 2026.

## Funcionalidades

- KEY e STEMS disponíveis simultaneamente na tela.
- Sidecars neurais no formato RX3STM2.
- STEMS de três partes: DRUMS, VOCAL e INST.
- DDJ-400 PAD FX 2:
  - Pad 1: DRUMS
  - Pad 2: VOCAL
  - Pad 3: INST
- Sincronização bidirecional entre touchscreen, áudio e LEDs.
- Cada faixa inicia com as três partes ativas.
- Beat Jump e limite privado de imagens preservados.

## Runtime funcional

Os hashes estão registrados em
`scripts/rb/runtime/patches/rk3399-key-stems-functional.json`.

O RBP proprietário e os binários compilados não são armazenados no Git.
Uma cópia recuperável permanece em `~/rx3-private`.

## Fonte reproduzível

O overlay em
`scripts/rb/runtime/overlays/rk3399-key-stems-0cb402e0`
contém as fontes CORE/STEMS, testes, assets RGB565 e o gerador RX3STM2
usados no baseline validado.

## LEDs PLAY/CUE

- PLAY permanece aceso durante reprodução.
- PLAY pisca durante pausa ou espera no CUE.
- CUE permanece aceso quando existe uma faixa carregada.
- O estado é reafirmado a cada 500 ms para evitar divergência local da DDJ-400.

## Exclusão persistente de Hot Cues

- `SHIFT + Pad 1..8` exclui o Hot Cue correspondente.
- A exclusão atualiza imediatamente a interface e o estado do engine.
- A alteração é persistida no banco Rekordbox do pendrive.
- Os Hot Cues permanecem apagados após trocar e recarregar a faixa.
- O worker da DDJ apenas enfileira a solicitação.
- A operação nativa é executada pelo `Ui_EventTask`, acordado por `set_flg`.
- Esse dispatch evita contenção entre o worker e a interface de navegação.
- Navegação da biblioteca validada sem lentidão após a gravação.
- Core validado: `27640e3dcd1071c370182ba80eb592716333c3a1`.
