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
if ($notes -notmatch [regex]::Escape($binName)) {
  $notes = $notes.TrimEnd() + "`n`n- Firmware is signed (Ed25519): ``$binName`` + ``$sigName``.`n"
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
gh release view $Tag --repo $Repo 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { throw "release $Tag already exists on GitHub" }
Run "gh release create" {
  gh release create $Tag (Join-Path $out $binName) (Join-Path $out $sigName) --repo $Repo --title "k85OS $Version" --notes-file $notesOut
}
Write-Host "Released: https://github.com/$Repo/releases/tag/$Tag" -ForegroundColor Green
