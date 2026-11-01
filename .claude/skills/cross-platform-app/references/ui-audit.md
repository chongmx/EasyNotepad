# Scripted UI audit at phone sizes

Goal: a screenshot of every screen, panel tab and dialog at ~390×844 and ~844×390 dp, reviewed as
contact sheets, with before/after pairs for the user. It finds what users hit on phones: overlapping
text, clipped rows, dialogs off screen.

## What made it reliable

- **Drive the app through its automation connector** (an MCP server, a test port, accessibility API),
  never the real mouse.
- **Fresh profile** per run: point the app at an empty settings folder (here `APPDATA`) so it starts
  in its sample workspace with demo data and does not restore the last session; give it its own
  connector endpoint so it cannot collide with a running copy.
- **One fresh process per screen.** Automation often cannot close a modal; restarting is simpler.
  Wait for the previous process to exit fully before starting the next (a screenshot tool that picks
  "the first process" may grab the dying one).
- **Wait long enough** after launch (~7 s), and **retry** the screens whose screenshot is missing.
- **Hide the automation pointer** before each capture (its tooltip covers things).
- **Capture the window content** (`PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT)` on Windows), not the
  screen: the user’s other windows may be on top. Pick the largest visible window of the process
  (tooltips and multi-viewport popups are separate small windows).
- Stitch 3–5 screenshots per sheet, scaled ~70 %, to review quickly.

## Driver (Python, talks MCP JSON-RPC over stdio to the app’s bridge)

The screen list holds this project's automation ids (`nav:Tools`, `panel-tab:History`, command
keys); replace them with your app's. The `--mcp` bridge and the `EASYNOTEPAD_ENDPOINT` variable are
this app's connector; any automation channel works.

```python
# Walks the app (already running, phone-sized) through screens and dialogs, screenshotting each.
# Usage: audit.py <out-prefix> [only-names...]
import json, subprocess, sys, time

EXE = '/mnt/c/path/to/YourApp.exe'
SCRATCH_WIN = r'C:\path\to\audit'
p = subprocess.Popen([EXE, '--mcp'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
n = 0
def call(method, params):
    global n
    n += 1
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': n, 'method': method, 'params': params}) + '\n'); p.stdin.flush()
    while True:
        msg = json.loads(p.stdout.readline())
        if msg.get('id') == n:
            return msg
call('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {}, 'clientInfo': {'name': 'audit', 'version': '1'}})
def tool(name, **args):
    return call('tools/call', {'name': name, 'arguments': args})
def cmd(key):
    tool('execute_command', command=key)
def click(element):
    tool('click', id=element)
def esc():
    tool('send_keys', keys='{Esc}')
def shot(name):
    tool('hide_pointer')
    time.sleep(0.8)
    out = SCRATCH_WIN + '\\audit_' + prefix + '_' + name + '.png'
    subprocess.run(['powershell.exe', '-NoProfile', '-File', SCRATCH_WIN + '\\shot5.ps1', '-out', out], capture_output=True)
    print('shot', name)

prefix = sys.argv[1]
only = set(sys.argv[2:])
screens = [
    ('history_log', lambda: (click('nav:Tools'), click('panel-tab:History'), click('git:history-log'))),
    ('history_compare', lambda: (click('nav:Tools'), click('panel-tab:History'), click('git:history-compare'))),
    ('problems', lambda: (click('nav:Tools'), click('panel-tab:Problems'))),
    ('diff', lambda: (click('nav:Tools'), click('panel-tab:Diff'))),
    ('comments', lambda: (click('nav:Tools'), click('panel-tab:Comments'))),
    ('checklist', lambda: (click('nav:Tools'), click('panel-tab:Checklist'))),
    ('search', lambda: (click('nav:Tools'), click('panel-tab:Search'))),
    ('files', lambda: (click('nav:Files'), click('explorer-tab:Files'))),
    ('open_docs', lambda: (click('nav:Files'), click('explorer-tab:Open'))),
    ('git_changes', lambda: (click('nav:Files'), click('explorer-tab:Git'))),
    ('agent', lambda: click('nav:Agent')),
    ('editor_find', lambda: (click('nav:Code'), cmd('Replace'))),
    ('preferences', lambda: cmd('Preferences')),
    ('syntax_colors', lambda: cmd('SyntaxColors')),
    ('about', lambda: cmd('About')),
    ('open_file', lambda: cmd('OpenFile')),
    ('branches', lambda: (click('nav:Files'), click('explorer-tab:Git'), cmd('GitBranches'))),
    ('stash', lambda: (click('nav:Files'), click('explorer-tab:Git'), cmd('GitStash'))),
    ('guide', lambda: cmd('GitGuide')),
    ('language', lambda: cmd('ChooseLanguage')),
    ('goto', lambda: cmd('GoToLine')),
    ('comment', lambda: (click('nav:Code'), cmd('AddReviewComment'))),
]
for name, action in screens:
    if only and name not in only:
        continue
    action()
    shot(name)
p.stdin.close()
```

