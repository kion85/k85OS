param(
  [switch]$Publish,
  [string]$Ver = '7.7',
  [string]$Repo = 'kion85/k85OS'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path main)) { throw "Run from the project root (folder with main)" }
$utf8 = New-Object System.Text.UTF8Encoding $false
$Tag = "v$Ver"
$out = Join-Path $PWD 'release_out'

function Run($what, [scriptblock]$cmd) {
  & $cmd
  if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# ---------- 1. five more screensaver phrases ----------
$ls = Get-ChildItem main -Recurse -Filter lock_screen.cpp | Select-Object -First 1
if (-not $ls) { throw "lock_screen.cpp not found" }
$t = [IO.File]::ReadAllText($ls.FullName)
if ($t.Contains('"One step at a time"')) {
  Write-Host "phrases: already added"
} else {
  if (-not $t.Contains('"You matter",')) { throw "phrases array not found (was patch_screensaver.ps1 applied?)" }
  $nl = if ($t -match "`r`n") { "`r`n" } else { "`n" }
  $add = '"You matter",' + $nl +
         '    "You are stronger than you think", "Rest is not a waste of time", "You are enough, just as you are",' + $nl +
         '    "Good things are coming", "One step at a time",'
  $b = "$($ls.FullName).rel.bak"
  if (-not (Test-Path $b)) { Copy-Item $ls.FullName $b }
  [IO.File]::WriteAllText($ls.FullName, $t.Replace('"You matter",', $add), $utf8)
  Write-Host "phrases: 5 added (15 total)" -ForegroundColor Green
}

# ---------- 2. version check ----------
$vh = Get-ChildItem main -Recurse -Filter version.h | Select-Object -First 1
$m = [regex]::Match([IO.File]::ReadAllText($vh.FullName), '#define\s+K85_FW_VERSION\s+"([^"]+)"')
if (-not $m.Success -or $m.Groups[1].Value -ne $Ver) { throw "version.h says '$($m.Groups[1].Value)', expected $Ver" }
Write-Host "version.h = $Ver" -ForegroundColor Green

# ---------- 3. build ----------
Write-Host "building..." -ForegroundColor Cyan
Run "idf.py build" { idf.py build }
$bin = Join-Path $PWD 'build\k85os_idf.bin'
if (-not (Test-Path $bin)) { throw "build\k85os_idf.bin not found" }

# ---------- 4. asset names (same pattern as the previous release) ----------
$names = @()
try { $names = @(gh release view v7.5 --repo $Repo --json assets --jq '.assets[].name') } catch { $names = @() }
$binName = $names | Where-Object { $_ -match '\.bin$' } | Select-Object -First 1
$sigName = $names | Where-Object { $_ -match '\.sig$' } | Select-Object -First 1
$binName = if ($binName) { $binName.Replace('7.5', $Ver) } else { "k85OS_$Ver.bin" }
$sigName = if ($sigName) { $sigName.Replace('7.5', $Ver) } else { "$binName.sig" }
Write-Host "assets: $binName , $sigName"

# ---------- 5. sign ----------
$start = Get-Date
& python sign_release.py $bin
if ($LASTEXITCODE -ne 0) {
  Write-Host "sign_release.py failed. Its first lines (usage):" -ForegroundColor Yellow
  Get-Content sign_release.py -TotalCount 40
  throw "signing failed - send me this output"
}
$sig = Get-ChildItem . -Recurse -Filter '*.sig' -File -ErrorAction SilentlyContinue |
  Where-Object { $_.LastWriteTime -ge $start.AddSeconds(-2) -and $_.FullName -notmatch 'managed_components|\\release_out\\' } |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $sig) { throw "no fresh .sig file found after signing - tell me what sign_release.py printed above" }
Write-Host "signature: $($sig.FullName)" -ForegroundColor Green

# ---------- 6. package + release notes ----------
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $bin (Join-Path $out $binName) -Force
Copy-Item $sig.FullName (Join-Path $out $sigName) -Force

$notes = @"
## k85OS $Ver

### Added
- **Voice assistant mode (screen reader)** for a broken or hard-to-read display. Hold **A** on the lock screen for 2 seconds: the stick says "on" / "off". While it is on, it speaks menu items as you scroll (all list menus, main menu, quick settings, BIOS), reads on-screen messages, and sets the volume to 55%.
- **Voice pack**: about 830 words (3.5 MB), downloaded automatically over WiFi into PSRAM after boot (release ``voice-1``). A green "Voice pack ready" screen shows when it is loaded.
- **New lock-screen screensaver**: a soft bell lullaby and 15 rotating kind messages ("You are a good person", "Everything will be okay", ...); the flying k85os letters stay clear of the text. Bells are rendered once and cached, so the animation never stutters.

### Removed
- **SSH server and SSH client** (Settings -> SSH Server, Services -> SSH Server, Tools -> SSH Connect). Web Terminal, Terminal and FTP are unchanged. This frees internal RAM.

### Fixed
- Mic Test no longer fights the speaker over the shared I2S line (loud noise when voice mode was on); the speaker is released and restored.
- HTTPS downloads from GitHub no longer fail with "Out of buffer" (larger request buffer), and TLS uses software AES to avoid "esp-aes: Failed to allocate memory" on low internal RAM.

### Notes
- The voice pack lives in RAM, so it is downloaded again after every boot (saved WiFi is required). Without WiFi the voice mode answers with a low beep.
- Menu entries after the removed SSH items moved up by one position.
- Firmware is signed (Ed25519): ``$binName`` + ``$sigName``.
"@
[IO.File]::WriteAllText((Join-Path $out "notes_$Ver.md"), $notes, $utf8)
Write-Host ""
Write-Host "Ready in $out :" -ForegroundColor Green
Get-ChildItem $out | Select-Object Name, Length

if (-not $Publish) {
  Write-Host ""
  Write-Host "Dry run finished. Flash and test the stick, then publish with:" -ForegroundColor Cyan
  Write-Host "  powershell -ExecutionPolicy Bypass -File .\release_77.ps1 -Publish"
  return
}

# ---------- 7. commit, push, release ----------
Write-Host ""
Write-Host "publishing $Tag ..." -ForegroundColor Cyan
Run "git add -u" { git add -u }
$newFiles = @('main/system/voice.h', 'main/system/voice.cpp', 'make_voice_pak.ps1') | Where-Object { Test-Path $_ }
if ($newFiles) { Run "git add" { git add @newFiles } }
git diff --cached --quiet
if ($LASTEXITCODE -ne 0) {
  Run "git commit" { git commit -m "k85OS ${Ver}: voice assistant mode, calm screensaver, SSH removed" }
} else { Write-Host "nothing new to commit" }
Run "git push" { git push }
Run "gh release create" {
  gh release create $Tag (Join-Path $out $binName) (Join-Path $out $sigName) --repo $Repo --title "k85OS $Ver" --notes-file (Join-Path $out "notes_$Ver.md")
}
Write-Host "Released: https://github.com/$Repo/releases/tag/$Tag" -ForegroundColor Green
