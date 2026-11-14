# Speech enhancement trainer (mic thingy)

Train causal, low-latency neural noise suppression (optionally light dereverberation) that runs in real
time, and compare it against strong baselines (RNNoise, Krisp) with the same metrics.

- **Data**: on-the-fly mixing of clean speech + noise + room impulse responses, random SNR (-5..20 dB),
  random level, microphone colouration, clipping. Microsoft DNS Challenge data or a small public set
  (LibriSpeech + OpenSLR 28 noises/RIRs + DEMAND), plus 11 synthetic noise families (typing, clicks, fans,
  hum, music-like tones...) that public sets cover thinly. 16 kHz and 48 kHz.
- **Models** (all causal, 20 ms window / 10 ms hop = **10 ms algorithmic latency**, streaming == offline):

  | name | idea | params | runs in the mic thingy app |
  |---|---|---|---|
  | `nsnet` | NSNet-style: log-power bins -> 2 GRUs -> per-bin gains + speech-presence | 0.33 M | yes (`export_c.py`, `src/dsp/nn.c`) |
  | `erbdf` | DeepFilterNet-style: ERB-band gains + complex multi-frame deep filter on the low bins | 0.35 M | not yet |
  | `crn` | causal conv-recurrent U-Net on the complex spectrum, bounded complex mask (DCCRN-E) | 1.3 M | not yet |
- **Loss**: multi-resolution STFT (spectral convergence + log magnitude) + SI-SDR + compressed complex spectral
  loss (+ speech-presence BCE). Band-limited automatically when the speech corpus is (LibriSpeech = 16 kHz).
- **Training**: AdamW, warm-up + cosine LR, gradient clipping, AMP (CUDA), checkpoints, validation on held-out
  speakers and noise (SI-SDR), DNSMOS on real recordings, before/after examples at every validation.
- **Evaluation**: SI-SDR, STOI, PESQ (if installed), DNSMOS P.835 (official ONNX), noise left in pauses;
  any external system (Krisp, RNNoise...) is scored on the same files.
- **Export**: streaming ONNX (one frame + explicit state per call), C weights for the app.

## 1. Install

Python 3.10+ (tested 3.14). In this folder (`source/ml`):

```bash
python -m venv .venv
```
Windows: `.venv\Scripts\activate`, Linux/macOS: `source .venv/bin/activate`, then:
```bash
pip install torch torchaudio --index-url https://download.pytorch.org/whl/cpu
```
(for an NVIDIA GPU use the CUDA command from pytorch.org instead)
```bash
pip install -r requirements.txt
```
```bash
python tests/test_pipeline.py
```
The self-test needs no data: every model and sample rate, offline == streaming, causality, the ONNX step
path, losses back-propagating.

## 2. Data

Small set (recommended on one machine, ~9 GB download, ~12 GB on disk):
```bash
python download_data.py --small
```
| set | what | licence | size |
|---|---|---|---|
| LibriSpeech train-clean-100, dev-clean, test-clean | read English speech, 16 kHz, 331 speakers | CC BY 4.0 | 6.3 + 0.7 GB |
| OpenSLR 28 RIRS_NOISES | 60 k real + simulated room impulse responses, 843 point-source noises | Apache 2.0 | 1.3 GB |
| DEMAND (48 kHz, channel 1) | 18 real environments (kitchen, cafe, station, car...), 3 held out | CC BY-SA 3.0 | 6 GB download |
| DNSMOS | Microsoft's P.835 quality predictor (ONNX) | MIT (DNS-Challenge repo) | 1.4 MB |

`test-clean` speakers and the DEMAND environments `OMEETING`, `PSTATION`, `TBUS` are never used for training.
Put your own noisy recordings into `data/real/` (any WAV; no clean reference needed): validation reports
their DNSMOS.

Training weights you plan to publish: LibriSpeech asks for attribution (CC BY), DEMAND is share-alike
(CC BY-SA) - leave `data/demand_*` out of `noise_dirs` if that matters for your release.

**Full DNS Challenge data** (~1 TB, 48 kHz full-band speech, noise, RIRs): use Microsoft's scripts,
<https://github.com/microsoft/DNS-Challenge> (`download-dns-challenge-4.sh` / `-5`), extract under
`data/dns/`, then train with `configs/dns48k.yaml` (no band limit, dereverberation on). The official
synthesizer is not needed: `se/dataset.py` mixes on the fly.

