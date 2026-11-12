# mic thingy

Real-time microphone filter in C11 (Windows 11 WASAPI + Linux PipeWire). Pipeline:
mic -> high-pass 80 Hz -> RNNoise -> VAD-driven noise gate -> compressor -> 3-band EQ -> limiter -> output.
Progress: see "Stages" below.

## Build
Windows (WinLibs GCC + CMake + Ninja; open a fresh shell so PATH is refreshed):
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\mic-thingy-cli.exe [--in N --out N]
```
Linux: `sudo apt install build-essential cmake ninja-build libpipewire-0.3-dev libasound2-dev`, same cmake commands.

## Real-time rules (mandatory)
- Audio callback (`src/audio/engine.c`) and everything it calls (`pipeline_process`, every `Fx.process`/`set_param`):
  NO malloc/free, NO mutexes, NO printf/I/O, NO blocking calls. Allocate in `init`, free in `destroy`.
- UI -> audio parameters: `_Atomic` values (`<stdatomic.h>`), relaxed ordering is fine.
- Audio -> UI metrics: atomics for scalar meters; lock-free SPSC ring buffer for streams (stage 4).
- 48 kHz, 480-sample frames (RNNoise requirement). Engine re-chunks arbitrary device block sizes via preallocated FIFOs.
- RNNoise wants float samples in int16 range: scale x32768 in, /32768 out (done in `fx_rnnoise.c`).
- Target end-to-end latency <= 20 ms - currently NOT met: RNNoise adds 10 ms (overlap-add; upstream look-ahead
  disabled by default, +10 ms if `rnnoise lookahead 1`), limiter 1.5 ms, FIFO pre-fill 0-10 ms (480 - gcd(block, 480) samples),
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
   processed by `mic-thingy-file`, scored by `vf-eval eval` against the *processed clean speech* (phase-safe).
   Output: `build/audiotest/out_<tag>/results.csv` + WAVs. Metrics: SDR, pause-noise attenuation, onset/offset loss
   (first/last 50 ms of words), level change, peak/clip/NaN. `--list` on `vf-eval eval` prints every onset.
   Profile `denoise` bypasses comp/eq/limiter (dynamics distort SNR math); `full` = whole chain.
   Real recordings (`real_*.wav`, no reference) get level percentiles only (`vf-eval noref`).
   TTS has digital-silence pauses: its scores are optimistic. Audio stays in build/ (never commit user recordings).
3. Single file: `build\mic-thingy-file -i in.wav -o out.wav [-c "cmd"]... --debug dbg.wav --csv frames.csv`
   (`dbg.wav` = 4 channels: in, out, VAD, gate gain -> `ffmpeg -i dbg.wav -lavfi showwavespic=split_channels=1 x.png`).
4. Real device latency (Windows, VB-Cable loop; bursts are audible to apps listening to CABLE Output - ask the user first):
   `build\vf-latency [--period N] [--periods M] [--exclusive] [--seconds S]` plays bursts into "CABLE Input", records
   "CABLE Output" in one miniaudio duplex device, prints the round trip. Measured 2026-11-12 (shared mode):
   period 240x2 (engine default) ~75-88 ms, 120x2 ~58-74 ms, 480x2 ~98-108 ms, exclusive is worse (95-130 ms).
   WASAPI shared capture on this machine is fixed at 480-frame periods; playback minimum is 128. Run-to-run spread
   is +-8 ms (start-up alignment), so compare settings with several runs. 6 alternating runs each: period 120 ->
   RTT 70.2 + FIFO 7.5 = 77.7 ms, 160 -> 67.4 + 6.7 = 74.1, 240 -> 73.4 + 5.0 = 78.4: no reliable gain, kept 240
   (`mic-thingy-cli --period N` to experiment).
   RNNoise `lookahead 0` (our patch, third_party/rnnoise/PATCHES.md) saves 10 ms; audio matrix cost: SDR -0.6 dB,
   pause attenuation -0.8 dB, onsets -0.5 dB, real recordings unchanged. DEFAULT since 2026-11-12 (user's choice):
   chain DSP delay = 480 (RNNoise) + 72 (limiter) = 11.5 ms.
Numbers are not a judgement of sound quality - the user listens; propose default changes, don't retune silently.

## Effect module interface (`src/dsp/fx.h`)
Each effect = own `.c/.h` with `Fx` descriptor: `init(fx, sr)`, `process(fx, buf, n, ctx)`, `set_param(fx, id, v)`, `destroy`, atomic `bypass`.
`FxFrameCtx` carries per-frame data along the chain (RNNoise writes `vad_prob`, the gate reads it).

## Layout
`src/audio` engine, devices, miniaudio impl | `src/dsp` effects + pipeline | `src/util` console, commands, wav I/O, config |
`tools` offline processor, evaluator, test-set scripts | `tests` ctest | `third_party` vendored libs.
Third-party code (pinned): miniaudio 0.11.26, RNNoise v0.2 (+ model weights `rnnoise_data-0b50c45`, compiled in; local
patch documented in `third_party/rnnoise/PATCHES.md` - keep it up to date), raylib 5.5 + raygui 4.0 (stage 4).
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
3. [done on Windows; Linux written, untested here] Virtual mic. `mic-thingy-cli --vmic [--monitor N]`
   (or interactive mode menu). Windows: duplex mic -> chain -> "CABLE Input"; apps pick "CABLE Output".
   Linux: capture-only engine -> tap -> `VfSink` -> `pw_stream` (`src/audio/vmic_pipewire.c`, media.class
   Audio/Source/Virtual, node "mic_thingy", node.latency 480/48000); CMake option `VF_PIPEWIRE` (default ON on Linux).
   Monitor = second playback device fed from a `VfSink`. `VfSink` (`src/audio/sink.c`) bridges two audio clocks:
   lock-free ring, primes to `target` (960), drops/repeats one sample for drift, raises target after an underrun.
   Guard: refuses CABLE Output / our own source (also the old "VoiceFilter" name) as input in vmic modes (feedback loop).
   E2E test (Windows, plays into CABLE): `mic-thingy-cli --in-file mix.wav --vmic --seconds 11` while
   `vf-record --in "CABLE Output" --seconds 9 rec.wav`, then `vf-eval match offline.wav rec.wav`
   (2026-11-12: r = 1.00000, gain 0.00 dB, residual -88.8 dB, 0 underruns).
4. [done] GUI `mic-thingy` (`src/main_gui.c`, raylib 5.5 + raygui 4.0, Polish UI). Device dropdowns, output mode
   (headphones / virtual mic / virtual mic + monitor), Start/Stop (auto-restart on device change), global bypass,
   one panel per effect built from `Fx.params` (sliders; `toggle` params as checkboxes; "Domyślne" reset),
   meters fed by the lock-free meter ring (`Pipeline.meter_ring`, `VF_METER_FIELDS` floats per frame): in/out
   peak + hold, VAD with gate-threshold marker, gate opening, compressor GR, latency estimate, xruns.
   Start/stop logic shared with the CLI in `src/app/session.c` (`VfSession`).
   raylib is built WITHOUT raudio (`SUPPORT_MODULE_RAUDIO=OFF`): its embedded miniaudio would clash with ours.
   raygui gotcha: call `GuiLoadStyleDefault()` BEFORE `GuiSetFont()` - it unloads the previous font's texture.
   Font: Segoe UI (Windows) / DejaVu or Noto (Linux) with Latin Ext-A for Polish, fallback raylib default.
   Visual check: `build\mic-thingy --autostart --mode 1 --in-file mix.wav --screenshot shot.png --frames 150`
   (writes shot.png into the working directory and quits; `--verbose` = raylib log).
   Linux GUI deps: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`.
