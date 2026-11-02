"""Usage: sites.py before.txt after.txt [binary]  (dumps of tools/malloc-trace/preload.so)"""
# Net live bytes per call site between two dumps of the malloc tracer.
import re, sys, collections, subprocess
APP = sys.argv[3] if len(sys.argv) > 3 else "bin/linux/x64/EasyNotepad"
SKIP = ("libc.so", "preload.so", "libstdc++", "operator new", "__libc", "malloc", "calloc", "realloc")
def load(path):
    sites = collections.Counter(); blocks = collections.Counter(); cur = None; size = 0; frames = []
    def flush():
        if cur is not None:
            key = tuple(frames[:6]); sites[key] += size; blocks[key] += 1
    for line in open(path, errors="replace"):
        if line.startswith("B "):
            flush(); cur = True; size = int(line.split()[1]); frames = []
        elif line.startswith("TOTAL"):
            flush(); cur = None
        elif cur is not None:
            m = re.match(r"(.*?)\((.*?)\)\s*\[", line.strip())
            if m:
                mod, sym = m.group(1).rsplit("/", 1)[-1], m.group(2)
                frames.append(f"{mod}({sym})")
    return sites, blocks
a, ab = load(sys.argv[1]); b, bb = load(sys.argv[2])
diff = {k: b[k] - a[k] for k in set(a) | set(b) if b[k] != a[k]}
def name(frame):
    m = re.match(r"EasyNotepad\(\+(0x[0-9a-f]+)\)", frame)
    if not m: return frame
    out = subprocess.run(["addr2line", "-e", APP, "-f", "-C", m.group(1)], capture_output=True, text=True).stdout.split("\n")[0]
    return "EasyNotepad:" + out.split("(")[0][:80]
total = sum(diff.values())
print(f"net {total:+d} bytes, {sum(bb.values()) - sum(ab.values()):+d} blocks, across {len(diff)} call sites")
for k, v in sorted(diff.items(), key=lambda kv: -kv[1])[:12]:
    if v <= 0: continue
    chain = [name(f) for f in k if not any(s in f for s in SKIP)]
    print(f"{v:+8d} B {bb[k] - ab[k]:+5d} blk  " + " <- ".join(chain[:4]))
