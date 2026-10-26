# listen.ps1 - UDP log listener for the PS5 title (default port 9999).
#   log_full.txt  = every line received (kept for digging later)
#   log_paste.txt = only the lines that pass -Include / -Exclude (this is the file to upload)
# Run (use listen.cmd, or this exact form - plain .\listen.ps1 is blocked by the execution policy):
#   powershell -ExecutionPolicy Bypass -File .\listen.ps1
#   .\listen.cmd -Preset perf                   # perf/prof/gc/crash lines only (no '|' needed on the command line)
#   powershell -ExecutionPolicy Bypass -File .\listen.ps1 -Include 'perf:|prof'   # custom regex (use this form, not listen.cmd, for '|')
#   .\listen.cmd -Preset lan                    # LAN debugging: net: lines + the game's own connect/serve/disconnect messages + every Minecraft LAN announcement that reaches this PC (lines start with [pc-lan])
#   .\listen.cmd -Lan                           # only the [pc-lan] part on top of the normal filter: UDP 4445 (multicast 224.0.2.60 on every adapter, broadcast, unicast), UDP 4446/25565/50000 and TCP 9999/25565 (console -> PC probes),
#                                               # and a PC -> console TCP test for every port the console starts listening on (lines '[pc-lan] PC -> PS5 ...')
#   .\listen.cmd -Preset snd                    # audio lines only (sndal, SoundSystem, OpenAL, errors)
#   .\listen.cmd -Exclude '^$'                  # no filtering at all
#   .\listen.cmd -Kill                          # first stop whatever powershell still holds the port (an old listener)
param(
  [int]$Port = 9999,
  [string]$Include = '.',
  [string]$Exclude = 'ps5-gallium|\[out\]|agc SubmitDcb|vo (IsFlipPending|WaitVblank|GetFlipStatus|SubmitFlip)|agc DcbSetFlip|RT (batch_|prepare_present|present)|\[trace\] (create_vm|jni_inner|args_|os_init|os_sysprops|sysprops|apply_ergo|set_heap_size|init_globals|universe|set_boot_path|mem commit)|oshi\.|com\.sun\.jna|\sat (java|jdk|sun)\.|Failed retrieving info',
  [string]$Dir = (Join-Path $env:USERPROFILE 'Downloads'),
  [string]$Preset = '',
  [switch]$Lan,
  [switch]$Kill
)
$ErrorActionPreference = 'Stop'

# Presets avoid '|' on the command line (cmd.exe treats it as a pipe when listen.cmd forwards the arguments).
switch ($Preset) {
  'perf' { $Include = 'pool:|slowframe|stall:|threads:|SAMPLE|loaders:|mem:|kmem|perf:|prof|gc:|g4:|LIBGL|gall|gcount|blitfix|shaders:|glstate:|crash-report:|Server thread|Saving|wily|sdl:|fence:|lib: call|gl:|sdkwait|kern:|spin 25us|slowdraw|swap #|McBoot|CRASH|PROBE|glError|ColorTest|PerfTest|fsprobe|listing|rmprobe|sndal|SoundSystem|cap #|cap frame|cap probe|cursor:|overlay:|osk:|pad:|inprobe|texcall|pngprobe|finish:|vfix:|clock:|mdbench|cpus:|mega:|display mode|SetDisplayRefresh|uncap|gputime|mega-verify|swap interval|fpslimit|net:|version:|sdkheap|A fatal error|Problematic frame|Native memory|Out of Memory|Internal Error|siginfo|SIGSEGV|SIGBUS|JRE version|hs_err'; $Exclude = '^$' }
  'mc'   { $Include = 'pool:|slowframe|stall:|threads:|SAMPLE|loaders:|mem:|kmem|g4:|LIBGL|gl: |lwjgl:|version:|gall|gcount|blitfix|shaders:|glstate:|sdl:|fence:|lib: call|McBoot|Render thread|ERROR|WARN|Exception|CRASH|PROBE|perf:|prof|gc:|fsprobe|listing|rmprobe|Crash Report|Caused by|Description:|glstate:|crash-report:|Server thread|Server-Worker|Saving|Saved|wily|Stopping|sndal|SoundSystem' }
  'snd'  { $Include = 'sndal|sndprobe|SoundSystem|OpenAL|sound|Sound|ERROR|Exception|Caused by|CRASH|McBoot'; $Exclude = 'Missing sound for event|^\[trace\] (gl|kern|sdkwait|perf):' }   # audio debugging: drops the ~700 'Missing sound' lines
  'lan'  { $Include = 'net:|lan|LAN|Started serving|Connecting|Disconnect|disconnect|Failed|Exception|Caused by|Login|login|Local game|local game|Unable to host|WARN|ERROR|McBoot|version:|kit:|launcher|jdk:|java:'; $Exclude = '^$'; $Lan = $true }   # LAN debugging: keeps the game INFO lines ([out]) that the default filter drops, and also watches UDP 4445 on this PC (see -Lan)
  'all'  { $Include = '.'; $Exclude = '^$' }
}

