#!/bin/sh
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)

ROOTFS=${ROOTFS:-$REPO/work/firmware-1.20/rootfs-stock}
PDJ=${PDJ:-$REPO/work/firmware-1.20/pdj-stock}
GUI=${GUI:-$REPO/work/firmware-1.20/gui-stock}
IMAGES=${IMAGES:-$REPO/work/firmware-1.20/iso-root/images}
PLAYER=${PLAYER:-$REPO/work/firmware-1.20/patched/rbp-rk3399}
SHIMS=${SHIMS:-$REPO/work/build/arm32/shims}
DISPLAY=${DISPLAY:-$REPO/work/build/arm32/display}
PROBE=${PROBE:-$REPO/work/probe/arm32/rx3-arm32-probe}
DFB=${DFB:-$REPO/work/rb/dfb}
RUNTIME=${RUNTIME:-$REPO/work/runtime/rx3}

# Hashes vêm de scripts/runtime/versions.env (fonte única).
. "$REPO/scripts/runtime/versions.env"

# Exigir privilégio explícito: o tar do rootfs precisa ler arquivos root-only.
if [ "$(id -u)" -ne 0 ]; then
    echo "ERRO: execute como root (sudo sh $0)" >&2
    echo "O tar do rootfs-stock precisa ler arquivos 0600 de root." >&2
    exit 1
fi

# Rejeitar rootfs/player que não sejam ARM32 soft-float EABI5.
check_arm32()
{
    file "$1" | grep -q 'ELF 32-bit LSB' || {
        echo "ERRO: $1 não é ELF 32-bit" >&2
        exit 1
    }
    file "$1" | grep -q 'ARM, EABI5' || {
        echo "ERRO: $1 não é ARM EABI5" >&2
        exit 1
    }
}

check_arm32 "$ROOTFS/bin/busybox"
check_arm32 "$PLAYER"

# Validar GLIBC <= 2.7 e soft-float nos shims LD_PRELOAD.
check_shim()
{
    local shim=$1
    local max
    max=$(arm-linux-gnueabi-objdump -T "$shim" 2>/dev/null |
          grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)
    case "$max" in
        GLIBC_2.4|GLIBC_2.5|GLIBC_2.6|GLIBC_2.7|'')
            ;;
        *)
            echo "ERRO: $shim exige $max (> GLIBC_2.7)" >&2
            exit 1
            ;;
    esac

    if arm-linux-gnueabi-readelf -A "$shim" | grep -q 'Tag_ABI_VFP_args'; then
        echo "ERRO: $shim é hard-float" >&2
        exit 1
    fi
}

# Checar que os .so são mais novos que os fontes correspondentes.
check_fresh()
{
    local shim=$1
    local source=$2
    [ -f "$source" ] || return 0
    [ "$shim" -nt "$source" ] || {
        echo "ERRO: $shim é mais antigo que $source; recompile" >&2
        exit 1
    }
}

# Explicação dos shims usados (os demais do rbtv são específicos de
# hardware que não existe no RK3399 ou foram substituídos por ARM64 nativo):
#   memshim.so   - redireciona mmap MAP_SHARED fd=-1 do rbp (obrigatório)
#   audioshim.so - 4 canais ALSA para DDJ-400
#   keyshim.so   - FIFO -> IKeyManager::sendKey, inicia pump do UiMain
# Não usados aqui: knobshim2, fbshim16, fbshim-tsc, gpioshim, tscshim, crashcatch.

for pair in \
    "memshim.so:memshim.c" \
    "audioshim.so:audioshim.c" \
    "keyshim.so:keyshim.c"
