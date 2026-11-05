"""Estimates, on a real tree, what each representation of the text would cost."""
# Part of tools/text-study (docs/plan/24-compact-text.md). Run: python3 -I estimate.py <tree>
import os, re, sys, zlib, heapq, collections, math
root = sys.argv[1]
EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".inl", ".py", ".md", ".txt", ".cmake", ".js", ".ts", ".java", ".cs", ".rs", ".go", ".lua", ".sh", ".json", ".yml", ".yaml")
files = []
for d, dirs, names in os.walk(root):
    dirs[:] = [x for x in dirs if not x.startswith(".")]
    for n in names:
        if n.endswith(EXT):
            p = os.path.join(d, n)
            try:
                b = open(p, "rb").read()
            except OSError:
                continue
            if len(b) <= 2 * 1024 * 1024 and b"\0" not in b[:8000]:
                files.append((os.path.relpath(p, root), b))
total = sum(len(b) for _, b in files)
print(f"{len(files)} text files, {total/1048576:.1f} MB")

def heap_block(n):  # glibc chunk for n bytes + os::mem's 16-byte header
    n += 16
    return max(32, (n + 8 + 15) // 16 * 16)
def string_cost(n):  # libstdc++ std::string: 32 inline, heap beyond 15 chars
    return 32 + (heap_block(n + 1) if n > 15 else 0)

# 1. Documents as now: vector<string> per line, vs one pool + offsets.
lines_all = 0; now = 0; pooled = 0
for _, b in files:
    ls = b.split(b"\n")
    lines_all += len(ls)
    now += sum(string_cost(len(l.rstrip(b"\r"))) for l in ls) + heap_block(32 * len(ls))
    pooled += len(b) + 4 * len(ls)
print(f"\n1. text as documents hold it: {lines_all} lines")
print(f"   now (std::string per line)          {now/1048576:7.1f} MB  = {now/total:5.2f} x the text")
print(f"   one pool + 4-byte line offsets      {pooled/1048576:7.1f} MB  = {pooled/total:5.2f} x")

# 2. Word-based byte-aligned codes over the whole tree ("spaceless words": a word followed by a single space
#    needs no separator token).
tok = re.compile(rb"[A-Za-z0-9_]+|[^A-Za-z0-9_]+")
freq = collections.Counter(); ntok = 0
for _, b in files:
    toks = tok.findall(b); ntok += len(toks)
    prev_word = False
    for t in toks:
        w = t[:1].isalnum() or t[:1] == b"_"
        if not w and prev_word and t == b" ":
            prev_word = False; continue  # implied single space
        freq[t] += 1; prev_word = w
ranked = [c for _, c in freq.most_common()]
vocab = sum(len(t) + 1 for t in freq)
def etdc(ranked, s=128):
    total = 0
    for i, f in enumerate(ranked):
        k, lim, base = 1, s, s
        while i >= lim:
            base *= 128; lim += base; k += 1
        total += f * k
    return total
def sc(ranked, s):  # (s,c)-dense code: s stoppers, c = 256 - s continuers
    c = 256 - s; total = 0
    for i, f in enumerate(ranked):
        k, lim, base = 1, s, s
        while i >= lim:
            base *= c; lim += base; k += 1
        total += f * k
    return total
e = etdc(ranked)
best_s = min(range(100, 250, 2), key=lambda s: sc(ranked, s))
scb = sc(ranked, best_s)
# character entropy (bit-level Huffman bound) and zlib for reference
cf = collections.Counter()
for _, b in files: cf.update(b)
H = -sum(c / total * math.log2(c / total) for c in cf.values())
z = sum(len(zlib.compress(b, 6)) for _, b in files)
zall = len(zlib.compress(b"".join(b for _, b in files)[:64 << 20], 6))
print(f"\n2. compressed size of the text ({len(freq)} distinct words/separators, vocabulary {vocab/1048576:.2f} MB)")
print(f"   End-Tagged Dense Code (byte-aligned, searchable)  {e/1048576:6.1f} MB  {e/total:5.1%}   + vocabulary {(e+vocab)/total:5.1%}")
print(f"   (s,c)-Dense Code, s={best_s}                       {scb/1048576:6.1f} MB  {scb/total:5.1%}   + vocabulary {(scb+vocab)/total:5.1%}")
print(f"   character Huffman (bit-level, not searchable)     {total*H/8/1048576:6.1f} MB  {H/8:5.1%}")
print(f"   zlib per file (not searchable)                    {z/1048576:6.1f} MB  {z/total:5.1%}")
print(f"   zlib whole tree (not searchable)                  {zall/1048576:6.1f} MB  {zall/min(total, 64<<20):5.1%}")

# 3. Explorer tree: as now vs names pool + parent links.
nodes = 0; now_e = 0; pool_e = 0
for d, dirs, names in os.walk(root):
    dirs[:] = [x for x in dirs if not x.startswith(".")]
    rel = os.path.relpath(d, root)
    for n in dirs + names:
        path = n if rel == "." else rel + "/" + n
        nodes += 1
        now_e += 96 + (string_cost(len(n)) - 32) + (string_cost(len(path)) - 32)
        pool_e += 16 + len(n)
    now_e += heap_block(96 * len(dirs + names)) if dirs + names else 0
print(f"\n3. explorer tree: {nodes} nodes")
print(f"   now (name + full path strings per node)  {now_e/1024:8.0f} KB")
print(f"   names pooled, 16-byte node, parent link  {pool_e/1024:8.0f} KB  ({pool_e/now_e:.0%})")
