<#
Runs the local-agent questions (questions.json) against a running EasyNotepad, through its
automation endpoint, and grades each final answer. The app must be running with the agent set to
Local; the data folder is opened as the workspace.

  powershell -ExecutionPolicy Bypass -File tests\agent-bench\run.ps1 [-Data C:\cpp\agent-samples]
      [-Only tips-smokers,receipt-vat] [-Kind table] [-Repeat 1] [-Timeout 600]

Writes results\<time>.md (a table) and results\<time>.json (every answer, tool and plan).
#>
param(
    [string]$Data = "C:\cpp\agent-samples",
    [string[]]$Only = @(),
    [string]$Kind = "",
    [int]$Repeat = 1,
    [int]$Timeout = 600,
    [string]$Out = (Join-Path $PSScriptRoot "results")
)
$ErrorActionPreference = "Stop"

function Invoke-App([string]$Method, $Params = @{}) {
    $ep = Get-Content "$env:APPDATA\EasyNotepad\mcp-endpoint.json" -Raw | ConvertFrom-Json
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", [int]$ep.port)
    try {
        $c.ReceiveTimeout = 120000
        $s = $c.GetStream()
        $req = (@{ token = $ep.token; id = 1; method = $Method; params = $Params } | ConvertTo-Json -Compress -Depth 5) + "`n"
        $b = [Text.Encoding]::UTF8.GetBytes($req)
        $s.Write($b, 0, $b.Length)
        $line = (New-Object IO.StreamReader($s, [Text.Encoding]::UTF8)).ReadLine()
        $r = $line | ConvertFrom-Json
        if ($r.error) { throw "$Method failed: $($r.error | ConvertTo-Json -Compress)" }
        return $r.result
    } finally { $c.Close() }
}

function Get-Numbers([string]$Text) {
    foreach ($m in [regex]::Matches($Text, '-?\d[\d,]*(\.\d+)?')) {
        $v = 0.0
        if ([double]::TryParse(($m.Value -replace ',', ''), [Globalization.NumberStyles]::Float,
                               [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) { $v }
    }
}

function Test-Check($Check, [string]$Answer, [string[]]$Tools) {
    if ($null -ne $Check.number) {
        $found = @(Get-Numbers $Answer)
        foreach ($want in @($Check.number)) {
            foreach ($n in $found) { if ([math]::Abs($n - [double]$want) -le [double]$Check.tol + 1e-9) { return $true } }
        }
        return $false
    }
    if ($null -ne $Check.text) {
        foreach ($t in @($Check.text)) { if ($Answer.IndexOf($t, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true } }
        return $false
    }
    if ($null -ne $Check.tool) { return $Tools -contains $Check.tool }
    return $false
}

function Get-CheckName($Check) {
    if ($null -ne $Check.number) { return "number " + (@($Check.number) -join "/") }
    if ($null -ne $Check.text) { return "text '" + (@($Check.text) -join "'/'") + "'" }
    return "tool " + $Check.tool
}

$set = Get-Content (Join-Path $PSScriptRoot "questions.json") -Raw -Encoding UTF8 | ConvertFrom-Json
$questions = @($set.questions | Where-Object { ($Only.Count -eq 0 -or $Only -contains $_.id) -and (-not $Kind -or $_.kind -eq $Kind) })
if ($questions.Count -eq 0) { throw "No questions match." }

$state = Invoke-App "get_state"
if (($state | ConvertTo-Json -Depth 6 -Compress) -match '"modified":true') { throw "The app has unsaved documents: save or close them first." }
[void](Invoke-App "open_folder" @{ path = $Data })
Start-Sleep 2

New-Item -ItemType Directory -Force $Out | Out-Null
$stamp = Get-Date -Format "yyyy-MM-dd_HHmmss"
$begun = Get-Date
$results = @()
foreach ($round in 1..$Repeat) {
    foreach ($q in $questions) {
        [void](Invoke-App "execute_command" @{ command = "NewChat" })
        $started = Get-Date
        [void](Invoke-App "ask_agent" @{ prompt = $q.question })
        # The new chat and the question take effect on the app's next frames: wait until this question
        # is the chat's first message and the agent is done with it.
        $chat = $null
        do {
            Start-Sleep 2
            $chat = Invoke-App "read_agent_chat"
            $mine = @($chat.messages).Count -gt 0 -and @($chat.messages)[0].role -eq "user" -and @($chat.messages)[0].text -like "$($q.question)*"
        } while ((-not $mine -or $chat.busy) -and ((Get-Date) - $started).TotalSeconds -lt $Timeout)
        $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds)
        if ($chat.busy) { [void](Invoke-App "execute_command" @{ command = "StopAgent" }) }
        $messages = @($chat.messages)
        $answer = ""
        for ($i = $messages.Count - 1; $i -ge 0; --$i) { if ($messages[$i].role -eq "agent") { $answer = $messages[$i].text; break } }
        $tools = @($messages | Where-Object { $_.role -eq "tool" -and $_.text -match '^\S{1,2} [a-z_]+(\s|$)' } | ForEach-Object { ($_.text -split '\s+')[1] })
        $plan = @($messages | Where-Object { $_.role -eq "tool" -and $_.text -like "Plan*" } | ForEach-Object { $_.text })
        $failed = @($q.checks | Where-Object { -not (Test-Check $_ $answer $tools) } | ForEach-Object { Get-CheckName $_ })
        $status = (Invoke-App "get_state").status
        $ok = $failed.Count -eq 0 -and -not $chat.busy
        $results += [pscustomobject]@{
            id = $q.id; kind = $q.kind; round = $round; ok = $ok; seconds = $seconds; timedOut = [bool]$chat.busy
            tools = $tools; steps = [math]::Max(0, @($messages | Where-Object { $_.role -eq "tool" -and $_.text -like "Step *" }).Count)
            failed = $failed; answer = $answer; plan = ($plan -join "`n"); status = $status
        }
        "{0,-26} {1,-5} {2,4}s  {3}" -f $q.id, $(if ($ok) { "ok" } else { "FAIL" }), $seconds, ($failed -join "; ")
    }
}

$passed = @($results | Where-Object ok).Count
# Files the agent made in the data folder (transcripts, edits accepted): listed, not deleted.
$made = @(Get-ChildItem -Recurse -File $Data | Where-Object { $_.LastWriteTime -gt $begun } | ForEach-Object { $_.FullName })
$md = @("# Local agent questions, $stamp", "", "$passed of $($results.Count) passed. $(@($results)[-1].status)", "",
        "| Question | Kind | Result | Time | Steps | Tools | Missing |", "|---|---|---|---|---|---|---|")
foreach ($r in $results) {
    $md += "| $($r.id) | $($r.kind) | $(if ($r.ok) { 'ok' } elseif ($r.timedOut) { 'timed out' } else { 'FAIL' }) | $($r.seconds) s | $($r.steps) | $($r.tools -join ', ') | $($r.failed -join '; ') |"
}
$md | Set-Content (Join-Path $Out "$stamp.md") -Encoding UTF8
$results | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out "$stamp.json") -Encoding UTF8
""
"$passed of $($results.Count) passed. Results: $(Join-Path $Out "$stamp.md")"
if ($made.Count) { "Files the agent made in $Data (left there):"; $made }
