#!/bin/sh
# Runs on the W520: a Helium window on the screen that uses the test driver
# in ./nvk-test (WebGPU on, compositor on Vulkan), with its own profile.
# It stays open until closed.  Log: ./nvk-test/helium-window.err
# The driver is the release build in ./nvk-test/rel when there is one, else
# the debug build.
# usage: helium-test-window.sh [url]
icd=$HOME/nvk-test/rel/nvk_icd.json
[ -f $icd ] || icd=$HOME/nvk-test/nvk_icd.json
export DISPLAY=:0 VK_ICD_FILENAMES=$icd
[ -f ~/.Xauthority ] || export XAUTHORITY=$(ls /tmp/serverauth.* 2>/dev/null | head -1)
setsid nohup /usr/lib/helium/helium --user-data-dir=$HOME/nvk-test/prof-x --no-first-run \
	--new-window --enable-unsafe-webgpu --enable-features=Vulkan \
	--hide-crash-restore-bubble --enable-logging=stderr --log-level=0 \
	"${1:-file://$HOME/nvk-test/canvas.html}" >/dev/null 2>$HOME/nvk-test/helium-window.err &