function Get-PortOwners([int]$p) {
  try { Get-NetUDPEndpoint -LocalPort $p -ErrorAction SilentlyContinue | ForEach-Object { Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue } | Sort-Object Id -Unique }
  catch { @() }
}

function Test-Admin {
  try { ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) } catch { $false }
}

if ($Kill) {
  foreach ($o in (Get-PortOwners $Port)) {
    if ($o.ProcessName -match '^(powershell|pwsh)$' -and $o.Id -ne $PID) {
      Write-Host "stopping old listener: $($o.ProcessName) (PID $($o.Id))"
      try { Stop-Process -Id $o.Id -Force }
      catch {
        Write-Host "  could not stop it: access denied. That window is running as Administrator (or as another user); this one is not." -ForegroundColor Yellow
        Write-Host "  Fix: close that old PowerShell window, or run this command from a PowerShell opened with 'Run as administrator'." -ForegroundColor Yellow
      }
    }
  }
  Start-Sleep -Milliseconds 500
}

try {
  $sock = New-Object System.Net.Sockets.UdpClient($Port)
} catch {
  Write-Host "Cannot listen on UDP ${Port}: it is already in use." -ForegroundColor Red
  foreach ($o in (Get-PortOwners $Port)) { Write-Host ("  held by {0} (PID {1}) - close that window, or run:  .\listen.cmd -Kill" -f $o.ProcessName, $o.Id) -ForegroundColor Yellow }
  if (-not (Test-Admin)) { Write-Host "  (this window is not elevated; if -Kill says access denied, the old listener was started as Administrator)" -ForegroundColor Yellow }
  exit 1
}
$sock.Client.ReceiveBufferSize = 16MB          # survives a paused console (QuickEdit selection) or a burst

