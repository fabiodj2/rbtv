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
RUNTIME=${RUNTIME:-$REPO/work/runtime/rx3}

STOCK_SHA256=3f0a1a9c4d107fcb856eb59bd77b39139b239b111f431d40d11b5c7e66dffddb
PLAYER_SHA256=11a6acdf51e776d01b2f01f16751dcc69712ed34e00d38ecd23ecef3d8ef91aa

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
$DISPLAY/fbshim32.so
$PROBE
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

# A chave descriptográfica não é necessária durante a execução do player.
rm -f "$stage/usr/local/pdj/aes256.key"

# Remover somente artefatos temporários instalados durante os testes.
rm -f \
    "$stage/usr/local/bin/audioshim-silence-test" \
    "$stage/usr/local/bin/fbshim32-smoke" \
    "$stage/usr/lib/audioshim-ddj400.so" \
    "$stage/usr/lib/fbshim-rk3399.so"

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
install -D -m 0755 "$DISPLAY/fbshim32.so" "$stage/usr/lib/fbshim.so"
install -D -m 0755 "$PROBE" "$stage/usr/local/bin/rx3-arm32-probe"

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

for device in printkdrv0 tsc2007_2-0048
do
    : >"$stage/dev/$device"
    chmod 0666 "$stage/dev/$device"
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
