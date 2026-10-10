param([string]$Voice = "")
Add-Type -AssemblyName System.Speech
$rate = 8000
$words = New-Object System.Collections.Generic.HashSet[string]
foreach ($f in Get-ChildItem main -Recurse -Include *.cpp,*.h) {
  foreach ($line in (Get-Content $f.FullName)) {
    if ($line -match 'k85_log|ESP_LOG|printf|strcmp|#include|http|/littlefs') { continue }
    foreach ($m in [regex]::Matches($line, '"([^"\\]{1,60})"')) {
      foreach ($w in [regex]::Matches($m.Groups[1].Value, '\b[A-Za-z]{2,14}\b')) { [void]$words.Add($w.Value.ToLower()) }
    }
  }
}
'abcdefghijklmnopqrstuvwxyz0123456789'.ToCharArray() | ForEach-Object { [void]$words.Add([string]$_) }
[void]$words.Add('on'); [void]$words.Add('off')

function Fnv([string]$s) {
  [uint64]$h = 2166136261
  foreach ($c in $s.ToCharArray()) {
    $h = $h -bxor [uint64][byte][char]$c
    $h = ($h * 16777619) -band [uint64]4294967295
  }
  $h
}

$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
if ($Voice) { $synth.SelectVoice($Voice) }
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo($rate, [System.Speech.AudioFormat.AudioBitsPerSample]::Eight, [System.Speech.AudioFormat.AudioChannel]::Mono)

$clips = @()
foreach ($w in $words) {
  $ms = New-Object System.IO.MemoryStream
  $synth.SetOutputToAudioStream($ms, $fmt)
  $synth.Speak($w)
  $synth.SetOutputToNull()
  $b = $ms.ToArray()
  if ($b.Length -lt 10) { continue }
  $s = 0; while ($s -lt $b.Length - 1 -and [math]::Abs([int]$b[$s] - 128) -lt 4) { $s++ }
  $e = $b.Length - 1; while ($e -gt $s -and [math]::Abs([int]$b[$e] - 128) -lt 2) { $e-- }
  $clips += [pscustomobject]@{ Hash = (Fnv $w); Data = ($b[$s..$e] + (@(128) * 480)) } # K85V:tail
}
$clips = $clips | Sort-Object Hash
$total = ($clips | ForEach-Object { $_.Data.Length } | Measure-Object -Sum).Sum
"words=$($clips.Count) size=$([math]::Round($total/1MB,2)) MB"

$fs = [IO.File]::Create("$PWD\voice.pak"); $bw = New-Object IO.BinaryWriter($fs)
$bw.Write([byte[]][char[]]'K85V'); $bw.Write([uint32]$clips.Count); $bw.Write([uint32]$rate)
$off = 12 + 12 * $clips.Count
foreach ($c in $clips) { $bw.Write([uint32]$c.Hash); $bw.Write([uint32]$off); $bw.Write([uint32]$c.Data.Length); $off += $c.Data.Length }
foreach ($c in $clips) { $bw.Write([byte[]]$c.Data) }
$bw.Close()
"voice.pak готов"