5. [done] Settings, presets, autostart. `src/app/settings.c` + `src/util/ini.c` + `src/util/platform.c`.
   `settings.ini` in `%APPDATA%\mic thingy` / `$XDG_CONFIG_HOME/mic-thingy` (override: env `VF_CONFIG_DIR`,
   used by tests): [audio] devices BY NAME + mode + bypass, [app] start_processing + preset, [fx.<name>] enabled +
   params. GUI autosaves 1 s after a change and on exit (not in --screenshot runs); CLI reads [fx.*]
   (`--no-config`, `--preset NAME`). Presets: built-ins in code (Domyślny, Głośne otoczenie, Podcast, Naturalny,
   as "set fx param value" lines), user presets in `presets/<name>.ini` (fx sections only, names sanitized,
   built-in names refused). Autostart: HKCU\...\Run "mic thingy" / `~/.config/autostart/mic-thingy.desktop`,
   command `"<exe>" --autostart --minimized`. Autostart test is opt-in: `VF_TEST_AUTOSTART=1` (uses a separate
   "MicThingy_selftest" entry and removes it).
   Paths are UTF-8 everywhere; on Windows `vf_fopen` / `vf_mkdir_p` convert to UTF-16.
6. [done] Background mode (`src/app/tray.c`, GUI only). `[app] background = 1`: the window close button hides the
   window to a tray icon (Windows: Shell_NotifyIcon on its own thread + hidden window class "MicThingyTrayWindow";
   menu: Pokaż okno / Przetwarzanie dźwięku / Filtr włączony / Zakończ) - audio keeps running until "Zakończ".
   No tray on Linux: close minimizes instead. Ticking "Start z systemem" also sets start_processing + background;
   autostart launches `--autostart --minimized` -> starts hidden in the tray and filters immediately.
   `FLAG_WINDOW_ALWAYS_RUN` is required (raylib otherwise blocks in glfwWaitEvents while minimized); FPS drops to 10
   when hidden/minimized and no UI is drawn. Single instance: named mutex "Local\MicThingySingleInstance"
   (Linux: flock on <config>/mic-thingy.lock); a second launch posts WM_APP+2 to the tray window -> first instance
   shows its window, second exits 0. Robustness: if the stream stops (device unplugged) or start fails (USB headset
   late after login), the GUI re-enumerates devices, finds them by name and retries every 2-3 s while running is wanted.
