#!/bin/sh
# Runs on the W520 with the copy-engine test module loaded: sets the
# module's test switches, runs the stencil copy test and prints the result
# and what the kernel logged (fault line and the engine's registers at the
# fault).  usage: ceport.sh <port value, -1 = leave> <0|1 test image>
cd /tmp/nvk
export VK_ICD_FILENAMES=/tmp/nvk/nvk_icd.json
export NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1 NVK_FERMI_DEBUG=1
p=/sys/module/nouveau/parameters
mark="ceport-$$"
echo wignix | sudo -S sh -c "echo $1 > $p/w520_ce_port; echo $2 > $p/w520_ce_fw;
	[ $2 = 1 ] && cp ce_test_code ce_test_data /lib/firmware/nouveau/;
	echo $mark > /dev/kmsg" 2>/dev/null
timeout 20 ./vkds 2>&1 | grep -i "ce copy\|B2\|B3\|B4\|B5\|failed" | cut -c1-100 | head -8
sleep 1
echo wignix | sudo -S dmesg 2>/dev/null | sed "1,/$mark/d" |
	grep -iE "fault 0|killed|ce0|ILLEGAL|INVALID|test image" |
	grep -v "cxid\|resetting" |
	sed 's/.*nouveau 0000:01:00.0: //' | cut -c1-118 | uniq -c | head -34
