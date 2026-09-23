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
