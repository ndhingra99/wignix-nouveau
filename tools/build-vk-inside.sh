#!/bin/sh
set -e
cd /builddir/mesa-dbg
# bindgen loads libclang 23; give it that version's own headers
export BINDGEN_EXTRA_CLANG_ARGS="-resource-dir $(ls -d /usr/lib/llvm/23/lib/clang/* | head -1)"
export CFLAGS="-O2 -march=sandybridge -mtune=sandybridge -pipe" CXXFLAGS="-O2 -march=sandybridge -mtune=sandybridge -pipe"
[ -d build-vk ] || meson setup build-vk --prefix=/usr --libdir=/usr/lib --buildtype=debugoptimized -Db_ndebug=false -Db_lto=false \
  --wrap-mode=nodownload --native-file /builddir/mesa-dbg/.native-vk.ini \
  -Dgallium-drivers= -Dvulkan-drivers=nouveau -Dllvm=enabled -Dshared-llvm=enabled -Dglvnd=disabled -Dglx=disabled -Degl=disabled \
  -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dplatforms=x11 -Dlibunwind=disabled -Dvideo-codecs= \
  -Dgallium-va=disabled -Dlmsensors=disabled -Dcpp_std=gnu++17 -Dvalgrind=disabled -Drust_std=2021
ninja -C build-vk -j 12
