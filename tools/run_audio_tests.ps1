# Runs the offline audio test matrix and prints objective metrics.
#   powershell -ExecutionPolicy Bypass -File tools\run_audio_tests.ps1 [-Cmds "set gate threshold 0.4;set gate hold 200"]
# Prerequisites: build (mic-thingy-file, vf-eval) and tools\make_testset.ps1.
# -Cmds: extra console commands (';'-separated) applied to every run, to compare settings.
param(
    [string]$Build = (Join-Path $PSScriptRoot '..\build'),
    [string]$Cmds = '',
    [string]$Tag = 'run'
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
$T = Join-Path $Build 'audiotest'
$O = Join-Path $T "out_$Tag"
New-Item -ItemType Directory -Force $O | Out-Null
$vf = Join-Path $Build 'mic-thingy-file.exe'
$ev = Join-Path $Build 'vf-eval.exe'
if (-not (Test-Path "$T\speech_pl.wav")) { throw 'run tools\make_testset.ps1 first' }
# Windows PowerShell turns native stderr into terminating errors under 'Stop'.
$ErrorActionPreference = 'Continue'

$extra = @()
foreach ($c in ($Cmds -split ';' | Where-Object { $_.Trim() -ne '' })) { $extra += '-c'; $extra += $c.Trim() }

# "denoise" = what the noise metrics are meant for (dynamics off, they distort SNR math);
# "full"    = the whole default chain.
$profiles = [ordered]@{
    denoise = @('-c', 'bypass comp on', '-c', 'bypass eq on', '-c', 'bypass limiter on')
    full    = @()
}
$noises = 'pink', 'white', 'brown', 'hum', 'fan', 'keyboard', 'babble'
$snrs = 0, 5, 10, 20

$csv = @()
foreach ($pn in $profiles.Keys) {
    Write-Host "`n=== profile: $pn $(if ($Cmds) { "+ $Cmds" }) ==="
    # clean speech alone first: what does the chain do to a voice with no noise at all?
    # (compared with the raw voice: level change, onsets/offsets lost to the gate/denoiser)
    $refp = "$O\$pn-clean.wav"
    & $vf -i "$T\speech_pl.wav" -o $refp @($profiles[$pn]) @extra 2>$null
    $r = & $ev eval "$T\speech_pl.wav" "$T\speech_pl.wav" $refp "$pn-clean"
    $r | Where-Object { $_ -notmatch '^CSV,' } | Write-Host
    $csv += ($r | Where-Object { $_ -match '^CSV,' }) -replace '^CSV,', ''
    # noisy runs are compared with that processed clean speech ($refp)
    foreach ($n in $noises) {
        foreach ($s in $snrs) {
            if ($pn -eq 'full' -and $s -ne 10) { continue }       # full chain: one SNR is enough
            $name = "$pn-$n-${s}dB"
            $mix = "$O\mix_$n-${s}dB.wav"
            if (-not (Test-Path $mix)) { & $ev mix "$T\speech_pl.wav" "$T\noise_$n.wav" $s $mix | Out-Null }
            & $vf -i $mix -o "$O\$name.wav" @($profiles[$pn]) @extra 2>$null
            $r = & $ev eval "$T\speech_pl.wav" $mix "$O\$name.wav" $name $refp
            $r | Where-Object { $_ -notmatch '^CSV,' } | Write-Host
            $csv += ($r | Where-Object { $_ -match '^CSV,' }) -replace '^CSV,', ''
        }
    }
}

Write-Host "`n=== real recordings (no clean reference, full chain) ==="
foreach ($f in (Get-ChildItem "$T\real_*.wav" -ErrorAction SilentlyContinue)) {
    $name = "full-$($f.BaseName)"
    & $vf -i $f.FullName -o "$O\$name.wav" --debug "$O\$name.dbg.wav" --csv "$O\$name.csv" @extra 2>$null
    & $ev noref $f.FullName "$O\$name.wav" $name | Write-Host
}

$hdr = 'name,delay_ms,corr,snr_in,sdr_out,pause_in_dbfs,pause_out_dbfs,pause_atten_db,speech_gain_db,onset_db,onset_worst_db,offset_db,offset_worst_db,peak,clip,nan,active_frames,pause_frames,onsets,offsets'
@($hdr) + $csv | Set-Content -Encoding UTF8 "$O\results.csv"
Write-Host "`nCSV: $O\results.csv"
