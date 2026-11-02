# 24. Compact text in memory: representations first, then compressed text you can search

**Why:** with a repository open, memory should settle at a value set by what is open, and that
value should be as small as the data allows. This item measures where the bytes are while a
repository is open, what each representation would cost (including byte-aligned compression that
can be searched without decompressing), and orders the changes by bytes saved per unit of risk.
Everything here was measured with the programs in `tools/text-study/` on a 4,563-file tree
(this repository with its third-party code: 62 MB of text, 3,155 files with declarations).

## What holds memory while a repository is open

`get_state` scopes with the tree open, six files open, after Find in Files, completion and the map
(`tools/text-study` probe in the session that produced this item):

| Scope | KB | What |
|---|---:|---|
| `intellisense` | 12,170 | the symbol index of the whole workspace |
| `imgui` | 5,660-6,010 | font atlas (RGBA), window draw buffers, glyph tables |
| `documents` | 4,617-4,944 | six open files (imgui.cpp, EditorView.cpp, ...): about 1.4 MB of text |
| `explorer` | 1,016 | the folder tree |
| `views`, `layers`, `other`, `find`, `markdown` | 35-630 each | caches, map layers, results |

The text of the files that are not open is never held: Find in Files and the index build read a
file, use it and drop it.

## Where the bytes go

