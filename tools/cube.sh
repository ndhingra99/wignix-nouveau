#!/bin/sh
# Runs on the W520: vkcube against $HOME/nvk-test for a few seconds, then the kernel's view.
export DISPLAY=:0 VK_ICD_FILENAMES=$HOME/nvk-test/nvk_icd.json
[ -f ~/.Xauthority ] || export XAUTHORITY=$(ls /tmp/serverauth.* 2>/dev/null | head -1)
t0=$(echo wignix | sudo -S dmesg 2>/dev/null | wc -l)
timeout ${1:-15} vkcube --c ${2:-300} 2>&1 | grep -v 'rustc\[\|BacktraceLock\|std\[\|core\[\|<unknown>' | sed 's/^ *[0-9]*: *0x[0-9a-f]* - /    /' | head -24
echo "rc=$?"
echo wignix | sudo -S dmesg 2>/dev/null | tail -n +$((t0+1)) | grep -i nouveau | grep -v helium | head -12 | cut -c1-170
