"""Usage: churn.py churn.txt [depth=1] [rows=30] [binary]  (a dump of tools/malloc-trace/churn.so)
What was allocated (and freed) during the traced window, by the app function that asked for it."""
import sys, re, collections, subprocess
APP = sys.argv[4] if len(sys.argv) > 4 else "bin/linux/x64/EasyNotepad"
entries=[]; cur=None
for line in open(sys.argv[1], errors="replace"):
    if line.startswith("B "):
        _, b, c = line.split(); cur=[int(b), int(c), []]; entries.append(cur)
    elif cur is not None:
        m=re.match(r"(.*?)\((.*?)\)\s*\[", line.strip())
        if m: cur[2].append((m.group(1).rsplit("/",1)[-1], m.group(2)))
cache={}
def sym(off):
    if off not in cache:
        out=subprocess.run(["addr2line","-e",APP,"-f","-C",off],capture_output=True,text=True).stdout.split("\n")[0]
        cache[off]=out.split("(")[0][:100]
    return cache[off]
SKIP=("operator new","enp::os::mem","std::","__gnu_cxx","ImGui::MemAlloc","main::{lambda","ImVector","ImChunkStream","ImPool")
def owner(frames, depth):
    names=[]; libs=[]
    for mod, s in frames:
        if mod.startswith("EasyNotepad"):
            if s.startswith("_Zn"): continue
            m=re.match(r"\+(0x[0-9a-f]+)", s)
            n=sym(m.group(1)) if m else s
            if any(x in n for x in SKIP): continue
            names.append(n)
            if len(names)>=depth: break
        elif not mod.startswith(("libc.so","libstdc++","churn.so")) and not names:
            libs.append(mod.split("-")[0].split(".so")[0])
    if names: return " <- ".join(names)
    return "lib:" + (libs[0] if libs else "?")
depth=int(sys.argv[2]) if len(sys.argv)>2 else 1
by=collections.Counter(); cnt=collections.Counter()
for b,c,f in entries:
    k=owner(f, depth); by[k]+=b; cnt[k]+=c
total=sum(by.values())
print(f"one cycle: {total/1048576:.1f} MB in {sum(cnt.values())} allocations of >= 1 KB")
for k,v in by.most_common(int(sys.argv[3]) if len(sys.argv)>3 else 30):
    print(f"{v/1024:10.0f} KB {cnt[k]:7d}x  {k}")