do
    shim_name=${pair%%:*}
    source_name=${pair##*:}
    check_shim "$SHIMS/$shim_name"
    check_fresh "$SHIMS/$shim_name" "$REPO/scripts/rb/$source_name"
done

required="
$ROOTFS/bin/busybox
$ROOTFS/lib/ld-linux.so.3
$PDJ/pdj/rbp
$GUI/pset/imagedata/imagedata.dat
$IMAGES/settings.tar.gz
$PLAYER
$SHIMS/memshim.so
$SHIMS/audioshim.so
$SHIMS/keyshim.so
$SHIMS/netshim.so
$DISPLAY/fbshim32.so
$PROBE
$DFB/lib/libdirectfb-1.4.so.0.0.0
$DFB/lib/libdirect-1.4.so.0.0.0
$DFB/lib/libfusion-1.4.so.0.0.0
$DFB/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
$DFB/lib/directfb-1.4-6/wm/libdirectfbwm_default.so
$DFB/lib/directfb-1.4-6/inputdrivers/libdirectfb_linux_input.so
$REPO/scripts/rb/directfbrc
"

for file in $required; do
    [ -e "$file" ] || {
        echo "ERRO: artefato ausente: $file" >&2
        exit 1
    }
done

printf '%s  %s\n' "$STOCK_SHA256" "$PDJ/pdj/rbp" |
    sha256sum -c -

printf '%s  %s\n' "$PLAYER_SHA256" "$PLAYER" |
    sha256sum -c -

[ ! -e "$RUNTIME" ] || {
    echo "ERRO: runtime já existe: $RUNTIME" >&2
    echo "Mova ou remova manualmente antes de reconstruir." >&2
    exit 1
}

base=$(dirname "$RUNTIME")
mkdir -p "$base"
stage=$(mktemp -d "$base/.rx3-runtime.XXXXXX")
archive=''

cleanup()
{
    if [ -n "${archive:-}" ] && [ -f "$archive" ]; then
        rm -f -- "$archive"
    fi

    if [ -n "${stage:-}" ] && [ -d "$stage" ]; then
        rm -rf -- "$stage"
    fi
}

trap cleanup EXIT HUP INT TERM

echo "== Base ARM32 glibc 2.13"
archive=$(mktemp "$base/.rootfs.XXXXXX.tar")

sudo tar     -C "$ROOTFS"     --exclude='./dev'     --exclude='./proc'     --exclude='./sys'     --exclude='./run'     --exclude='./tmp'     -cf "$archive"     .

tar     -C "$stage"     --no-same-owner     -xf "$archive"

rm -f -- "$archive"
archive=''

# aes256.key mantida por ora: não há evidência de que o player a abre em
# runtime, mas também não há prova de que não abre. Remover somente depois de
# confirmar empiricamente com o player rodando (ver AUDITORIA_RBTV_ORANGE_PI_4_LTS.md).

# Remover somente artefatos temporários instalados durante os testes.
rm -f \
    "$stage/usr/local/bin/audioshim-silence-test" \
    "$stage/usr/local/bin/fbshim32-smoke" \
    "$stage/usr/lib/audioshim-ddj400.so" \
    "$stage/usr/lib/fbshim-rk3399.so"

echo "== Desabilitando driver de GPU (Vivante/GAL): DirectFB deve renderizar em software"
GAL="$stage/usr/lib/directfb-1.4-0/gfxdrivers/libdirectfb_gal.so"
[ ! -f "$GAL" ] || mv "$GAL" "$GAL.disabled"

echo "== Player e recursos oficiais"
mkdir -p "$stage/root" "$stage/root/gui" "$stage/root/settings"

cp -a --no-preserve=ownership "$PDJ/pdj" "$stage/root/"
cp -a --no-preserve=ownership "$GUI/." "$stage/root/gui/"
tar -xzf "$IMAGES/settings.tar.gz" -C "$stage/root/settings"

install -m 0555 \
    "$PDJ/pdj/rbp" \
    "$stage/root/pdj/rbp-stock"

install -m 0755 \
    "$PLAYER" \
    "$stage/root/pdj/rbp"

echo "== Shims ARM32"
install -D -m 0755 "$SHIMS/memshim.so" "$stage/usr/lib/memshim.so"
install -D -m 0755 "$SHIMS/audioshim.so" "$stage/usr/lib/audioshim.so"
install -D -m 0755 "$SHIMS/keyshim.so" "$stage/usr/lib/keyshim.so"
install -D -m 0755 "$SHIMS/netshim.so" "$stage/usr/lib/netshim.so"
install -D -m 0755 "$DISPLAY/fbshim32.so" "$stage/usr/lib/fbshim.so"
install -D -m 0755 "$PROBE" "$stage/usr/local/bin/rx3-arm32-probe"

echo "== DirectFB customizado RK3399"
rm -rf "$stage/usr/lib/directfb-1.4-6"
cp -a "$DFB/lib/." "$stage/usr/lib/"
install -D -m 0644 "$REPO/scripts/rb/directfbrc" "$stage/etc/directfbrc"

echo "== Diretórios de runtime"
mkdir -p \
    "$stage/dev" \
    "$stage/proc" \
    "$stage/sys" \
    "$stage/run" \
    "$stage/tmp" \
    "$stage/media/usb1/sda1" \
    "$stage/media/usb2/sda1"

chmod 1777 "$stage/tmp"

echo "== Framebuffer virtual 1280x800x32"
truncate -s $((1280 * 800 * 4)) "$stage/dev/fb0"
chmod 0666 "$stage/dev/fb0"

echo "== Dispositivos emulados"
for device in \
    subucom_spi1.0 \
    subucom_spi2.0 \
    subucom_spi_rdy3.0 \
    subucom_spi_rdy4.0 \
    hidg0
do
    rm -f "$stage/dev/$device"
    mkfifo -m 0666 "$stage/dev/$device"
done

# printkdrv0 é apenas um arquivo stub.
: >"$stage/dev/printkdrv0"
chmod 0666 "$stage/dev/printkdrv0"

# O touch bridge escreve relatórios RX3 neste FIFO. rx3-control é usado pelo
# helper para comandos auxiliares da interface.
for device in tsc2007_2-0048 rx3-control
do
    rm -f "$stage/dev/$device"
    mkfifo -m 0666 "$stage/dev/$device"
done

dd if=/dev/zero bs=4096 count=1 2>/dev/null |
    tr '\000' '\001' >"$stage/dev/gpiodrv"

chmod 0666 "$stage/dev/gpiodrv"

: >"$stage/dev/mem"
chmod 0000 "$stage/dev/mem"
rm -f "$stage/dev/paudiog0"

echo "== FIFOs de controle e USB"
for fifo in \
    rb-keys.fifo \
    rb-ctrl.fifo \
    udev_usb1 \
    udev_usb2 \
    udev_usbctn1 \
    udev_usbctn2
do
    rm -f "$stage/tmp/$fifo"
    mkfifo -m 0666 "$stage/tmp/$fifo"
done

echo "== /proc mínimo emulado"
printf '%s\n' \
    'processor	: 0' \
    'model name	: ARMv7 Processor rev 10 (v7l)' \
    'Features	: swp half thumb fastmult vfp edsp neon vfpv3 tls' \
    'CPU architecture: 7' \
    'Hardware	: XDJ-RX3 compatible runtime' \
    >"$stage/proc/cpuinfo"

printf '%s\n' \
    'rootfs / rootfs rw 0 0' \
    'proc /proc proc rw 0 0' \
    'sysfs /sys sysfs rw 0 0' \
    'tmpfs /tmp tmpfs rw 0 0' \
    >"$stage/proc/mounts"

chmod 0444 "$stage/proc/cpuinfo" "$stage/proc/mounts"

rm -f "$stage/etc/mtab"
ln -s /proc/mounts "$stage/etc/mtab"

echo "== Manifesto do runtime"
stock_hash=$(sha256sum "$stage/root/pdj/rbp-stock" | awk '{print $1}')
player_hash=$(sha256sum "$stage/root/pdj/rbp" | awk '{print $1}')
memshim_hash=$(sha256sum "$stage/usr/lib/memshim.so" | awk '{print $1}')
audioshim_hash=$(sha256sum "$stage/usr/lib/audioshim.so" | awk '{print $1}')
keyshim_hash=$(sha256sum "$stage/usr/lib/keyshim.so" | awk '{print $1}')
fbshim_hash=$(sha256sum "$stage/usr/lib/fbshim.so" | awk '{print $1}')

cat >"$stage/RX3-RK3399-RUNTIME" <<EOF
schema=1
firmware=1.20
board=Orange Pi 4 LTS
soc=RK3399
display=1920x1080
controller=Pioneer DDJ-400
stock_player_sha256=$stock_hash
player_sha256=$player_hash
memshim_sha256=$memshim_hash
audioshim_sha256=$audioshim_hash
keyshim_sha256=$keyshim_hash
fbshim_sha256=$fbshim_hash
EOF

chmod 0444 "$stage/RX3-RK3399-RUNTIME"

printf '%s  %s\n' "$STOCK_SHA256" "$stage/root/pdj/rbp-stock" |
    sha256sum -c -

printf '%s  %s\n' "$PLAYER_SHA256" "$stage/root/pdj/rbp" |
    sha256sum -c -

mv "$stage" "$RUNTIME"
stage=''

trap - EXIT HUP INT TERM

echo "PASS: runtime montado em $RUNTIME"
