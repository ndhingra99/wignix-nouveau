#!/bin/sh
# Runs on the W520: a test from $HOME/nvk-test against the test driver, then the
# kernel log lines it caused.  usage: t.sh <program> [args]
cd $HOME/nvk-test
export VK_ICD_FILENAMES=$HOME/nvk-test/nvk_icd.json
export DISPLAY=:0
t0=$(echo wignix | sudo -S dmesg 2>/dev/null | wc -l)
timeout 30 "$@" 2>&1 | grep -v 'WARNING: NVK\|rustc\[\|BacktraceLock\|std\[\|core\[\|<unknown>' | sed 's/^ *[0-9]*: *0x[0-9a-f]* - /    /' | tail -14
# faults are logged after the process is gone
sleep 1
echo wignix | sudo -S dmesg 2>/dev/null | tail -n +$((t0+1)) | grep -i nouveau | grep -v 'helium\|fifo:000000:0[0-9]\[' | sed 's/.*nouveau 0000:01:00.0: //' | cut -c1-160 | sort | uniq -c | sort -rn | head -20