7. [done] UI languages + retro look. `src/app/i18n.c`: `tr(S_...)`, `tr_fx()`, `tr_param()`; English is the default,
   Polish second (`[app] language = en|pl`, header EN/PL toggle, `--lang`). Every new UI string goes into the
   `k_str` table in BOTH languages (ctest checks none is empty). Built-in presets have stable keys
   (default/noisy/podcast/natural) stored in settings; display names come from `vf_preset_display_name()`;
   old Polish names still map to the keys. Tray menu/tooltip follow the language.
   Look ("2000s"), split-complementary palette: cool graphite/slate neutrals (chassis, panels, title strips),
   neon green ONLY for accents and anything switched ON, burgundy for anything switched OFF / errors, amber only
   as meter warning. On/off controls use `onoff()` (green on / burgundy off) - use it for any new switch.
   Win2000-style bevelled panels with gradient title strips and status LEDs (green on, dim green idle, burgundy off), segmented LED meters, LCD windows with scanlines (Lucida Console), Tahoma UI font, blinking
   cursor status line. Theme colours/fonts are at the top of `main_gui.c` (`C_*`, `load_fonts`, `apply_style`).
   Screenshot checks: use a VF_CONFIG_DIR with `start_processing = 0` plus `--in-file`, otherwise the real mic
   is opened (and in monitor mode played to the speakers).
   Live test (Windows): scratchpad-style script with VF_CONFIG_DIR, `--minimized --in-file`, vf-record on CABLE,
   PostMessage WM_CLOSE to the GLFW window; note PowerShell turns $null into "" for P/Invoke strings -
   use [NullString]::Value with FindWindowW.

8. [done] Rename to "mic thingy" + icon + title bar + precise editing.
   - User-facing name "mic thingy" (window, logo, tray, PipeWire source "mic thingy microphone", autostart entry).
     Executables: `mic-thingy` (GUI), `mic-thingy-cli`, `mic-thingy-file`. Internal identifiers keep the `vf_`/`VF_`
     prefix (not user visible - don't churn them). Config dir: `%APPDATA%\mic thingy`, `~/.config/mic-thingy`;
     `vf_config_dir()` moves an old `VoiceFilter`/`voicefilter` dir over once; `vf_autostart_migrate()` swaps an old
     "VoiceFilter" Run entry / voicefilter.desktop for the new one at GUI start.
   - Icon: `src/app/icon_art.c` draws it procedurally (bevelled retro monitor, navy screen, green sine + square wave).
     `build/make_icon assets/mic-thingy.ico` regenerates the committed .ico after art changes; Windows resource
     `GLFW_ICON` (assets/mic-thingy.rc.in) gives the .exe/window/taskbar icon; the tray renders the art at runtime
     (grey waves when stopped); Linux uses SetWindowIcon. The .rc is built as a separate OBJECT library because
     windres breaks on include paths with spaces (the project dir is "mic thingy").
   - Title bar: `src/app/win_chrome.c` sets DWM caption/text/border colours (Windows 11) = header top colour
     #3A445C, so there is no white bar; Windows 10 gets the dark title bar. Kept out of main_gui.c because
     windows.h clashes with raylib.h.
   - Editing: click a value (LCD text) in an effect panel -> text box, Enter/click-away applies (comma accepted),
     Esc cancels; mouse wheel over a slider = 1% of range (Shift 0.1%, ints >= 1); the VAD meter's amber marker
     is the gate threshold - drag it or use the wheel.
   - GUI interaction test without touching the real mouse/keyboard: PostMessage WM_MOUSEMOVE/WM_LBUTTON*/WM_CHAR/
     WM_KEYDOWN (lParam needs the scan code, e.g. Enter 0x1C, Backspace 0x0E) / WM_MOUSEWHEEL (screen coords) to the
     GLFW window, VF_CONFIG_DIR with start_processing = 0, then read settings.ini (autosaved after 1 s).
9. [done] Open source (MIT) + final logo.
   - LICENSE (MIT, "mic thingy contributors"), THIRD_PARTY_NOTICES.md (every vendored lib + where its license text is),
     CONTRIBUTING.md (rules for contributors), README.md in English (README.pl.md Polish), .gitattributes.
     Keep THIRD_PARTY_NOTICES.md in sync when adding/updating a dependency; only MIT-compatible licenses.
   - Git history was rewritten so author/committer is "mic thingy dev <mic-thingy@users.noreply.github.com>" and
     contains no personal e-mail or local user paths. Never commit personal data (e-mails, user names in paths,
     device names in screenshots - use `mic-thingy --demo` for public screenshots; assets/screenshot.png was made so).
   - Logo (after the user's sketch): bevelled frame with green + white bands top/left, navy screen, tilted handheld
     mic with slots, green status LED, mesh grille and a cable. Also drawn in the GUI header (36 px).
     Hi-res renders: assets/logo-1024.png, assets/logo-256.png (make_icon writes any <name>-<size>.png).
   - Not published anywhere yet (the user chose "prepare only").