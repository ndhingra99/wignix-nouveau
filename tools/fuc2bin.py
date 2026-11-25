#!/usr/bin/python3
# Turns the kernel's built-in copy-engine image (ce/fuc/gf100.fuc3.h) into
# the two binary files the test module loads from /lib/firmware/nouveau:
# ce_test_code and ce_test_data.  usage: fuc2bin.py <gf100.fuc3.h> <out dir>
import re, struct, sys
src = open(sys.argv[1]).read()
for name, out in (("gf100_ce_data", "ce_test_data"), ("gf100_ce_code", "ce_test_code")):
    body = re.search(name + r"\[\] = \{(.*?)\};", src, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    words = [int(w, 16) for w in re.findall(r"0x[0-9a-fA-F]+", body)]
    open("%s/%s" % (sys.argv[2], out), "wb").write(struct.pack("<%dI" % len(words), *words))
    print(out, len(words) * 4, "bytes")
