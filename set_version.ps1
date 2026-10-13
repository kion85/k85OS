param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path main)) { throw "Run from the project root (folder with main)" }
if ($Version -notmatch '^\d+\.\d+(\.\d+)?$') { throw "Version must look like 7.8 or 7.7.1 (no leading v)" }
$utf8 = New-Object System.Text.UTF8Encoding $false

# current version = the K85_FW_VERSION define in version.h
$vh = Get-ChildItem main -Recurse -Filter version.h | Select-Object -First 1
if (-not $vh) { throw "version.h not found" }
$m = [regex]::Match([IO.File]::ReadAllText($vh.FullName), '#define\s+K85_FW_VERSION\s+"([^"]+)"')
if (-not $m.Success) {
  Write-Host "K85_FW_VERSION define not found in version.h. Contents:" -ForegroundColor Yellow
  Get-Content $vh.FullName
  throw "stopping - send me this output"
}
$oldVer = $m.Groups[1].Value
if ($oldVer -eq $Version) { Write-Host "version is already $Version"; return }
Write-Host "version: $oldVer -> $Version" -ForegroundColor Cyan

# replace the old number only where it stands alone (7.7 is not touched inside 17.7 or 7.7.1)
$rx = [regex]::new('(?<![\d.])' + [regex]::Escape($oldVer) + '(?![\d.])')
foreach ($n in 'version.h', 'boot_screen.cpp', 'system_info.cpp') {
  $f = Get-ChildItem main -Recurse -Filter $n | Select-Object -First 1
  if (-not $f) { Write-Host "  ! $n not found" -ForegroundColor Yellow; continue }
  $raw = [IO.File]::ReadAllText($f.FullName)
  $sep = if ($raw -match "`r`n") { "`r`n" } else { "`n" }
  $lines = [IO.File]::ReadAllLines($f.FullName)
  $changed = 0
  for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($rx.IsMatch($lines[$i])) {
      $line2 = $rx.Replace($lines[$i], $Version)
      Write-Host ("  {0}:{1}:  {2}  ->  {3}" -f $n, ($i + 1), $lines[$i].Trim(), $line2.Trim())
      $lines[$i] = $line2
      $changed++
    }
  }
  if ($changed -gt 0) {
    [IO.File]::WriteAllText($f.FullName, (($lines -join $sep) + $sep), $utf8)
    Write-Host "patched $n ($changed line(s))" -ForegroundColor Green
  } else {
    Write-Host "$n : nothing to change"
  }
}

Write-Host ""
Write-Host "--- other places in main/ that still contain '$oldVer' (not changed, check by hand):"
Get-ChildItem main -Recurse -Include *.cpp,*.h,*.c,*.txt,*.json,*.yml -File |
  Select-String -Pattern $rx | ForEach-Object { "  $($_.Filename):$($_.LineNumber): $($_.Line.Trim())" }

Write-Host ""
Write-Host "--- how the OTA check compares versions (make sure it understands '$Version'):"
$ota = Get-ChildItem main -Recurse -Filter ota.cpp | Select-Object -First 1
if ($ota) {
  Select-String -Path $ota.FullName -Pattern 'atof|strtod|sscanf|strverscmp|compare|newer|K85_FW_VERSION' |
    Select-Object -First 12 | ForEach-Object { "  $($_.LineNumber): $($_.Line.Trim())" }
}
if ($Version -match '^\d+\.\d+\.\d+$') {
  Write-Host "  note: $Version has three parts; if the comparison above parses only two (atof, %d.%d), use a two-part version such as 7.8." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Next:" -ForegroundColor Green
Write-Host "  idf.py build flash"
Write-Host "  git add -u; git commit -m 'Bump version to $Version'; git push"
Write-Host "  powershell -ExecutionPolicy Bypass -File .\release_77.ps1 -Ver $Version -Publish   (after testing on the stick)"