**Open documents** (`TextBuffer`: one `std::string` per line). A `std::string` is 32 bytes, and a
line longer than 15 characters also has its own heap block (malloc's header and rounding, plus
os::mem's 16-byte header). Over all 1,222,614 lines of the tree:

| Representation | Size | x the text |
|---|---:|---:|
| `std::string` per line (now) | 150.8 MB | 2.94 |
| one pool + 4-byte line offsets | 56.0 MB | 1.09 |

The overhead is the per-line container, not the characters: a representation change saves more
than any entropy coding of the characters would.

**Explorer tree** (`ExplorerItem`: a name and the full path as strings, per node, 4,926 nodes):
1,495 KB estimated (1,016 KB measured in the app); names pooled with 16-byte nodes and parent links
(the path is the parents' names): 156 KB, about 10%.

**Symbol index** (measured parts, 85,127 declarations):

| Part | KB | If each distinct value is stored once |
|---|---:|---:|
| entries (32 bytes each) | 2,660 | 20 bytes each: 1,663 |
| names | 1,235 | 827 (49,372 distinct) |
| containers (class/namespace) | 702 | 71 (4,271 distinct) |
| declaring lines | 4,662 | 4,116 (72,386 distinct) |
| paths | 178 | |
| **parts** | **9,437** | |
| reserved, unused | about 2,700 | 0 |

The app reports 12,170 KB: `SymbolIndex::reserve()` estimates from the source size (a sixth of
it for text, one entry per 400 bytes) and `finish()` copies down only when more than half is
unused, so about 2.7 MB of reserved room stays.

## Compression measured

Word-based byte-aligned codes model text as words and separators ("spaceless": a single space
after a word is implied). Each token gets a codeword of 1 to 3 bytes by frequency; in End-Tagged
Dense Code (ETDC, a successor of tagged Huffman: same idea, simpler code, nearly the same size)
the last byte of every codeword has its high bit set, so a codeword can be found in the
compressed bytes by plain `memchr`/`memcmp` and a match is checked by looking at one byte before
it. (s,c)-Dense Code tunes the split between last and continuing bytes.

Whole tree, 62 MB of text, one vocabulary (285,989 words, 5.2 MB):

| Coding | Size | Searchable without decoding |
|---|---:|---|
| ETDC codes | 33.4% | yes |
| ETDC codes + vocabulary | 41.8% | yes |
| (s,c)-DC, s=192, + vocabulary | 36.8% (`estimate.py`, which counts a lighter vocabulary: ETDC 37.4% there) | yes |
| character Huffman (bit-level) | 63.9% | no |
| zlib per file | 21.8% | no |

Search inside the codes (`tools/text-study/etdc_bench.cpp`), against the plain text scanned in
place:

| Query | Text in place | ETDC codes |
|---|---:|---:|
| whole word `size_t` (22,001 hits) | 34.1 ms | 3.9 ms |
| whole word `ImGui` (12,551 hits) | 12.0 ms | 3.6 ms |
| whole word `memory` (2,135 hits) | 23.1 ms | 8.5 ms |
| substring `emor` (379 words, 5,078 hits) | 40.1 ms | 68.8 ms (one pass, bitmap of matching words) |
| substring `Imgui` (3 words) | 13.8 ms | 15-185 ms |

A whole word is one codeword: 3-9x faster than scanning the text. A substring or a
case-insensitive word is every vocabulary word that contains it: 2-5x slower. A regex has to be
run over the vocabulary and, when it spans words, over decoded text. Decoding runs at 231 MB/s:
a 60-line screen in about 13 us, fine for drawing, but every edit and every frame would go
through it.

## Substring search: similarity baked in

The vocabulary already knows which words contain a substring (a scan of it takes 3-5 ms; a
trigram index of it 0.01-0.3 ms). What costs is finding those words in the codes. Three ways to
skip codes, measured (`substring_bench.cpp`, blocks of about 1 KB of codes, same hits everywhere):

| Substring | Text in place | Codes, every block | Block signatures (blocks read) | Per-word block lists (blocks read) |
|---|---:|---:|---:|---:|
| `emor` (379 words) | 39.3 ms | 80.5 ms | 59.0 ms (71%) | 7.9 ms (9%) |
| `ndow` (1,253 words) | 34.0 ms | 72.9 ms | 42.6 ms (50%) | 11.3 ms (12.5%) |
| `utf8` (66 words) | 23.6 ms | 64.7 ms | 43.0 ms (59%) | 0.7 ms (0.8%) |
| `ollapsi` (17 words) | 30.9 ms | 65.8 ms | 26.9 ms (33%) | 0.5 ms (0.6%) |
| `TreeNodeEx` (2 words) | 11.6 ms | 65.2 ms | 17.7 ms (22%) | 0.1 ms |
| `Imgui`, `Breadcrumb` | 9-12 ms | 68 ms | 18-50 ms | 0.0 ms |

- **Block signatures** (512-bit trigram bitmap per block, 6% of the codes): a block holds about
  3 KB of text, enough trigrams to fill most bits, so most blocks still have to be decoded.
- **Per-word block lists** (for each vocabulary word, the blocks it occurs in, as delta varints:
  4.4 MB for the tree): 3x to over 100x faster than scanning the plain text, because only the
  blocks holding a matching word are decoded. This is where the similarity pays: words sharing
  the substring are grouped by the vocabulary, and the lists jump to them.
- **Similar words, similar codes:** code length depends only on the frequency class, so within a
  class codewords can be given in alphabetical order at no cost in size; a prefix then maps to a
  few contiguous rank ranges (two comparisons instead of a bitmap). It does not skip codes: the
  membership test was never the cost.
- **Identifier pieces as words** ("TreeNodeEx" -> Tree Node Ex, so a substring on a camelCase or
  `_` boundary is a sequence of codewords): the vocabulary halves (5.2 -> 2.6 MB) but the codes
  grow (20.8 -> 26.9 MB): 55.1% of the text instead of 48.9% with the lists. Not worth it.

What it costs: codes 20.8 + vocabulary 5.2 + lists 4.4 MB = 30.4 MB for the 62 MB tree (49%).
Today the app holds none of the unopened files' text (Find in Files reads them from disk), so a
searchable compressed copy of the workspace is new memory spent for speed, not a saving; Find in
Files in place (below) scans in 9-40 ms plus about 290 ms to read the 62 MB from the disk cache.
Compression saves memory
where text is already held: the index's declaring lines (plan step 4) and open documents.

## Find in Files today

`model::searchText` copies every line into a new `std::string` and uses `std::regex` (libstdc++'s
backtracking engine) for everything but plain, case-sensitive text; Find in Files reads every
file into a new string; the panel runs each search on a new detached thread (`runAsync`: a new
malloc arena each time, the churn rule 10 removed from IntelliSense), and the automation
`search_files` runs it on the UI thread. Same 4,326 files, same hits (`search_bench.cpp`):

| Query | Now | In place | |
|---|---:|---:|---:|
| plain, match case | 120 ms | 25 ms | 5x |
| plain, ignore case | 1,503 ms | 108 ms | 14x |
| whole word | 4,323 ms | 12 ms | 354x |
| regex `Scope\("[a-z]+"\)` | 1,371 ms | 12 ms | 116x (`std::regex` only on lines holding the literal `Scope("`, ripgrep's prefilter) |

## Plan, in order

1. **Index at its real size** (no format change): `finish()` copies down to the exact size once
   per build; containers interned (a table of 4,271 instead of 85,127 copies); entries packed to
   20 bytes (file and container as 16-bit indexes, column 16-bit). About 12.2 -> 7.8 MB.
2. **Explorer tree pooled**: names in one pool, 16-byte nodes with parent and first-child links,
   paths built when asked. About 1.0 MB -> 0.16 MB.
3. **Find in Files in place**: scan the read buffer directly (memchr + memcmp, case folding on
   the first byte, word edges checked at the hit), line numbers counted only at hits, `std::regex`
   only on lines holding the pattern's required literal, one reused read buffer, one
   `os::dispatch::Worker` thread; `search_files` off the UI thread. 5-354x faster and no
   per-line allocations.
4. **Declaring lines in ETDC** with one workspace vocabulary that also stores the names: names and
   declaring lines about 5.9 -> 2.4 MB. Names stay directly comparable (the vocabulary is sorted
   storage); only hover text and AI context snippets decode, a line at a time.
5. **Documents as one pool** (a piece table: the file's text plus an append buffer for edits,
   line starts in an array): 2.94x -> about 1.1x the text of open files. This changes
   `TextBuffer::line()` from `const std::string&` to `std::string_view` for every caller, so it
   comes last and alone.

Not planned: compressing open documents (edited on every key, drawn every frame, substring and
regex search 2-5x slower, and after step 5 the remaining gain is small), and compressing the
explorer's names (156 KB after step 2).

**Tests:** each step keeps `memory_accounting_counts_scopes_and_folder_cycles_release_everything`
and `memory_cycle.py` passing, and adds a test that the new form gives the same answers as the old
(index lookups, explorer paths, search hits on a corpus with Unicode, CRLF and long lines) and an
upper bound on its bytes.
