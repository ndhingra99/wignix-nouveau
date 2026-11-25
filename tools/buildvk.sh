#!/bin/sh
# Sync the working Mesa tree into the 64-bit test tree, build NVK there and
# copy the driver to the W520's test directory (its /tmp is noexec).
# usage: buildvk.sh [nopush]
# Set WIGNIX_PKG, WIGNIX_TEST and W520 (e.g. user@host) for your own layout.
vp=${WIGNIX_PKG:-$HOME/wignix-sandy-src/void-packages}
w=$vp/masterdir-i686-w520/builddir/mesa-dbg-26.2.7
m=$vp/masterdir-x86_64_sandy-c
log=${WIGNIX_LOG:-$HOME/wignix-sandy/tmp/mesa-src/buildvk.log}
cd $vp || exit 1
rsync -a --exclude build --exclude build-vk $w/src/ $m/builddir/mesa-dbg/src/
xbps-uunshare -b $vp/hostdir:/host -- $m /builddir/mesa-dbg/.build-vk.sh > $log 2>&1
r=$?
grep -E '^\.\./.*error|^error|FAILED' $log | cut -c1-240 | head -20
[ $r = 0 ] || exit $r
b=$m/builddir/mesa-dbg/build-vk/src/nouveau/vulkan
mkdir -p /tmp/nvk
cp $b/libvulkan_nouveau.so /tmp/nvk/ && strip -g /tmp/nvk/libvulkan_nouveau.so
sed 's|"library_path": *"[^"]*"|"library_path": "'"${WIGNIX_TEST:-$HOME/nvk-test}"'/libvulkan_nouveau.so"|' \
	$b/nouveau_icd.x86_64.json > /tmp/nvk/nvk_icd.json
echo built
[ "$1" = nopush ] && exit 0
[ -n "${W520:-}" ] || { echo "set W520=user@host to push"; exit 0; }
timeout 40 ssh -o ConnectTimeout=15 $W520 'mkdir -p nvk-test' >/dev/null 2>&1
timeout 120 scp -o ConnectTimeout=15 /tmp/nvk/libvulkan_nouveau.so /tmp/nvk/nvk_icd.json \
	$W520:nvk-test/ >/dev/null 2>&1 && echo pushed
