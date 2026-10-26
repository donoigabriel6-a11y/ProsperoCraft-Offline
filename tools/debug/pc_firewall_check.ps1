# pc_firewall_check.ps1 - run in an ADMINISTRATOR PowerShell on the PC (not through listen.cmd):
#   powershell -ExecutionPolicy Bypass -File .\pc_firewall_check.ps1            # find out what this PC drops from the console
#   powershell -ExecutionPolicy Bypass -File .\pc_firewall_check.ps1 -Fix       # same, then allow everything from the console (one inbound rule limited to its IP)
# What it does: shows the network profile and the inbound rules for Java/PowerShell, switches on Windows' "dropped packet" audit, waits while you test on the console
# (open a world to LAN, join the PC's world), then lists every packet from the console that Windows dropped, by protocol and port. The audit is switched off again at the end.
param(
  [string]$Ps5 = '172.16.1.251',
  [switch]$Fix
)
$ErrorActionPreference = 'Continue'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Write-Host "Run this from a PowerShell opened with 'Run as administrator' (the packet-drop audit and the Security log need it)." -ForegroundColor Red; exit 1 }

Write-Host "== network profile (Public blocks most inbound traffic unless a rule says otherwise) ==" -ForegroundColor Cyan
Get-NetConnectionProfile | ForEach-Object { "{0,-28} {1,-10} {2}" -f $_.InterfaceAlias, $_.NetworkCategory, $_.Name }
Write-Host "`n== firewall profiles ==" -ForegroundColor Cyan
Get-NetFirewallProfile | ForEach-Object { "{0,-8} enabled={1} inbound default={2}" -f $_.Name, $_.Enabled, $_.DefaultInboundAction }
Write-Host "`n== inbound rules for Java / PowerShell / port 9999 / 4445 (enabled) ==" -ForegroundColor Cyan
$seen = @{}
foreach ($f in (Get-NetFirewallApplicationFilter | Where-Object { $_.Program -match 'java|powershell|pwsh' })) {
  $r = $f | Get-NetFirewallRule
  if ($r.Direction -eq 'Inbound' -and $r.Enabled -eq 'True' -and -not $seen[$r.Name]) { $seen[$r.Name] = 1
    $pf = $r | Get-NetFirewallPortFilter
    "{0,-5} {1,-7} profile={2,-14} proto={3,-5} port={4,-8} {5}  [{6}]" -f $r.Action, $r.Direction, $r.Profile, $pf.Protocol, $pf.LocalPort, $r.DisplayName, $f.Program }
}
foreach ($pf in (Get-NetFirewallPortFilter | Where-Object { $_.LocalPort -match '^(9999|4445)$' })) {
  $r = $pf | Get-NetFirewallRule
  if ($r.Direction -eq 'Inbound' -and $r.Enabled -eq 'True' -and -not $seen[$r.Name]) { $seen[$r.Name] = 1
    "{0,-5} {1,-7} profile={2,-14} proto={3,-5} port={4,-8} {5}" -f $r.Action, $r.Direction, $r.Profile, $pf.Protocol, $pf.LocalPort, $r.DisplayName }
}

# Filtering Platform Packet Drop + Filtering Platform Connection (GUIDs, so this works in any Windows language)
$guids = '{0CCE9225-69AE-11D9-BED3-505054503030}', '{0CCE9226-69AE-11D9-BED3-505054503030}'
foreach ($g in $guids) { auditpol /set /subcategory:"$g" /failure:enable | Out-Null }
$t0 = Get-Date
try {
  Write-Host "`nAudit is on. NOW on the console: open a world to LAN, wait ~20 s, then (in a second round) join the PC's LAN world." -ForegroundColor Yellow
  [void](Read-Host "Press Enter here when you are done testing")
} finally {
  foreach ($g in $guids) { auditpol /set /subcategory:"$g" /failure:disable | Out-Null }   # back to the Windows default (no auditing)
}

Write-Host "`n== packets from $Ps5 that Windows dropped since $($t0.ToString('HH:mm:ss')) ==" -ForegroundColor Cyan
$ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Security'; Id = 5152, 5157; StartTime = $t0 } -ErrorAction SilentlyContinue)
$rows = foreach ($e in $ev) {
  $m = $e.Message
  if ($m -notmatch "Source Address:\s+$([regex]::Escape($Ps5))\b") { continue }
  $proto = if ($m -match 'Protocol:\s+(\d+)') { switch ($Matches[1]) { '6' { 'TCP' } '17' { 'UDP' } '1' { 'ICMP' } default { "proto $($Matches[1])" } } } else { '?' }
  $dst = if ($m -match 'Destination Address:\s+(\S+)') { $Matches[1] } else { '?' }
  $dp  = if ($m -match 'Destination Port:\s+(\d+)') { $Matches[1] } else { '' }
  $app = if ($m -match 'Application Name:\s+(.+)') { $Matches[1].Trim() } else { '' }
  [pscustomobject]@{ Key = "$proto -> ${dst}:$dp"; App = $app }
}
$rows = @($rows)
if ($rows.Count -eq 0) {
  Write-Host "Nothing from $Ps5 was dropped by Windows Firewall in that time. If the console still cannot reach this PC, the block is not in this PC's firewall (router / access point / the console itself)." -ForegroundColor Green
} else {
  $rows | Group-Object Key | Sort-Object Count -Descending | ForEach-Object { "{0,5} x  {1}   {2}" -f $_.Count, $_.Name, (($_.Group | Where-Object { $_.App } | Select-Object -First 1).App) }
  Write-Host "`nWindows Firewall is dropping these. Allow the console with:" -ForegroundColor Yellow
  Write-Host "  New-NetFirewallRule -DisplayName 'PS5 console (LAN)' -Direction Inbound -Action Allow -RemoteAddress $Ps5 -Profile Any" -ForegroundColor White
  if ($Fix) {
    New-NetFirewallRule -DisplayName 'PS5 console (LAN)' -Direction Inbound -Action Allow -RemoteAddress $Ps5 -Profile Any | Out-Null
    Write-Host "rule 'PS5 console (LAN)' created (inbound, only from $Ps5). Test again." -ForegroundColor Green
  }
}
