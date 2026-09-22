# Scratch copy of the dedicated VQ IP with its one K=256-only slice generalised.
# vq_pq_top.sv:329 takes 8 bits from a KW-bit index field; at K=64 (KW=6) that is
# out of range. Taking KW bits and letting the 8-bit field zero-extend keeps the
# output format and is identical at K=256. ip_repo is NOT modified.
import os, shutil, sys
src = "C:/Users/Fahad/ip_repo/vq_pq_axi_1.0/src"
dst = "C:/Users/Fahad/AppData/Local/Temp/claude/C--Users-Fahad-Zynq-Zynq-srcs-sources-1-new/2ba9a201-644e-401b-85ad-6761a18893ac/scratchpad/vq_src_k64"
if os.path.isdir(dst):
    shutil.rmtree(dst)
shutil.copytree(src, dst)
p = dst + "/vq_pq_top.sv"
t = open(p).read()
old = "pos_word[qq][8*i +: 8] = eng_idx[i][qq*KW +: 8];"
new = "pos_word[qq][8*i +: 8] = eng_idx[i][qq*KW +: KW];   // scratch: zero-extends at KW < 8"
n = t.count(old)
if n != 1:
    print("ANCHOR COUNT", n, "- not written")
    sys.exit(1)
open(p, "w").write(t.replace(old, new))
print("copied", sorted(os.listdir(dst)), "patched 1 line")