## Runner (PowerShell): fresh profile, one process per screen

```powershell
param([string]$prefix, [string[]]$names, [string]$size = "390x844", [switch]$fresh)
# One fresh app per screen, phone-sized; screenshots via audit.py. -fresh: an empty settings folder,
# so the app starts in its sample workspace (demo git data), on its own connector endpoint.
$s = "C:\path\to\audit"
$wsl = "/mnt/c/path/to/audit"
$endpoint = "$s\audit-endpoint.json"
foreach ($n in $names) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo "C:\path\to\YourApp.exe"
    $psi.Arguments = "--window-size=$size"
    $psi.UseShellExecute = $false
    if ($fresh) {
        $appData = "$s\audit-appdata"
        if (Test-Path $appData) { Remove-Item -Recurse -Force $appData }
        New-Item -ItemType Directory $appData | Out-Null
        $psi.EnvironmentVariables["APPDATA"] = $appData
        $psi.EnvironmentVariables["EASYNOTEPAD_ENDPOINT"] = $endpoint
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Seconds 7
    if ($fresh) {
        wsl -e bash -c "cd $wsl && EASYNOTEPAD_ENDPOINT='$endpoint' WSLENV=EASYNOTEPAD_ENDPOINT python3 audit.py $prefix $n" | Out-Null
    } else {
        wsl -e bash -c "cd $wsl && python3 audit.py $prefix $n" | Out-Null
    }
    Stop-Process -Id $p.Id -Force; $p.WaitForExit(8000) | Out-Null; while (Get-Process YourApp -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 200 }
    Start-Sleep -Milliseconds 400
}
Get-ChildItem "$s\audit_${prefix}_*.png" | Select-Object Name
```

## Window capture (PowerShell, largest window of the process, PrintWindow)

```powershell
param([string]$out)
# The largest visible window of the running app (tooltips can be separate small windows).
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public class W5 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Largest(uint pid) {
    IntPtr best = IntPtr.Zero; long area = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) { RECT r; DwmGetWindowAttribute(h, 9, out r, 16);
        long a = (long)(r.R - r.L) * (r.B - r.T); if (a > area) { area = a; best = h; } }
      return true; }, IntPtr.Zero);
    return best; }
}
"@
$p = Get-Process YourApp | Select-Object -First 1
$h = [W5]::Largest([uint32]$p.Id)

$r = New-Object W5+RECT; [W5]::DwmGetWindowAttribute($h, 9, [ref]$r, 16) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [W5]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
$bmp.Save($out); "saved $($bmp.Width)x$($bmp.Height)"
```

## Before/after sheet

```powershell
param([string]$out, [string[]]$pairs)
# Before/after sheet: each pair "Title|before.png|after.png" as two phone shots under a caption.
Add-Type -AssemblyName System.Drawing
$cellW = 300; $cellH = 650; $gap = 14; $caption = 52
$cols = $pairs.Count
$c = New-Object System.Drawing.Bitmap ($cols * (2 * $cellW + $gap) + ($cols - 1) * 30 + 20), ($cellH + $caption + 20)
$g = [System.Drawing.Graphics]::FromImage($c)
$g.Clear([System.Drawing.Color]::FromArgb(245, 246, 248)); $g.InterpolationMode = 'HighQualityBicubic'; $g.TextRenderingHint = 'AntiAlias'
$titleFont = New-Object System.Drawing.Font('Segoe UI', 14, [System.Drawing.FontStyle]::Bold)
$tagFont = New-Object System.Drawing.Font('Segoe UI', 10)
$x = 10
foreach ($p in $pairs) {
    $parts = $p.Split('|')
    $g.DrawString($parts[0], $titleFont, [System.Drawing.Brushes]::Black, $x, 6)
    $i = 0
    foreach ($file in $parts[1], $parts[2]) {
        $img = [System.Drawing.Bitmap]::FromFile($file)
        $scale = [Math]::Min($cellW / $img.Width, $cellH / $img.Height)
        $w = [int]($img.Width * $scale); $h = [int]($img.Height * $scale)
        $cx = $x + $i * ($cellW + $gap)
        $g.DrawImage($img, $cx, $caption, $w, $h)
        $tag = if ($i -eq 0) { 'Before' } else { 'After' }
        $brush = if ($i -eq 0) { [System.Drawing.Brushes]::Firebrick } else { [System.Drawing.Brushes]::SeaGreen }
        $g.DrawString($tag, $tagFont, $brush, $cx, 30)
        $img.Dispose(); $i++
    }
    $x += 2 * $cellW + $gap + 30
}
$c.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); "saved $out"
```
