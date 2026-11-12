# VoiceFilter

Real-time microphone filter in C11 (Windows 11 WASAPI + Linux PipeWire). Pipeline:
mic -> high-pass 80 Hz -> RNNoise -> VAD-driven noise gate -> compressor -> 3-band EQ -> limiter -> output.
Progress: see "Stages" below.

## Build
Windows (WinLibs GCC + CMake + Ninja; open a fresh shell so PATH is refreshed):
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\voicefilter-cli.exe [--in N --out N]
```
Linux: `sudo apt install build-essential cmake ninja-build libpipewire-0.3-dev libasound2-dev`, same cmake commands.

## Real-time rules (mandatory)
- Audio callback (`src/audio/engine.c`) and everything it calls (`pipeline_process`, every `Fx.process`/`set_param`):
  NO malloc/free, NO mutexes, NO printf/I/O, NO blocking calls. Allocate in `init`, free in `destroy`.
- UI -> audio parameters: `_Atomic` values (`<stdatomic.h>`), relaxed ordering is fine.
- Audio -> UI metrics: atomics for scalar meters; lock-free SPSC ring buffer for streams (stage 4).
- 48 kHz, 480-sample frames (RNNoise requirement). Engine re-chunks arbitrary device block sizes via preallocated FIFOs.
- RNNoise wants float samples in int16 range: scale x32768 in, /32768 out (done in `fx_rnnoise.c`).
- Target end-to-end latency <= 20 ms - currently NOT met: RNNoise v0.2 alone adds 20 ms (2 frames: overlap-add +
  one frame look-ahead `delayed_X`), limiter 1.5 ms, FIFO pre-fill 0-10 ms (480 - gcd(block, 480) samples),
  plus WASAPI buffers. Each effect declares `Fx.latency`; `engine_latency()` sums it. Always report latency honestly.

## Testing (run after every DSP/engine change)
1. `ctest --test-dir build` - offline unit tests (`tests/test_dsp.c`, C-generated signals only, cross-platform):
   filter responses, gate hold, compressor curve, limiter ceiling, NaN safety, command parser,
   measured RNNoise delay (= `RNNOISE_DELAY_SAMPLES`, 960), dry/wet phase alignment, whole-chain delay,
   and the engine callback (`engine_process_block`) with block sizes 480/441/448/512/128/1024/random.
2. Audio matrix (Windows; needs ffmpeg + System.Speech TTS):
   ```
   powershell -ExecutionPolicy Bypass -File tools\make_testset.ps1 [-Recordings "rec.mkv@47.9;rec2.mkv@20.7"]
   powershell -ExecutionPolicy Bypass -File tools\run_audio_tests.ps1 -Tag base [-Cmds "set rnnoise wet 0.95;set gate hold 250"]
   ```
   Polish TTS speech + 7 noises (pink/white/brown/hum/fan/keyboard/babble) mixed at 0/5/10/20 dB SNR by `vf-eval mix`,
   processed by `voicefilter-file`, scored by `vf-eval eval` against the *processed clean speech* (phase-safe).
   Output: `build/audiotest/out_<tag>/results.csv` + WAVs. Metrics: SDR, pause-noise attenuation, onset/offset loss
   (first/last 50 ms of words), level change, peak/clip/NaN. `--list` on `vf-eval eval` prints every onset.
   Profile `denoise` bypasses comp/eq/limiter (dynamics distort SNR math); `full` = whole chain.
   Real recordings (`real_*.wav`, no reference) get level percentiles only (`vf-eval noref`).
   TTS has digital-silence pauses: its scores are optimistic. Audio stays in build/ (never commit user recordings).
3. Single file: `build\voicefilter-file -i in.wav -o out.wav [-c "cmd"]... --debug dbg.wav --csv frames.csv`
   (`dbg.wav` = 4 channels: in, out, VAD, gate gain -> `ffmpeg -i dbg.wav -lavfi showwavespic=split_channels=1 x.png`).
Numbers are not a judgement of sound quality - the user listens; propose default changes, don't retune silently.

## Effect module interface (`src/dsp/fx.h`)
Each effect = own `.c/.h` with `Fx` descriptor: `init(fx, sr)`, `process(fx, buf, n, ctx)`, `set_param(fx, id, v)`, `destroy`, atomic `bypass`.
`FxFrameCtx` carries per-frame data along the chain (RNNoise writes `vad_prob`, the gate reads it).

## Layout
`src/audio` engine, devices, miniaudio impl | `src/dsp` effects + pipeline | `src/util` console, commands, wav I/O, config |
`tools` offline processor, evaluator, test-set scripts | `tests` ctest | `third_party` vendored libs.
Third-party code (pinned): miniaudio 0.11.26, RNNoise v0.2 (+ model weights `rnnoise_data-0b50c45`, compiled in), raylib 5.5 + raygui 4.0 (stage 4).
RNNoise has no upstream CMake: built by our own target using the portable (non-x86-RTCD) code path.

## Conventions
- C11, `-Wall -Wextra -Wpedantic` clean for our code (third_party built with `-w`).
- Commit after every working stage; one stage must compile and be testable before starting the next.
- Don't test with speakers (mic feedback) - use headphones.

## Stages
1. [done] Mic -> RNNoise -> headphones, console device selection, `b` bypass.
2. [done] Full chain (highpass 4th-order, RNNoise, VAD gate, compressor, EQ3, look-ahead limiter) + console `:` commands
   (`show`, `set <fx> <param> <value>`, `bypass`, `reset`), keys 1..6. Offline tests: `ctest --test-dir build` (`tests/test_dsp.c`).
   Params: UI writes `Pipeline.params[fx][id]` (atomic); audio thread applies changes once per frame. Effects declare
   their params in `Fx.params` (name/unit/min/max/default) - the GUI in stage 4 builds its sliders from that table.
3. Virtual mic: Windows "CABLE Input" (VB-Cable), Linux `pw_stream` Audio/Source.
4. GUI (raylib + raygui).
5. INI settings, presets, autostart.
