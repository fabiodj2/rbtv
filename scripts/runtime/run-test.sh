#!/bin/sh
# Roda o player RX3 no chroot com strace e um timeout.
# Monta /proc e /sys DENTRO do runtime, desmonta ao sair via trap.
# Nunca monta devtmpfs — cria apenas os character devices básicos.
#
# Uso:  sudo sh scripts/runtime/run-test.sh [timeout_segundos]
#
# Pré-requisitos:
#   - work/runtime/rx3 montado por scripts/runtime/assemble-rk3399.sh
#   - strace instalado no host
#   - execução como root (sudo)

set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
RUNTIME=${RUNTIME:-$REPO/work/runtime/rx3}
TIMEOUT=${1:-15}

# --- Validação defensiva (a lição do incidente) ---

if [ -z "$RUNTIME" ]; then
    echo "ERRO: RUNTIME vazio" >&2
    exit 1
fi

case "$RUNTIME" in
    /*) ;;
    *) echo "ERRO: RUNTIME precisa ser caminho absoluto: $RUNTIME" >&2; exit 1 ;;
esac

if [ ! -d "$RUNTIME" ]; then
    echo "ERRO: RUNTIME não existe: $RUNTIME" >&2
    exit 1
fi

if [ ! -f "$RUNTIME/root/pdj/rbp" ]; then
    echo "ERRO: player ausente em $RUNTIME/root/pdj/rbp" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "ERRO: execute como root (sudo sh $0)" >&2
    exit 1
fi

command -v strace >/dev/null || { echo "ERRO: instale strace" >&2; exit 1; }

# --- Trap de limpeza ---

cleanup()
{
    # Desmonta apenas o que NÓS montamos (não mexe em montagens pré-existentes)
    if mountpoint -q "$RUNTIME/sys" 2>/dev/null; then
        umount "$RUNTIME/sys" 2>/dev/null || true
    fi
    if mountpoint -q "$RUNTIME/proc" 2>/dev/null; then
        umount "$RUNTIME/proc" 2>/dev/null || true
    fi
}

trap cleanup EXIT HUP INT TERM

# --- Montagem defensiva ---

if ! mountpoint -q "$RUNTIME/proc" 2>/dev/null; then
    mount -t proc proc "$RUNTIME/proc"
    echo "montado: $RUNTIME/proc"
else
    echo "já montado: $RUNTIME/proc"
fi

if ! mountpoint -q "$RUNTIME/sys" 2>/dev/null; then
    mount -t sysfs sysfs "$RUNTIME/sys"
    echo "montado: $RUNTIME/sys"
else
    echo "já montado: $RUNTIME/sys"
fi

# --- Devices mínimos (sem devtmpfs) ---

for pair in \
    'null:1:3' \
    'zero:1:5' \
    'random:1:8' \
    'urandom:1:9'
do
    name=${pair%%:*}
    rest=${pair#*:}
    major=${rest%%:*}
    minor=${rest##*:}
    target="$RUNTIME/dev/$name"

    if [ ! -c "$target" ]; then
        rm -f "$target"
        mknod "$target" c "$major" "$minor"
        chmod 666 "$target"
        echo "criado: $target ($major:$minor)"
    fi
done

# --- Verificações pós-montagem ---

[ -d "$RUNTIME/proc/self" ] || { echo "ERRO: /proc/self não aparece após montar" >&2; exit 1; }
[ -d "$RUNTIME/proc/self/cmdline" ] 2>/dev/null || \
    [ -r "$RUNTIME/proc/self/cmdline" ] || { echo "ERRO: /proc/self/cmdline não legível" >&2; exit 1; }

echo "/proc/self OK"

# --- Execução ---

mkdir -p "$REPO/work/tests"
log="$REPO/work/tests/player-run.log"
strace_log="$REPO/work/tests/player-run.strace"

echo
echo "== Rodando player por ${TIMEOUT}s =="

set +e
timeout \
    --preserve-status \
    --signal=INT \
    --kill-after=5 \
    "$TIMEOUT" \
    strace \
        -f \
        -e trace=file,process,signal,ioctl \
        -s 200 \
        -o "$strace_log" \
        chroot \
            --userspec=1000:1000 \
            --groups=44 \
            "$RUNTIME" \
            /usr/bin/env \
            LD_PRELOAD=/usr/lib/memshim.so:/usr/lib/audioshim.so:/usr/lib/keyshim.so \
            /root/pdj/rbp \
    2>&1 | tee "$log"
resultado=${PIPESTATUS[0]:-$?}
set -e

echo
echo "RETORNO=$resultado"

echo
echo "== Últimas 30 linhas do strace =="
tail -30 "$strace_log" 2>/dev/null || true

echo
echo "== Sinais =="
grep -E 'SIG(SEGV|ABRT|BUS|ILL|KILL|TERM)' "$strace_log" 2>/dev/null | head -10 || true

echo
echo "== IOCTLs em /dev/fb0 =="
grep -E 'ioctl.*fb0' "$strace_log" 2>/dev/null | head -20 || true

echo
echo "== ENOENTs =="
grep -E 'ENOENT' "$strace_log" 2>/dev/null | \
    grep -oE '"[^"]+"' | sort -u | head -30 || true

echo
echo "== stderr do player (últimas 20 linhas) =="
grep -v -E '^[0-9]+ ' "$log" | tail -20 || true

echo
echo "PASS: execução terminada (desmontando via trap)"
