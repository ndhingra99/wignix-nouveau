#!/bin/sh
# Runs on the W520: vulkaninfo against $HOME/nvk-test with an ioctl trace.
export DISPLAY=:0 VK_ICD_FILENAMES=$HOME/nvk-test/nvk_icd.json
cd $HOME/nvk-test
strace -f -e trace=ioctl -o st.txt timeout 40 vulkaninfo --summary > vi.txt 2>&1
grep -v '^$' vi.txt | head -${1:-30}
echo "--- failing ioctls"
grep ioctl st.txt | grep -v '= 0$' | grep -v ZCULL | sed 's/DRM_IOCTL_ETNAVIV_GET_PARAM or .*DRM_IOCTL_NOUVEAU_GETPARAM[^,]*/NOUVEAU_GETPARAM/' | tail -8 | cut -c1-170
