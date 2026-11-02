#!/usr/bin/env python3
"""Memory check on the real app: fresh start, then open a folder, use it, close it, N times.

Drives EasyNotepad through its automation endpoint (no clicks) and compares what the process holds
after each Close Folder with what it held when it had just started. Exits 1 when a close does not
give memory back (docs/MEMORY.md, "Definition of done").

    python3 scripts/memory_cycle.py --app bin/linux/x64/EasyNotepad --folder ~/code/some-repo \
        --files README.md docs/guide.md assets/logo.png lib/libfoo.so

Linux needs a display for the app (DISPLAY=:1, or xvfb-run). Only the standard library is used.
It never runs commands that start the agent CLI (Dissect Project, Ask): those cost real agent
usage and keep running after the close; the map is built with the local Refresh Map instead.

What passes: after the warm-up cycles (first-use tables, caches filling, the GL driver touching its
buffers), the readings after each close may oscillate but must not climb. On Linux the private
memory is split by mapping: the malloc heap and thread arenas must stay flat; the GPU driver's
mappings must keep their sizes (pages first written inside them are reported, not failed: they are
the driver's fixed command buffers filling up once, docs/MEMORY.md section 6).
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time


def mappings(pid):
    """Private-dirty KB and total size KB per mapping class, from /proc/<pid>/smaps (Linux)."""
    path = f"/proc/{pid}/smaps"
    if not os.path.exists(path):
        return None
    out = {}
    cls = None
    for line in open(path):
        parts = line.split()
        if parts and "-" in parts[0] and len(parts) >= 5 and all(c in "0123456789abcdef-" for c in parts[0]):
            name = parts[5] if len(parts) > 5 else ""
            if name == "[heap]":
                cls = "heap"
            elif name == "":
                cls = "anon"
            elif name.startswith("/dev/nvidia") or name.startswith("/dev/dri") or "nvidia" in name or "libGL" in name or "dri" in name:
                cls = "driver"
            else:
                cls = None
            if cls:
                lo, hi = parts[0].split("-")
                entry = out.setdefault(cls, {"dirty": 0, "size": 0})
                entry["size"] += (int(hi, 16) - int(lo, 16)) // 1024
        elif cls and line.startswith("Private_Dirty:"):
            out[cls]["dirty"] += int(parts[1])
    return out


def theil_sen(values):
    """Robust slope per cycle: the median of all pairwise slopes. One spike cannot move it."""
    n = len(values)
    if n < 3:
        return 0.0
    slopes = sorted((values[j] - values[i]) / (j - i) for i in range(n) for j in range(i + 1, n))
    return slopes[len(slopes) // 2]


def call(endpoint, method, params=None, timeout=60):
    ep = json.load(open(endpoint))
    with socket.create_connection(("127.0.0.1", ep["port"]), timeout=timeout) as s:
        s.sendall((json.dumps({"token": ep["token"], "id": 1, "method": method, "params": params or {}}) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
    reply = json.loads(buf.decode())
    if "error" in reply:
        raise RuntimeError(f"{method}: {reply['error']}")
    return reply["result"]


def snapshot(endpoint, settle):
    time.sleep(settle)  # the app trims 4 frames after a close and again 1.5 s later
    st = call(endpoint, "get_state")
    mem, proc = st["memory"], st["process"]
    return {
        "heapKB": mem["liveKB"],
        "blocks": mem["blocks"],
        "privateKB": proc["privateKB"],
        "residentKB": proc["residentKB"],
        "handles": proc.get("handles", 0),
        "gdi": proc.get("gdiObjects", 0),
        "user": proc.get("userObjects", 0),
        "threads": proc.get("threads", 0),
        "imguiWindows": st.get("imgui", {}).get("windows", 0),
        "scopes": {s["name"]: s["liveKB"] for s in mem["scopes"] if s["blocks"]},
    }


def row(label, s):
    print(f"{label:<28} heap {s['heapKB']:>7} KB / {s['blocks']:>7} blocks   private {s['privateKB']:>7} KB   "
          f"rss {s['residentKB']:>7} KB   handles {s['handles']:>4}  gdi {s['gdi']:>4}  user {s['user']:>4}  "
          f"threads {s['threads']:>3}  imgui windows {s['imguiWindows']}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", help="the EasyNotepad binary to start (omit with --endpoint to use a running one)")
    ap.add_argument("--endpoint", help="endpoint file of an app already running with EASYNOTEPAD_ENDPOINT")
    ap.add_argument("--folder", required=True, help="the folder to open each cycle (absolute path)")
    ap.add_argument("--files", nargs="*", default=[], help="workspace-relative files to open each cycle")
    ap.add_argument("--steps", nargs="*", default=[],
                    help="after the files, each cycle: open=FILE, click=AUTOMATION_ID or command=NAME, in order "
                         "(e.g. open=data.json click=data-view:tree click=data-view:accordion)")
    ap.add_argument("--cycles", type=int, default=8)
    ap.add_argument("--warmup", type=int, default=3, help="cycles that may still grow (caches, driver buffers)")
    ap.add_argument("--csv", help="write every reading to this CSV file (cycle, heap, blocks, private, rss, mappings...)")
    ap.add_argument("--print-every", type=int, default=1, help="print only every Nth cycle (long runs)")
    ap.add_argument("--trend-kb", type=float, default=128.0,
                    help="long runs: fail when the fitted growth over the second half of the cycles exceeds this (KB)")
    ap.add_argument("--settle", type=float, default=3.5, help="seconds to wait before each reading")
    ap.add_argument("--no-map", action="store_true", help="skip the Map view and Refresh Map")
    ap.add_argument("--heap-slack-kb", type=int, default=256,
                    help="heap after a close may exceed the fresh start by this much (first-use tables)")
    ap.add_argument("--private-slack-kb", type=int, default=4096,
                    help="private memory after a close may exceed the fresh start by this much (allocator, driver)")
    ap.add_argument("--drift-kb", type=int, default=512, help="growth allowed between the first and last close")
    args = ap.parse_args()

    proc = None
    endpoint = args.endpoint
    if args.app:
        endpoint = os.path.join(tempfile.mkdtemp(prefix="enp-memory-cycle-"), "endpoint.json")
        env = dict(os.environ, EASYNOTEPAD_ENDPOINT=endpoint)
        proc = subprocess.Popen([os.path.abspath(args.app)], env=env, cwd=os.path.dirname(os.path.abspath(args.app)),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(endpoint) and os.path.getsize(endpoint) > 0:
                break
            time.sleep(0.1)
    if not endpoint or not os.path.exists(endpoint):
        sys.exit("no endpoint: pass --app, or --endpoint of an app started with EASYNOTEPAD_ENDPOINT")

    failures = []
    try:
        fresh = snapshot(endpoint, args.settle)
        row("fresh start", fresh)
        pid = json.load(open(endpoint)).get("pid")
        closes = []
        maps = []
        for cycle in range(1, args.cycles + 1):
            call(endpoint, "open_folder", {"path": os.path.abspath(args.folder)})
            for f in args.files:
                try:
                    call(endpoint, "open_file", {"path": f})
                except RuntimeError as e:
                    print(f"  ({e})")
                time.sleep(0.3)
            for step in args.steps:
                kind, _, value = step.partition("=")
                method, params = {"open": ("open_file", {"path": value}), "click": ("click", {"id": value}),
                                  "command": ("execute_command", {"command": value})}.get(kind, (None, None))
                if not method:
                    sys.exit(f"unknown step {step!r}: use open=, click= or command=")
                try:
                    call(endpoint, method, params)
                except RuntimeError as e:
                    print(f"  ({e})")
                time.sleep(0.4)
            if not args.no_map:
                call(endpoint, "execute_command", {"command": "ShowMapView"})
                time.sleep(1.0)
                call(endpoint, "execute_command", {"command": "RefreshMap"})
                time.sleep(1.5)
                call(endpoint, "execute_command", {"command": "ShowCodeView"})
            opened = snapshot(endpoint, 1.0)
            if cycle % args.print_every == 0 or cycle == 1:
                row(f"cycle {cycle}: open", opened)
            call(endpoint, "execute_command", {"command": "CloseFolder"})
            closed = snapshot(endpoint, args.settle)
            if cycle % args.print_every == 0 or cycle == 1 or cycle == args.cycles:
                row(f"cycle {cycle}: after close", closed)
            closes.append(closed)
            maps.append(mappings(pid) if pid else None)
            if args.csv:
                m = maps[-1] or {}
                with open(args.csv, "a" if cycle > 1 else "w") as f:
                    if cycle == 1:
                        f.write("cycle,heapKB,blocks,privateKB,rssKB,handles,threads,heapDirtyKB,anonDirtyKB,driverDirtyKB,driverMappedKB,openHeapKB\n")
                    f.write(",".join(str(x) for x in (
                        cycle, closed["heapKB"], closed["blocks"], closed["privateKB"], closed["residentKB"], closed["handles"],
                        closed["threads"], m.get("heap", {}).get("dirty", 0), m.get("anon", {}).get("dirty", 0),
                        m.get("driver", {}).get("dirty", 0), m.get("driver", {}).get("size", 0), opened["heapKB"])) + "\n")

        last = closes[-1]
        base = min(args.warmup, len(closes) - 1)  # the first close after the warm-up
        if maps[base] and maps[-1]:
            print(f"private memory by mapping, after close {base + 1} -> after close {len(closes)} (KB):")
            for cls in ("heap", "anon", "driver"):
                a, b = maps[base].get(cls, {"dirty": 0, "size": 0}), maps[-1].get(cls, {"dirty": 0, "size": 0})
                print(f"  {cls:<7} dirty {a['dirty']:>7} -> {b['dirty']:>7} ({b['dirty'] - a['dirty']:+d})   mapped {a['size']:>8} -> {b['size']:>8}")
            # Climbing, not oscillating: every reading after the warm-up above the one before it.
            for cls in ("heap", "anon"):
                series = [m.get(cls, {"dirty": 0})["dirty"] for m in maps[base:] if m]
                rises = all(y >= x for x, y in zip(series, series[1:]))
                if len(series) > 2 and rises and series[-1] - series[0] > 64:
                    failures.append(f"{cls} private memory climbs every cycle: {series}")
            a, b = maps[base].get("driver"), maps[-1].get("driver")
            if a and b and b["size"] > a["size"]:
                failures.append(f"GPU driver mappings grow: {a['size']} -> {b['size']} KB (new buffers per cycle)")
        later = closes[base:]
        # Long runs: fit a robust trend through the second half of the readings. Early cycles may
        # still be filling caches and driver buffers; whatever is still rising after that is a leak.
        if len(later) >= 20:
            half = later[len(later) // 2:]
            half_maps = [m for m in maps[base:][len(later) // 2:] if m]
            series = {"private": [c["privateKB"] for c in half], "heap blocks": [c["blocks"] for c in half]}
            if len(half_maps) == len(half):
                series["malloc heap dirty"] = [m.get("heap", {}).get("dirty", 0) for m in half_maps]
                series["anonymous dirty"] = [m.get("anon", {}).get("dirty", 0) for m in half_maps]
                series["driver dirty"] = [m.get("driver", {}).get("dirty", 0) for m in half_maps]
            print(f"trend over cycles {base + 1 + len(later) // 2}..{len(closes)} (Theil-Sen, per cycle -> over the window):")
            for name, values in series.items():
                slope = theil_sen(values)
                grown = slope * len(values)
                limit = 8 if name == "heap blocks" else args.trend_kb
                print(f"  {name:<18} {slope:+9.3f} per cycle -> {grown:+9.1f} over {len(values)} cycles   "
                      f"min {min(values)}  max {max(values)}  mean {sum(values) / len(values):.1f}")
                if grown > limit:
                    failures.append(f"{name} still climbs in the second half: {slope:+.3f}/cycle = {grown:+.1f} over {len(values)} cycles (limit {limit})")
        if len(later) > 1 and (max(c["heapKB"] for c in later) - min(c["heapKB"] for c in later) > 16):
            failures.append("heap after close is not stable after the warm-up: " + str([c["heapKB"] for c in later]))
        if last["heapKB"] > fresh["heapKB"] + args.heap_slack_kb:
            failures.append(f"heap after close {last['heapKB']} KB > fresh {fresh['heapKB']} KB + {args.heap_slack_kb}")
        if last["privateKB"] > fresh["privateKB"] + args.private_slack_kb:
            failures.append(f"private after close {last['privateKB']} KB > fresh {fresh['privateKB']} KB + {args.private_slack_kb}")
        if len(later) > 1:
            first = later[0]
            if last["blocks"] - first["blocks"] > 64:
                failures.append(f"heap blocks grow per cycle: {first['blocks']} -> {last['blocks']}")
            if last["privateKB"] - first["privateKB"] > args.drift_kb:
                failures.append(f"private grows per cycle: {first['privateKB']} -> {last['privateKB']} KB")
            for key in ("handles", "gdi", "user", "threads"):
                if last[key] > first[key] + 2:
                    failures.append(f"{key} grow per cycle: {first[key]} -> {last[key]}")
        grown = {k: v for k, v in last["scopes"].items() if v > fresh["scopes"].get(k, 0) + 16}
        if grown:
            print("scopes above the fresh start after the last close (KB):",
                  {k: f"{fresh['scopes'].get(k, 0)} -> {v}" for k, v in grown.items()})
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        sys.exit(1)
    print("OK: closing the folder gives the memory back")


if __name__ == "__main__":
    main()
