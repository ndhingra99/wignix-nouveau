#!/usr/bin/python3
# Reads or writes the GPU's registers through the PCI resource file
# (run as root on the W520).
# usage: mmio.py r <hex offset> [words]
#        mmio.py w <hex offset> <hex value>
import mmap, os, struct, sys

DEV = "/sys/bus/pci/devices/0000:01:00.0/resource0"

def main():
    op, off = sys.argv[1], int(sys.argv[2], 16)
    fd = os.open(DEV, os.O_RDWR if op == "w" else os.O_RDONLY)
    prot = mmap.PROT_READ | (mmap.PROT_WRITE if op == "w" else 0)
    m = mmap.mmap(fd, os.fstat(fd).st_size, mmap.MAP_SHARED, prot)
    if op == "w":
        m[off:off + 4] = struct.pack("<I", int(sys.argv[3], 16))
        return
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    for i in range(n):
        a = off + i * 4
        print("%06x: %08x" % (a, struct.unpack("<I", m[a:a + 4])[0]))

main()
