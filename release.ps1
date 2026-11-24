param(
  [Parameter(Mandatory = $true)][string]$Version,
  [Parameter(Mandatory = $true)][string]$NotesFile,
  [switch]$Publish,
  [string]$Repo = 'kion85/k85OS'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path main)) { throw "Run from the project root (folder with main)" }
if ($Version -notmatch '^\d+\.\d+(\.\d+)?$') { throw "Version must look like 7.8 or 7.7.1 (no leading v)" }
$utf8 = New-Object System.Text.UTF8Encoding $false
$Tag = "v$Version"

function Run($what, [scriptblock]$cmd) {
  & $cmd
  if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# ---------- notes ----------
if (-not (Test-Path $NotesFile)) { throw "notes file not found: $NotesFile" }
$notes = [IO.File]::ReadAllText((Resolve-Path $NotesFile).Path)
if ($notes.Trim().Length -lt 20) { throw "notes file is empty" }

# ---------- version in the source must match ----------
$vh = Get-ChildItem main -Recurse -Filter version.h | Select-Object -First 1
$m = [regex]::Match([IO.File]::ReadAllText($vh.FullName), '#define\s+K85_FW_VERSION\s+"([^"]+)"')
if (-not $m.Success -or $m.Groups[1].Value -ne $Version) {
  throw "version.h says '$($m.Groups[1].Value)', expected $Version  (run set_version.ps1 $Version first)"
}
Write-Host "version.h = $Version" -ForegroundColor Green

# K85REL1: everything that can stop a release is checked BEFORE the long build
if ($Publish) {
  $k85eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  gh auth status 2>&1 | Out-Null
  $k85auth = $LASTEXITCODE
  $ErrorActionPreference = $k85eap
  if ($k85auth -ne 0) { throw 'gh is not logged in (run: gh auth login)' }
  $k85dirty = git status --porcelain --untracked-files=no
  if ($LASTEXITCODE -ne 0) { throw 'git status failed' }
  if ($k85dirty) { Write-Host $k85dirty; throw 'uncommitted changes in tracked files - commit them first, so the release matches the source' }
  $k85new = git status --porcelain --untracked-files=all -- main bootloader_components CMakeLists.txt partitions.csv sdkconfig.defaults
  if ($LASTEXITCODE -ne 0) { throw 'git status failed' }
  if ($k85new) { Write-Host $k85new; throw 'new files that are part of the build are not in git: git add them (or put them in .gitignore)' }
}
# ---------- build ----------
Write-Host "building..." -ForegroundColor Cyan
Run "idf.py build" { idf.py build }
$bin = Join-Path $PWD 'build\k85os_idf.bin'
if (-not (Test-Path $bin)) { throw "build\k85os_idf.bin not found" }

# ---------- sign ----------
$start = Get-Date
& python sign_release.py $bin
if ($LASTEXITCODE -ne 0) {
  Write-Host "sign_release.py failed. Its first lines (usage):" -ForegroundColor Yellow
  Get-Content sign_release.py -TotalCount 40
  throw "signing failed"
}
$sig = "$bin.sig"
if (-not (Test-Path $sig) -or (Get-Item $sig).LastWriteTime -lt $start.AddSeconds(-2)) {
  $found = Get-ChildItem . -Recurse -Filter '*.sig' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $start.AddSeconds(-2) -and $_.FullName -notmatch 'managed_components|\\release_out\\' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $found) { throw "no fresh .sig file found after signing" }
  $sig = $found.FullName
}
Write-Host "signature: $sig" -ForegroundColor Green

# ---------- package ----------
$binName = "k85OS_$Version.bin"
$sigName = "$binName.sig"
$out = Join-Path $PWD "release_out\$Version"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $bin (Join-Path $out $binName) -Force
Copy-Item $sig (Join-Path $out $sigName) -Force

