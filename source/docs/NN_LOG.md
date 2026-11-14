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
