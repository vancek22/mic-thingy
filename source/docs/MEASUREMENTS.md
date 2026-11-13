# Measurements (2026-11-13)

One machine (Windows 11, AMD Radeon RX 7700 XT, Logitech PRO X and EPOS headsets, VB-Cable), Release build with
MinGW GCC 16. Numbers are from this machine only; compare configurations with each other, not with other computers.
Latency numbers have a +-8 ms run-to-run spread (start-up alignment of the audio streams).

## Goals from the plan and where they ended up

| Goal | Result | Verdict |
|---|---|---|
| DSP delay below 10 ms | 11.5 ms (default, quality mode) -> **0.2 ms** in the "Low latency" preset | met in the preset, not by default |
| Microphone to headphones below 10 ms | ~44 ms default, **~32 ms** with the low-latency preset (10 ms capture period + 22 ms WASAPI playback buffer of the headset) | not met; the rest is Windows audio |
| Microphone to a call through VB-Cable | device loop 69-74 ms (of which ~21 ms is mic thingy's buffers, ~48-52 ms VB-Cable + Windows) + DSP | not met; needs the driver (not built) |
| Memory below 40 MB | tray with the filter running **2.8 MB** private; window open **~91 MB** | met in the tray, not with the window open |

## CPU of the effect chain (`vf-bench`, speech-like signal, best of 5 x 2000 frames, per 10 ms frame)

| Configuration | us / frame | % of one core |
|---|---|---|
| default chain (highpass, RNNoise, gate, compressor, EQ, limiter) | 155 | 1.55 |
| default + low-latency mode | 212 | 2.12 |
| + hum / dereverb / breath / guard / agc / deesser / warmth | 158-164 | 1.58-1.64 each |
| + voice (pitch +4 semitones) | 165 | 1.65 |
| + echo canceller (2048-tap NLMS) | 765 | 7.65 |
| everything on | 789 | 7.89 |

(An earlier figure of 78 us/frame came from a file of hum noise, where RNNoise skips the network on silent frames;
speech needs the full network.) The real-time budget is 10 000 us per frame.

## Memory and CPU of the program (`gui_measure.ps1`: private working set, CPU over 10 s after 4 s settling)

| Scenario | private WS | CPU (of one core) |
|---|---|---|
| tray, engine running | 2.8 MB | 0.6-0.9 % |
| tray + 2 soundboard clips of 14 s | 5.4 MB (+2.6 MB = the clips as 16-bit) | 0.6 % |
| tray + in-game indicator + system loopback | 3.0 MB | 0.9 % |
| window open, Basic tab | 91 MB | 3.8 % (20 fps unfocused, 60 fps focused) |
| window open, Tools tab (spectrum analyser) | 91 MB | 6 % |
| window open, compact | 75 MB | 4.4 % |

Before M1 the tray with a hidden window and the engine running was 61.9 MB private (the OpenGL context and driver
mapped most of it). Time from launch to the window being visible: 310-327 ms.

## Latency breakdown (engine report, headset path CABLE Output -> Logitech, 480-frame periods)

| Part | default | low-latency preset |
|---|---|---|
| capture period | 10.0 ms | 10.0 ms |
| playback buffer (headset forces 480-frame periods) | 22.0 ms | 22.0 ms |
| engine FIFO | 0 ms | 0 ms |
| DSP chain | 11.5 ms | 0.2 ms |
| **total** | **~44 ms** | **~32 ms** |

CABLE Input (240-frame periods): playback buffer 11 ms, FIFO 5 ms; see CLAUDE.md for the VB-Cable loop breakdown.

## Audio quality (offline matrix: 28 noisy mixes of Polish TTS speech x 7 noises x 4 SNRs, `denoise` profile; "full" = whole chain)

Low-latency mode against the default (mean over the 28 mixes):

| | default | low latency |
|---|---|---|
| SDR | 13.1 dB | 8.9 dB (steady noise: pink 14.6 -> 6.7, white 17.5 -> 8.8; hum / keyboard / babble unchanged) |
| attenuation in speech pauses | 23.2 dB | 22.2 dB |
| loss in the first 50 ms of words | -6.6 dB | -6.9 dB |

The SDR loss is time alignment, not filter length (128, 256, 512, 768 taps all give the same): the gains describe the
window centred at the frame start and are applied to the next 480 samples, about 5 ms late on average.

Each optional effect switched on alone, on top of the default chain (mean over the mixes):

| effect | denoise SDR | pause attenuation | word-onset loss | full-chain peak | note |
|---|---|---|---|---|---|
| none | 13.07 | 23.2 dB | -6.6 dB | 0.392 | |
| hum | 13.59 | 25.1 dB | -8.5 dB | 0.389 | removes the hum noise; the 150/200 Hz notches also cost ~2 dB on voiced onsets |
| dereverb | 12.93 | 28.1 dB | -5.6 dB | 0.358 | +4.9 dB more attenuation in pauses |
| breath | 12.06 | 24.8 dB | -6.9 dB | 0.383 | -1 dB SDR |
| guard | 12.84 | 23.5 dB | -6.5 dB | 0.384 | |
| agc | 12.08 | 21.3 dB | -6.3 dB | 0.554 | boosts speech; pauses a little less attenuated |
| deesser | 13.14 | 23.2 dB | -6.5 dB | 0.391 | |
| warmth | 12.86 | 23.1 dB | -6.6 dB | 0.349 | |

No NaN or clipping in any run. These numbers are not a verdict on sound quality - the TTS speech has digital-silence
pauses, so they are optimistic - the user listens before a default changes.

## What was not measured

Linux (nothing compiled there), echo cancellation in a real room, system-audio mixing with a real application, the
driver (not built), push-button hotkeys pressed physically.

## Optimisation round (2026-11-13, after the measurements above)

Profile of the default chain (155 us/frame): RNNoise 132 us (85%) - its network 92 us, feature extraction 30 us -,
limiter 13 us, compressor 7.6, EQ 2.8, high-pass 2.7, gate 1.3. What was done and what it bought (`vf-bench`):

| Change | Where | Effect |
|---|---|---|
| int8 network weights only (`DISABLE_DEBUG_FLOAT`) | RNNoise build | network 92 -> 52 us (portable C), exe 8.4 -> 4.3 MB; audio matrix identical (SDR 13.07 dB, pause 23.2 dB) |
| RNNoise's SSE4.1/AVX2 kernels with run-time CPU dispatch | RNNoise build (`VF_RNNOISE_SIMD`) | network 52 -> 19 us |
| limiter: monotonic queue instead of a 73-step window minimum per sample | `fx_limiter.c` | 13 -> 2.3 us |
| AVX2+FMA dot product / axpy with run-time dispatch (`dsp/simd.c`) in the echo canceller | `fx_aec.c` | 640 -> 54 us, same test results (ERLE 68 dB, SDR 7.3 dB) |
| same kernel in the low-latency FIR | `fx_rnnoise.c` | low-latency mode +57 -> +37 us |
| spectrum columns precomputed, 10 fps when idle and unfocused | GUI | fewer powf calls and redraws |

| Configuration | before | after |
|---|---|---|
| default chain | 155 us (1.55 % of a core) | **72 us (0.72 %)** |
| default + low-latency mode | 212 us | 109 us |
| everything on (incl. echo canceller) | 789 us | **151 us** |
| mic-thingy.exe | 8.0 MB | 4.1 MB |

What is left in the default chain: RNNoise 57 us (feature extraction 30 us - FFT and pitch search in portable C -
network 19 us, synthesis 6 us), everything else together ~15 us. Further speed-ups would need SIMD versions of the
feature extraction (upstream has SSE4.1 LPC helpers that are not vendored) or a smaller network; both are tracked as
ideas, not done. The window itself (~91 MB, 3-5 % of a core while visible) is the graphics driver, not the audio path.
