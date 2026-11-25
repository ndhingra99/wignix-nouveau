#!/bin/sh
# Runs on the W520: vkcube against $HOME/nvk-test, a capture of its window after a
# few seconds, and the kernel log lines it caused.
export DISPLAY=:0 VK_ICD_FILENAMES=$HOME/nvk-test/nvk_icd.json
[ -f ~/.Xauthority ] || export XAUTHORITY=$(ls /tmp/serverauth.* 2>/dev/null | head -1)
t0=$(echo wignix | sudo -S dmesg 2>/dev/null | wc -l)
(timeout 10 vkcube --c 2000 >$HOME/nvk-test/cube.out 2>&1 &)
sleep 4
id=$(xwininfo -root -tree 2>/dev/null | grep -i 'vkcube\|Vulkan Cube' | head -1 | awk '{print $1}')
[ -n "$id" ] && xwd -id $id -out $HOME/nvk-test/cube.xwd
sleep 7
grep -v 'rustc\[\|BacktraceLock\|std\[\|core\[\|<unknown>\|Selected\|WARNING' $HOME/nvk-test/cube.out | sed 's/^ *[0-9]*: *0x[0-9a-f]* - /    /' | head -16
echo wignix | sudo -S dmesg 2>/dev/null | tail -n +$((t0+1)) | grep -i nouveau | grep -v helium | sed 's/.*nouveau 0000:01:00.0: //' | cut -c1-120 | sort | uniq -c | sort -rn | head -10
