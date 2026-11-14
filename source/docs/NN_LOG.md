# Neural denoiser ("nn" effect) - training and evaluation log

Code: `tools/nn/` (Python, CPU torch) -> `src/dsp/nn_weights.c` (generated) -> `src/dsp/fx_nn.c`.
Speech: LibriSpeech (CC BY 4.0) dev-clean + train-clean-100 subset for training, **test-clean speakers only for
evaluation** (never trained on). Noise: synthesised families in `tools/nn/nnlib.py` (+ babble from LibriSpeech).

Two evaluations, both through the real C program (`mic-thingy-file`):
1. `tools/nn/evaluate.py` - held-out speakers x 13 noise families x SNR 0/5/10/20 dB, denoiser only (highpass, gate,
   comp, eq, limiter bypassed). Noise families come from the same generators as training (other seeds): optimistic.
2. `tools/run_audio_tests.ps1` - the project's older matrix: Polish TTS voice (never trained on) + ffmpeg noises
   (different generator code) through the "denoise" profile (gate ON). Out of distribution for the model.

Columns (1): dSDR = SI-SDR gain over the noisy input; pause = noise removed where nobody speaks (dB);
lost = share of speech frames more than 6 dB quieter than the clean voice.

| run | data / steps | eval 1 ALL: dSDR / pause / lost | notes |
|---|---|---|---|
| RNNoise (baseline) | - | +1.16 / 8.2 dB / 5.0 % | keyboard 5.1, mouse 4.7, clatter 1.6, thump 1.2 dB pause attenuation |
| run1 @ step 500 | dev-clean (40 spk), 500 x 128 x 2 s | +7.18 / 24.8 dB / 4.1 % | mouse 38.8, keyboard 29.7, thump 20.1 dB; babble +0.4 |
| run1 + RNNoise in series @500 | | +3.82 / 24.8 / 8.1 % | worse than nn alone: both remove speech |

Eval 2 (gate on, denoise profile), mean over 0/5/10/20 dB:

| noise | RNNoise SDR / pause / onset | nn@500 SDR / pause / onset |
|---|---|---|
| pink | 14.6 / 38.7 / -20.9 | 13.3 / 13.1 / -2.0 |
| white | 17.5 / 35.4 / -16.1 | 16.8 / 24.8 / -9.8 |
| brown | 12.9 / 35.6 / -13.7 | 11.1 / 7.5 / +3.1 |
| hum | 14.7 / 6.8 / +4.3 | 14.8 / 33.7 / +3.4 |
| fan | 14.8 / 37.8 / -8.0 | 13.1 / 11.7 / +3.6 |
| keyboard | 8.7 / 7.7 / +7.6 | 9.9 / 23.9 / +4.1 |
| babble | 8.3 / 0.1 / +1.0 | 8.2 / 0.2 / +1.0 |

RNNoise + gate kills stationary noise in pauses but also chops word onsets (-14..-21 dB); the nn keeps onsets but
leaves more stationary noise in pauses at step 500.

Eval 3 - real recorded noise, never seen in training (Windows notification sounds `C:\Windows\Media`, a folder of
game sound effects; local only, not committed), held-out speakers, SNR 0/5/10/20, `evaluate.py --noise-dirs`:

| model | ALL dSDR / pause / lost | Windows sounds | game sfx |
|---|---|---|---|
| RNNoise | +0.96 / 5.0 / 7.7 % | -1.00 / 3.0 / 5.8 % | +2.91 / 7.0 / 9.5 % |
| nn@500 | +1.89 / 7.6 / 2.8 % | -0.49 / 5.8 / 1.8 % | +4.28 / 9.5 / 3.7 % |

Gate onset experiment (2026-11-14): an energy trigger in the gate (open on a level jump 12 dB above a tracked
background, 60 ms hold) moved pink-noise onsets only -20.9 -> -15.3 dB (white unchanged) and let keyboard clicks
through (pause 7.7 -> 1.4 dB); reverted. It measures after RNNoise, where onsets are already reduced.

**Bandwidth trap (found 2026-11-14, before run 3).** LibriSpeech is 16 kHz. Runs 1-2 saw/predicted 161 bins
(0-8 kHz), the loss covered all 481 bins (clean target ~0 above 8 kHz) and the gain above 8 kHz was the mean of
bins 140-160. Result on CLEAN speech: gain 0.94 at 1-5 kHz but 0.22-0.35 at 7-8 kHz (and the same above 8 kHz),
i.e. a muffled voice that no SDR metric could see (all references are band-limited too). Fix in run 3: 145 bins
(0-7.2 kHz) in and out, loss only there, bins >= 145 take the mean gain of 5-7 kHz; `evaluate.py` now prints the
band levels of clean 48 kHz TTS speech through each config (must stay ~0 dB in every band).
CPU (vf-bench, while training ran on the same machine): default chain ~93 us/frame, + nn ~+200 us (3 % of a core).

| run3 @ 1500 (in the app since 2026-11-14) | tr100 + dev (291 spk), 145 bins, 1500 x 128 x 2 s | **+8.78 / 27.7 dB / 4.6 %** | keyboard 31.5, mouse 38.8 dB; babble +0.6 dB, lost 17 % (worst family) |

Clean wideband speech through the C effect (run3 @ 1500): 300-3k 0.0, 4-7k -0.4, 7-8k -0.3, 8-12k -0.3,
12-16k -1.8 dB (run 1-2 cut 7-8 kHz by 9-13 dB). Real noise (eval 3): dSDR +1.92 vs RNNoise +0.96, lost speech
0.5 % vs 7.7 %, pause 5.7 vs 5.0 dB. Run 3 then died at ~1700 steps (out of RAM while other jobs ran).

**Next trainer: `source/ml`** (2026-11-14): config-driven project (nsnet / crn / erbdf), real noise and RIRs
(OpenSLR 28, DEMAND), MR-STFT + SI-SDR + compressed spectral loss, DNSMOS, streaming ONNX, `export_c.py`
for this effect. See `source/ml/README.md`.
