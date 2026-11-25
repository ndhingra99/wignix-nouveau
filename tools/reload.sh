#!/bin/sh
# Swap the running nouveau for /tmp/nouveau.ko and bring the X session back.
# usage (root): reload.sh "<nouveau config string>" ["<nouveau debug string>"]
exec > /tmp/reload.log 2>&1
echo "== $(date) config=$1"
sv down agetty-tty1
sleep 1
pkill -x Xorg; pkill -x xinit
sleep 3
pkill -9 -x Xorg
for p in $(lsof -t /dev/dri/card0 /dev/dri/renderD128 2>/dev/null | sort -u); do
	echo "killing $p $(cat /proc/$p/comm 2>/dev/null)"; kill -9 $p
done
sleep 2
for v in /sys/class/vtconsole/vtcon*; do
	grep -q "frame buffer" $v/name && echo 0 > $v/bind
done
sleep 1
if rmmod nouveau; then
	if insmod /tmp/nouveau.ko config="$1" ${2:+debug="$2"}; then
		echo "test module loaded"
	else
		echo "INSMOD FAILED, loading the stock module"
		modprobe nouveau
	fi
else
	echo "RMMOD FAILED"; lsmod | grep '^nouveau'
fi
sleep 3
for v in /sys/class/vtconsole/vtcon*; do
	grep -q "frame buffer" $v/name && echo 1 > $v/bind
done
dmesg | grep -E 'nouveau' | grep -vE 'W520 (level|reg|node)|W520 mem  |timing|helium' | tail -25
cat /sys/bus/pci/drivers/nouveau/*/pstate
sv up agetty-tty1
echo "== done $(date)"