$full = Join-Path $Dir 'log_full.txt'
$filt = Join-Path $Dir 'log_paste.txt'
$enc  = New-Object System.Text.UTF8Encoding($false)
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
$fw = New-Object System.IO.StreamWriter($full, $false, $enc); $fw.AutoFlush = $true
$pw = New-Object System.IO.StreamWriter($filt, $false, $enc); $pw.AutoFlush = $true
$lsock = $null
if ($Lan) {   # Minecraft's LAN announcements arrive on UDP 4445 (multicast 224.0.2.60); this shows which of them reach this PC, in the same stream as the console log
  try {
    $lsock = New-Object System.Net.Sockets.Socket('InterNetwork', 'Dgram', 'Udp')
    $lsock.SetSocketOption('Socket', 'ReuseAddress', $true)       # shared with Minecraft's own LAN list on this PC
    $lsock.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 4445)))
    $grp = [System.Net.IPAddress]::Parse('224.0.2.60')
    foreach ($ni in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
      if ($ni.OperationalStatus -ne 'Up') { continue }
      foreach ($ua in $ni.GetIPProperties().UnicastAddresses) {
        if ($ua.Address.AddressFamily -ne 'InterNetwork') { continue }
        try { $lsock.SetSocketOption('IP', 'AddMembership', (New-Object System.Net.Sockets.MulticastOption($grp, $ua.Address))); $m = "[pc-lan] joined 224.0.2.60 on $($ua.Address) ($($ni.Name))" }
        catch { $m = "[pc-lan] join FAILED on $($ua.Address) ($($ni.Name)): $($_.Exception.Message)" }
        Write-Host $m; $fw.WriteLine($m); $pw.WriteLine($m)
      }
    }
  } catch { Write-Host "[pc-lan] cannot listen on UDP 4445: $($_.Exception.Message)" -ForegroundColor Yellow; $lsock = $null }
}
$tl = $null
$tl2 = $null
$xudp = @{}      # extra UDP probe ports: the console sends one datagram to each from a fresh socket (net: lan: selftest UDP ...)
$seenT = @{}     # PC -> console TCP tests already done (ip:port)
if ($Lan) {   # TCP 9999 / 25565: the console connects here when it starts a LAN world (log lines "net: lan: selftest TCP ...") - shows whether console -> this PC TCP works at all
  try { $tl = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, $Port); $tl.Start() }
  catch { $m = "[pc-lan] cannot listen on TCP ${Port}: $($_.Exception.Message)"; Write-Host $m -ForegroundColor Yellow; $fw.WriteLine($m); $pw.WriteLine($m); $tl = $null }
  try { $tl2 = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, 25565); $tl2.Start() }
  catch { $m = "[pc-lan] cannot listen on TCP 25565: $($_.Exception.Message)"; Write-Host $m -ForegroundColor Yellow; $fw.WriteLine($m); $pw.WriteLine($m); $tl2 = $null }
  foreach ($xp in 4446, 25565, 50000) {
    try { $xs = New-Object System.Net.Sockets.Socket('InterNetwork', 'Dgram', 'Udp'); $xs.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $xp))); $xudp[$xp] = $xs }
    catch { $m = "[pc-lan] cannot listen on UDP ${xp}: $($_.Exception.Message)"; Write-Host $m -ForegroundColor Yellow; $fw.WriteLine($m); $pw.WriteLine($m) }
  }
}
function Test-PcToPs5([string]$ip, [int]$port, [string]$what) {   # PC -> console TCP connect, 3 s: "connected" means the console accepts inbound TCP on that port
  $c = New-Object System.Net.Sockets.TcpClient; $res = 'timeout (no answer in 3 s)'
  try {
    $ar = $c.BeginConnect($ip, $port, $null, $null)
    if ($ar.AsyncWaitHandle.WaitOne(3000)) { try { $c.EndConnect($ar); $res = 'CONNECTED (the console accepts TCP here)' } catch { $res = 'refused (the console answers, nothing listens there)' } }
  } catch { $res = "error: $($_.Exception.Message)" }
  try { $c.Close() } catch {}
  $m = "[pc-lan] {0} PC -> PS5 {1}:{2} TCP {3}: {4}" -f (Get-Date -Format HH:mm:ss.fff), $ip, $port, $what, $res
  Write-Host $m -ForegroundColor Cyan; $script:fw.WriteLine($m); $script:pw.WriteLine($m)
}
Write-Host "listening on UDP $Port  (full: $full | filtered: $filt)  Ctrl+C to stop"
$total = 0; $kept = 0
try {
  $lbuf = New-Object byte[] 2048
  $lep = [System.Net.EndPoint](New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0))
  while ($true) {
    $rl = New-Object System.Collections.ArrayList
    [void]$rl.Add($sock.Client); if ($lsock) { [void]$rl.Add($lsock) }; if ($tl) { [void]$rl.Add($tl.Server) }; if ($tl2) { [void]$rl.Add($tl2.Server) }; foreach ($xs in $xudp.Values) { [void]$rl.Add($xs) }
    [System.Net.Sockets.Socket]::Select($rl, $null, $null, 500000)   # 0.5 s so Ctrl+C works; $rl keeps only the sockets with data
    foreach ($rs in $rl) {
      $tsrv = $null
      if ($tl -and [object]::ReferenceEquals($rs, $tl.Server)) { $tsrv = $tl } elseif ($tl2 -and [object]::ReferenceEquals($rs, $tl2.Server)) { $tsrv = $tl2 }
      if ($tsrv) {
        $tc = $tsrv.AcceptTcpClient(); $m = "[pc-lan] {0} TCP connection to port {1} from {2}" -f (Get-Date -Format HH:mm:ss.fff), $tsrv.LocalEndpoint.Port, $tc.Client.RemoteEndPoint; $tc.Close()
        $total++; $kept++; $fw.WriteLine($m); $pw.WriteLine($m); Write-Host $m -ForegroundColor Cyan
        continue
      }
      $xp = $null
      foreach ($k in @($xudp.Keys)) { if ([object]::ReferenceEquals($rs, $xudp[$k])) { $xp = $k } }
      if ($xp) {
        $n = $xudp[$xp].ReceiveFrom($lbuf, [ref]$lep)
        $m = "[pc-lan] {0} UDP {1} from {2}: {3}" -f (Get-Date -Format HH:mm:ss.fff), $xp, $lep, $enc.GetString($lbuf, 0, $n)
        $total++; $kept++; $fw.WriteLine($m); $pw.WriteLine($m); Write-Host $m -ForegroundColor Cyan
        continue
      }
      if ($lsock -and [object]::ReferenceEquals($rs, $lsock)) {
        $n = $lsock.ReceiveFrom($lbuf, [ref]$lep)
        $m = "[pc-lan] {0} from {1} {2} bytes {3}" -f (Get-Date -Format HH:mm:ss.fff), $lep, $n, $enc.GetString($lbuf, 0, $n)
        $total++; $kept++; $fw.WriteLine($m); $pw.WriteLine($m); Write-Host $m -ForegroundColor Cyan
        continue
      }
      $bytes = $sock.Receive([ref]$ep)
      $text = $enc.GetString($bytes)
      foreach ($line in ($text -split "`r?`n")) {
        if ($line -eq '') { continue }
        $total++
        $fw.WriteLine($line)
        if ($line -match $Include -and $line -notmatch $Exclude) { $kept++; $pw.WriteLine($line); Write-Host $line }
        if ($Lan -and ($line -match 'net: listen fd \d+ backlog \d+ on port (\d+)' -or $line -match 'net: bind: TCP socket fd \d+, port 0 -> (\d+)')) {   # the console started listening: can this PC connect to it?
          $pp = [int]$Matches[1]; $pip = $ep.Address.ToString()
          if ($pp -gt 0 -and -not $seenT.ContainsKey("${pip}:$pp")) {
            $seenT["${pip}:$pp"] = 1
            if (-not $seenT.ContainsKey("${pip}:2121")) { $seenT["${pip}:2121"] = 1; Test-PcToPs5 $pip 2121 '(FTP, known good)' }
            Test-PcToPs5 $pip $pp '(port the game listens on)'
          }
        }
      }
    }
  }
} finally {
  if ($fw) { $fw.Dispose() }
  if ($pw) { $pw.Dispose() }
  if ($sock) { $sock.Close() }
  if ($lsock) { $lsock.Close() }
  if ($tl) { $tl.Stop() }
  if ($tl2) { $tl2.Stop() }
  foreach ($xs in $xudp.Values) { $xs.Close() }
  Write-Host "stopped: $total lines received, $kept kept -> $filt"
}
