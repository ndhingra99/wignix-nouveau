#!/bin/sh
# Runs on the W520: Helium in a window on the screen with the test driver,
# the given page from ./nvk-test, a capture of the window after a while,
# the page's console lines and the kernel log lines it caused.
# usage: helium-canvas.sh <page.html> [extra helium flags]
page=$1; shift
export DISPLAY=:0 VK_ICD_FILENAMES=$HOME/nvk-test/nvk_icd.json
[ -f ~/.Xauthority ] || export XAUTHORITY=$(ls /tmp/serverauth.* 2>/dev/null | head -1)
t0=$(echo wignix | sudo -S dmesg 2>/dev/null | wc -l)
(timeout 22 /usr/lib/helium/helium --user-data-dir=$HOME/nvk-test/prof-x --no-first-run \
	--new-window --window-size=640,560 --enable-unsafe-webgpu --enable-features=Vulkan \
	--enable-logging=stderr --log-level=0 "$@" \
	file://$HOME/nvk-test/$page >/dev/null 2>$HOME/nvk-test/helium.err &)
sleep 14
id=$(xwininfo -root -tree 2>/dev/null | grep -i 'wgpu-canvas' | head -1 | awk '{print $1}')
[ -n "$id" ] && xwd -id $id -out $HOME/nvk-test/helium.xwd && echo "captured window $id"
sleep 10
grep -o 'WGPU [^"]*' $HOME/nvk-test/helium.err | cut -c1-200 | head -12
grep -v -i dbus $HOME/nvk-test/helium.err | grep -i -E 'vulkan|dawn|skia|gpu process|exited|crash|lost|fallback' |
	grep -v 'NVK is not\|WGPU' | cut -c1-220 | head -12
echo wignix | sudo -S dmesg 2>/dev/null | tail -n +$((t0+1)) | grep -i -E 'nouveau|segfault' |
	sed 's/.*nouveau 0000:01:00.0: //' | cut -c1-130 | sort | uniq -c | sort -rn | head -8
