# Builds the audio test set in build/audiotest (Windows: System.Speech TTS + ffmpeg).
#   powershell -ExecutionPolicy Bypass -File tools\make_testset.ps1 [-Recordings "a.mkv@47.8;b.mkv@20.6"]
# -Recordings: real recordings to include as "no reference" material; "@N" = start second of the
# unfiltered (raw microphone) part. Generated audio stays out of git (build/ is ignored).
param(
    [string]$Out = (Join-Path $PSScriptRoot '..\build\audiotest'),
    [string]$Recordings = ''      # "a.mkv@47.8;b.mkv@20.6" (';'-separated, -File can't pass arrays)
)
$ErrorActionPreference = 'Stop'
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw 'ffmpeg not found (winget install Gyan.FFmpeg)' }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

Add-Type -AssemblyName System.Speech
function Speak([string]$textFile, [string]$culture, [string]$wav, [int]$rate) {
    $s = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $voice = $s.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Culture.Name -eq $culture } | Select-Object -First 1
    if (-not $voice) { throw "no TTS voice for $culture" }
    $s.SelectVoice($voice.VoiceInfo.Name)
    $s.Rate = $rate
    $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
    $s.SetOutputToWaveFile($wav, $fmt)
    $pb = New-Object System.Speech.Synthesis.PromptBuilder
    $pb.AppendBreak([TimeSpan]::FromMilliseconds(700))
    foreach ($line in (Get-Content -Encoding UTF8 $textFile)) {
        if ($line.Trim() -eq '') { continue }
        $pb.AppendText($line)
        $pb.AppendBreak([TimeSpan]::FromMilliseconds(900))
    }
    $s.Speak($pb)
    $s.Dispose()
}

$data = Join-Path $PSScriptRoot 'testdata'
Write-Host 'TTS: speech_pl.wav (target), babble_en.wav (background talker)'
Speak (Join-Path $data 'sentences_pl.txt') 'pl-PL' "$Out\speech_pl.wav" 0
Speak (Join-Path $data 'sentences_en.txt') 'en-US' "$Out\babble_en.wav" 1

$len = 60
function Gen([string]$name, [string]$filter) {
    & ffmpeg -v error -y -f lavfi -i $filter -t $len -ac 1 -ar 48000 -c:a pcm_f32le "$Out\noise_$name.wav"
    if ($LASTEXITCODE) { throw "ffmpeg failed for $name" }
}
Write-Host 'Noises: pink, white, brown, hum, fan, keyboard'
Gen 'pink'  "anoisesrc=d=${len}:c=pink:r=48000:a=0.3:s=1"
Gen 'white' "anoisesrc=d=${len}:c=white:r=48000:a=0.2:s=2"
Gen 'brown' "anoisesrc=d=${len}:c=brown:r=48000:a=0.5:s=3"
# mains hum: 50 Hz + odd harmonics
Gen 'hum'   "aevalsrc=0.3*sin(2*PI*50*t)+0.15*sin(2*PI*150*t)+0.08*sin(2*PI*250*t)+0.04*sin(2*PI*350*t):s=48000:d=$len"
# fan: low-passed pink noise with a slow wobble
& ffmpeg -v error -y -f lavfi -i "anoisesrc=d=${len}:c=pink:r=48000:a=0.4:s=4" -af "lowpass=f=900,tremolo=f=0.3:d=0.2" -ac 1 -c:a pcm_f32le "$Out\noise_fan.wav"
# keyboard: random impulses (~8 per second) through a resonant band-pass = clicky taps
& ffmpeg -v error -y -f lavfi -i "aevalsrc='if(lt(random(1),0.00017),1,0)':s=48000:d=$len" -af "bandpass=f=3000:w=1500,highpass=f=800,volume=6" -ac 1 -c:a pcm_f32le "$Out\noise_keyboard.wav"
if ($LASTEXITCODE) { throw 'ffmpeg failed for keyboard' }
# background talker: loop the English TTS to the noise length
& ffmpeg -v error -y -stream_loop -1 -i "$Out\babble_en.wav" -t $len -ac 1 -ar 48000 -c:a pcm_f32le "$Out\noise_babble.wav"

$i = 0
foreach ($r in ($Recordings -split ';' | Where-Object { $_.Trim() -ne '' })) {
    $i++
    $parts = $r -split '@'
    $start = if ($parts.Count -gt 1) { $parts[1] } else { '0' }
    Write-Host "Real recording $i : $($parts[0]) from ${start}s"
    & ffmpeg -v error -y -ss $start -i $parts[0] -vn -ac 1 -ar 48000 -c:a pcm_f32le "$Out\real_$i.wav"
}
Write-Host "Test set ready in $Out"