## 3. Train

```bash
python train.py --config configs/crn48k.yaml
```
```bash
python train.py --config configs/nsnet48k.yaml train.batch=32 train.threads=12
```
```bash
python train.py --config configs/erbdf48k.yaml
```
Any config value can be overridden as `section.key=value`; resume with `--resume runs/<name>/last.pt`.
Output in `runs/<name>/`: `log.csv`, `best.pt` (best validation SI-SDR), `last.pt`, `examples/stepNNNNNN/`
(noisy + enhanced WAVs), `val/` (the fixed validation mixtures).

Configs: `crn48k`, `nsnet48k`, `erbdf48k` (48 kHz, LibriSpeech band-limited: the network sees and predicts
0-7.2 kHz, higher bins follow the 5-7 kHz gain, so real full-band voices keep their treble), `crn16k`
(wideband, 3x cheaper), `dns48k` (full DNS, full band).

Throughput reference (CPU, 32 threads, Ryzen-class): nsnet ~2-3 steps/s at batch 32 x 2 s; a GPU makes the
CRN practical (AMP is enabled automatically on CUDA).

## 4. Inference

```bash
python inference.py --ckpt runs/crn48k/best.pt noisy.wav --out-dir enhanced
```
```bash
python inference.py --ckpt runs/crn48k/best.pt noisy.wav --stream --check
```
`--stream` runs the real-time path (10 ms blocks, carried state); `--check` compares it with the offline
path. Live microphone demo (headphones!):
```bash
python demo.py --ckpt runs/crn48k/best.pt --live --in "Microphone" --out "Headphones"
```

## 5. Evaluate and compare with Krisp

Build a test set (held-out speakers and noises), then score checkpoints and any external outputs:
```bash
python evaluate.py --make-set data/testset --speech data/LibriSpeech/test-clean --noise data/demand_val --n 60
```
```bash
python evaluate.py --set data/testset --ckpt runs/crn48k/best.pt --ckpt runs/nsnet48k/best.pt --examples 4
```
External systems: put their output next to the input as `<name>_noisy.<system>.wav`
(e.g. `0007_snr05_noisy.krisp.wav`); they are aligned (device delay) and scored like the models.
Krisp has no file mode. Play the noisy file into the microphone Krisp listens to (e.g. a VB-Audio
virtual cable) and record "Krisp Microphone". `tools/nn/krisp_sim.py` automates that loop. Krisp
cancels the playing app's own audio as echo, so the player must be a separate process (the script does
that).

Metrics: `si_sdr` (dB, needs the clean reference), `stoi`, `pesq_wb` (if the `pesq` package is installed),
`pause_atten_db` (noise removed where nobody speaks), `dnsmos_sig/bak/ovrl` (no reference needed; OVRL is the
headline quality number). Per-item CSV in `<set>/results.csv`, before/after WAVs in `<set>/examples/`.

## 6. Export

```bash
python export_onnx.py runs/crn48k/best.pt
```
Streaming ONNX: inputs `re`, `im` (rfft of the windowed 20 ms frame) + `state_*`; outputs the enhanced
`re_out`, `im_out` + `state_out_*` (feed back next frame). The host does window, FFT and overlap-add exactly as
`se/stream.py`. The script verifies onnxruntime against torch.

For the mic thingy app (nsnet only):
```bash
python export_c.py runs/nsnet48k/best.pt
```
It writes `src/dsp/nn_weights.c` + `tests/nn_golden.h`; `ctest` then checks the C inference against torch.

## Layout

```
configs/           YAML configs (data paths, STFT, model, loss, training)
se/config.py       dataclass config + YAML/CLI overrides
se/audio.py        audio I/O, resampling, levels, EQ, RIR helpers
se/synth_noise.py  synthetic noise families
se/dataset.py      on-the-fly mixing (train stream, fixed validation set)
se/stft.py         STFT/iSTFT (power-complementary windows, perfect reconstruction)
se/models/         base interface, nsnet, crn, erbdf
se/losses.py       MR-STFT, SI-SDR, compressed complex spectral loss, VAD BCE
se/metrics.py      SI-SDR, STOI, PESQ, DNSMOS, alignment
se/stream.py       real-time frame-by-frame processing
train.py inference.py evaluate.py export_onnx.py export_c.py demo.py download_data.py
tests/test_pipeline.py
```