# K85FULL1: a complete flash image (bootloader + partition table + otadata + app) for M5Burner / esptool at address 0x0.
# The signed OTA file above is the application only: flashed at 0x0 it does not boot (black screen).
$fullName = "k85OS_${Version}_full.bin"
$fullPath = Join-Path $out $fullName
$bl  = Join-Path $PWD 'build\bootloader\bootloader.bin'
$ptb = Join-Path $PWD 'build\partition_table\partition-table.bin'
$otd = Join-Path $PWD 'build\ota_data_initial.bin'
foreach ($k85f in @($bl, $ptb, $otd)) { if (-not (Test-Path $k85f)) { throw "missing $k85f - the full image cannot be built" } }
Run "esptool merge_bin" { python -m esptool --chip esp32s3 merge_bin -o $fullPath --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 $bl 0x8000 $ptb 0x10000 $otd 0x20000 $bin }
# self-check: bootloader at 0x0, partition table at 0x8000, and the application inside is byte-for-byte the signed one
$k85fs = [IO.File]::OpenRead($fullPath)
try {
  $k85b = New-Object byte[] 1; [void]$k85fs.Read($k85b, 0, 1)
  $k85p = New-Object byte[] 2; $k85fs.Position = 0x8000; [void]$k85fs.Read($k85p, 0, 2)
  $k85n = (Get-Item $bin).Length
  $k85a = New-Object byte[] $k85n; $k85fs.Position = 0x20000
  $k85got = 0
  while ($k85got -lt $k85n) { $k85r = $k85fs.Read($k85a, $k85got, $k85n - $k85got); if ($k85r -le 0) { break }; $k85got += $k85r }
} finally { $k85fs.Close() }
$k85sha = [System.Security.Cryptography.SHA256]::Create()
$k85hash = [BitConverter]::ToString($k85sha.ComputeHash($k85a, 0, $k85got)) -replace '-', ''
if ($k85b[0] -ne 0xE9 -or $k85p[0] -ne 0xAA -or $k85p[1] -ne 0x50 -or $k85got -ne $k85n -or $k85hash -ne (Get-FileHash $bin -Algorithm SHA256).Hash) { throw "the full image failed its self-check" }
Write-Host "full image: $fullName  ($((Get-Item $fullPath).Length) bytes, self-check ok)" -ForegroundColor Green
if ($notes -notmatch [regex]::Escape($binName)) {
  $notes = $notes.TrimEnd() + "`n`n- Firmware is signed (Ed25519): ``$binName`` + ``$sigName``.`n"
}
if ($notes -notmatch [regex]::Escape($fullName)) {
  $notes = $notes.TrimEnd() + "`n`n- Full flash image for M5Burner / esptool, written at address 0x0 (bootloader + partition table + app): ``$fullName``. ``$binName`` is the OTA update (application only) - do not flash it at 0x0.`n"
}
$notesOut = Join-Path $out 'notes.md'
[IO.File]::WriteAllText($notesOut, $notes, $utf8)
Write-Host ""
Write-Host "Ready in $out :" -ForegroundColor Green
Get-ChildItem $out | Select-Object Name, Length

if (-not $Publish) {
  Write-Host ""
  Write-Host "Dry run finished. Flash and test the stick, commit your changes, then publish with:" -ForegroundColor Cyan
  Write-Host "  powershell -ExecutionPolicy Bypass -File .\release.ps1 -Version $Version -NotesFile $NotesFile -Publish"
  return
}

# ---------- publish ----------
Write-Host ""
Write-Host "publishing $Tag ..." -ForegroundColor Cyan
$dirty = git status --porcelain --untracked-files=no
if ($dirty) {
  Write-Host $dirty
  throw "uncommitted changes in tracked files - commit them first, so the release matches the source"
}
Run "git push" { git push }
$k85eap2 = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
$k85view = (gh release view $Tag --repo $Repo 2>&1 | Out-String)
$k85code = $LASTEXITCODE
$ErrorActionPreference = $k85eap2
if ($k85code -eq 0) { throw "release $Tag already exists on GitHub" }
if ($k85view -notmatch 'not found') { throw "cannot tell whether release $Tag exists (network or login problem?): $k85view" }
Run "gh release create" {
  gh release create $Tag (Join-Path $out $binName) (Join-Path $out $sigName) (Join-Path $out $fullName) --repo $Repo --title "k85OS $Version" --notes-file $notesOut
}
Write-Host "Released: https://github.com/$Repo/releases/tag/$Tag" -ForegroundColor Green
