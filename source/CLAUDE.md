# mic thingy

**Repository layout (since 2026-11-13)**: the repository root holds only README.md, README.pl.md, LICENSE, the two release
programs (`mic-thingy.exe`, `mic-thingy-linux`, committed by `.github/workflows/release.yml` when a release is published) and
`source/`, which is the old project root: everything below (`CMakeLists.txt`, `src/`, `tools/`, `build/` ...) is relative to
`source/`. Run cmake and the tools from `source/`; Claude Code sessions should start in `source/` to load this file.

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
   Latency breakdown (L0, 2026-11-12, `docs/PLAN_PERFORMANCE.md`): `engine_latency()` now reads the REAL WASAPI
   buffers (`dev.wasapi.actualBufferSizeInFrames*`; under IAudioClient3 internalPeriods x period is not the buffer),
   miniaudio's fixed-size callback buffers (`intermediaryBufferCap`), callback block min/max and the FIFO fill
   (relaxed atomics in `engine_process_block`). `mic-thingy-cli --latency-report` prints it; `vf-latency` builds its
   device with `engine_device_config()` (same as the engine) and prints "ours vs cable + OS". Experimental knobs
   (`EngineConfig` / CLI / vf-latency): `--fixed` (old fixed-size callbacks), `--pro-audio` (MMCSS), `--periods M`.
   L1 (2026-11-12): variable-size callbacks (`noFixedSizedCallback`) + `noPreSilencedOutputBuffer` are now the
   DEFAULT for the engine and the monitor device: the 2 x 240-frame miniaudio intermediaries are gone (~5 ms;
   A/B over 12 alternating runs each: ~75 vs ~80 ms median). Measured and dropped: `--periods 1` (WASAPI keeps the
   same 528/1056-frame buffers), period 128 / 480 (480 doubles the playback buffer), pro-audio (all within the
   +-8 ms spread). Not tried, by reasoning: adaptive FIFO priming (measured: WASAPI duplex still calls back with
   240-frame blocks, so 240 FIFO samples are unavoidable with 480-sample frames); separate capture + playback
   devices for vmic (would add a VfSink buffer on top of the same WASAPI buffers).
   Resampling note: miniaudio opens WASAPI with AUTOCONVERTPCM, so `internalSampleRate` always reads 48000; the
   engine reads the mixer rate from `ma_device_get_info()` -> `nativeDataFormats[0]` (= GetMixFormat, WASAPI only)
   and the GUI shows `S_N_RESAMPLE_FMT` when it is not 48000. Not triggered here (all devices at 48 kHz).
   Soak tests with the new default, 0 underruns / 0 overruns each: file source -> "CABLE Input" (playback only,
   10 min) and duplex "CABLE Output" -> Logitech headset (silent, 12 min). Period sizes are device dependent:
   the headset forces 480-frame playback periods (buffer 1056 = 22 ms) but then the FIFO is 0; CABLE Input takes 240.
   The variable-callback default also applies on Linux (capture-only engine, PipeWire tap) - untested there.
   L0 measurement, 6 alternating runs x 6 configs, median RTT (min..max):
   default 81.5 (75.7..87.2) | --no-fixed 74.2 (65.0..78.1) | --pro-audio 78.4 | both 74.0 | --no-fixed --period 128
   72.4 | --no-fixed --periods 3 76.3 ms. Buffers: capture period 480 (buffer 1056), playback buffer 528 (280 at
   period 128), miniaudio intermediaries 240 + 240. Our side of the loop = ~26 ms; **~50-56 ms is VB-Cable + the
   Windows audio engine** (not reachable from our code; VB-Cable "Max Latency" setting / own driver, plan section 7).
   The WASAPI-guarded paths (`MA_SUPPORT_WASAPI`) are not compiled on Linux here - check there.
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
   Audio/Source + node.virtual (tested on Fedora/PipeWire 1.6: with Audio/Source/Virtual the node had 0 ports), node "mic_thingy", node.latency 480/48000); CMake option `VF_PIPEWIRE` (default ON on Linux).
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
   - Logo (after the user's sketch, in the GUI palette): bevelled frame with a green accent band + silver band
     top/left and graphite shadow sides, dark LCD screen with grid + scanlines, tilted slate handheld mic with slots,
     glowing green LED, green accent ring, mesh grille and cable, plus a small LED level meter. "Off" variant
     (tray while stopped): burgundy band/LED, dark meter. Below 40 px (`detail` = 0) grid/scanlines/meter are
     dropped and the mic gets a neon rim + lighter body so it reads on the dark screen; the GUI header renders
     the logo at its display size (36 px) for that reason.
     Hi-res renders: assets/logo-1024.png, assets/logo-256.png (make_icon writes any <name>-<size>.png).
   - Not published anywhere yet (the user chose "prepare only").
## Performance work log (docs/PLAN_PERFORMANCE.md)
- **M1** (2026-11-13): in background mode the window AND its OpenGL context are destroyed (`window_open`/`window_close`
  in main_gui.c; `GuiSetFont(GetFontDefault())` before unloading fonts or raygui double-frees on reopen; raylib `GetTime()`
  is invalid without a window -> `vf_now_s()`). Tray with engine running: private working set 61.9 MB -> 0.8-2.3 MB;
  window open ~70-86 MB (mostly the GL driver). No MSAA. 30 fps when unfocused.
- **L2 low-latency mode** (`rnnoise lowlat 1`, preset "Low latency" = + `limiter lookahead 0`): RNNoise only analyses
  (`rnnoise_analyze_frame`, third_party/rnnoise/PATCHES.md #2); its 481 per-bin gains become a 128-tap minimum-phase FIR
  (`src/dsp/minphase.c`, cepstral method, 1024-pt FFT) that filters the SAME frame, cross-faded from the previous frame's
  filter over 240 samples. DSP delay 11.5 ms -> 8 samples (declared; measured lag 3). CPU 78 -> 105 us per 10 ms frame.
  Audio matrix (denoise profile, 28 noisy mixes) vs default: SDR 13.1 -> 8.9 dB (stationary noise: pink 14.6 -> 6.7,
  white 17.5 -> 8.8; hum/keyboard/babble unchanged), pause attenuation 23.2 -> 22.2 dB, onsets -0.3 dB, clean speech
  unchanged. Longer filters (256/512/768 taps) give the same SDR: the loss is TIME alignment - the gains describe the
  960-sample window centred at the frame START and are applied to the 480 samples after it (avg 5 ms late), which is
  inherent to a zero-latency design. Default stays "quality"; the user decides after listening.
- Not done (reasoned): M2 two processes (tray is already ~1 MB), M3 software renderer (raylib 5.5 has none), L3 exclusive
  WASAPI (measured worse), RNNoise AVX2 (CPU is 0.8% of a core). Not done: processing the chain on device-sized blocks
  (would save the 0-5 ms FIFO but needs delayed gains).

## Added 2026-11-13 (docs/PLAN_PERFORMANCE.md + docs/PLAN_FEATURES.md)
Tests: `ctest` now runs three executables: `test_dsp` (core), `test_fx` (optional effects + AEC), `test_app` (mute/mix bus,
soundboard, taps, hotkeys, stats, calibration, undo, automute, appwatch, themes, settings/profiles/export, recorder,
spectrum, A/B, vmic ring). `test_app` uses `VF_CONFIG_DIR=vf_test_cfg_app` (in the build dir).

**Effects** (chain order in `src/dsp/chain.h`; 14 effects, `VF_MAX_FX` 16): optional ones have `Fx.default_bypass = 1`
(restored by `vf_fx_reset`/presets) and `Fx.page` (GUI tab 0 basic / 1 extra / 2 effects):
aec (NLMS 2048 taps, needs reference), hum (notches, auto 50/60 via 100 ms Goertzel), dereverb (tail expander following
the speech level), breath (VAD-based breaths + HF clicks, needs the RNNoise VAD), guard (fast shout compressor),
agc (speech frames only), deesser (x - lowpass band split: `x - h(1-g)` with a HIGH-PASS h is wrong - phase),
warmth (tanh + low shelf, 0 = bit exact), voice (pitch = two cross-faded delay taps, +-3% perceived pitch because of the
sidebands; robot; radio). Tests never use effect indices - use `pipeline_find_fx`.

**Pipeline** (`pipeline.[ch]`): `mute` (10 ms ramp, voice only) and the mix bus (`MixSource`: gain, duck while the
voice is active, level meter) are applied right before the limiter (`mix_before`); with the chain bypassed the same
mute + bus + a hard clip run. The limiter reports `ctx->limiter_gain` -> meter field `VF_M_LIM`. `scope_ring` feeds the
spectrum analyser (UI sets/clears it). Engine hooks: `pre_tap` (raw input), `capture` (memory), `rec_ring` (recorder).
`VfSink` has `vf_sink_flush()`; the monitor has `gain` and a switchable source (raw / processed).

**App modules** (`src/app`, all pure logic and tested): soundboard (lock-free replace: `enabled`/`busy` handshake in
`quiesce`), recorder (own thread, drops instead of blocking), abtest, hotkeys (GetAsyncKeyState polling; text form
"ctrl+alt+m"), stats (1 s samples x 1 h, quality verdict, dropout history), calibrate, undo (+snapshots), automute
(lock / idle; idle mute ends only by the user), appwatch (rule matching + process list), theme (data), overlay
(Windows layered window on its own thread), settings_ext (profiles, export/import text), `util/thread`, `dsp/spectrum`.
`src/audio/sysaudio.c`: WASAPI loopback -> two sinks (mix bus + AEC reference). `src/audio/vmic_shm.[ch]`: driver ring.

**GUI**: `src/gui_tools.inc` is included by main_gui.c (one translation unit). `ui_tick()` runs every loop iteration,
also with no window (hotkeys, mute, overlay, stats, per-program presets, per-mic profiles). Colours: `g_c[]` filled by
`apply_theme()`; use the `C_*` macros and `mixc()`; text on dark strips goes through `on_dark()`. Compact window is its
own drawing path (`draw_compact`). Switching theme at runtime: `retheme()` (must reset the raygui font first).
Screenshot flags: `--tab N` (0 main, 1 settings, 2 look), `--scroll PX` (main view scrolled down), settings.ini `theme = amber|classic`, `compact = 1`.
settings.ini gained [window] [mix] [board] [hotkeys] [apps] and app keys (theme, compact, overlay, per_mic_profiles,
auto_mute_*), monitor keys live in [audio_extra]. Old files still load (defaults for new keys).

**Echo canceller (beta)**: reference = system loopback; the program cannot know the loopback-to-microphone delay, so
`aec delay` is manual (0-200 ms) and the 2048 taps cover 43 ms after it. Double talk: Geigel + "residual far above what a
converged filter leaves" + 50 ms hold; latch `conv` after 1 s of > 10 dB reduction, unlatch after 10 s frozen. Synthetic
test: ERLE > 60 dB, near-end SDR ~7 dB in double talk. Real rooms are untested; one loopback device only.

**Driver**: not built (needs WDK + VM); see `driver/README.md`. **CI**: `.github/workflows/ci.yml` (never run; the Linux
code has still never been compiled).

**Performance (2026-11-13)**: `vf-bench` prints the CPU cost per effect and per configuration. Default chain 72 us per
10 ms frame (RNNoise int8 + AVX2 dispatch 57 us); `dsp/simd.[ch]` = AVX2/FMA dot and axpy with `__builtin_cpu_supports`
dispatch (portable fallback) - use it for any new long filter, store the weights reversed so both vectors run forwards;
the limiter uses a monotonic queue. RNNoise is built with `DISABLE_DEBUG_FLOAT` (int8 weights) and the upstream x86
dispatch; if a CPU lacks AVX2 the SSE4.1 / portable kernels are chosen automatically. New modules: `dsp/anc.[ch]`
(FxLMS skeleton, simulation-tested, not in the voice chain; limits in the header), `app/howl.[ch]` (monitor feedback
guard, setting `howl_guard`).

**Security round (2026-11-13)**: INI parser is bounded (4096 entries; `\r`, `\n`, `\r\n` end a line), WAV decode capped (30 min /
100 M samples), preset/profile names never map to Windows device names, settings are written without control characters,
`vf_harden_process()` drops the current directory from the DLL search path. `fuzz-ini` (tools/fuzz_ini.c, also a ctest with
30 000 inputs) mutation-fuzzes the INI parser and checks bounds, last-wins lookup and a write/parse round trip; run longer
with `build\fuzz-ini --iterations 5000000 --seed N`; `-DVF_LIBFUZZER` builds a libFuzzer entry point.
Hotkeys: `vf_hotkeys_grab()` (Windows, RegisterHotKey on a helper thread) swallows modifier combinations so the focused
program no longer sees ctrl+alt+m; polling (GetAsyncKeyState) still reads the state (verified). A combination another
program already owns is simply not swallowed; bare keys (no modifier) are never grabbed so they stay typeable.

**Check for updates** (Settings tab, About panel): `app/update.[ch]`, version in `app/version.h` (`VF_VERSION`, bump it with
each release; the tag of the newest GitHub release, e.g. `v1.0.1`, is compared numerically). Only on click: one HTTPS GET of
`/repos/vancek22/mic-thingy/releases/latest` (Windows WinHTTP, Linux `curl`), reduced to a validated tag; the page opened is
built from the tag, never taken from the response. 404 = "no release published yet" (state until a release exists).

**Windows search / Start menu**: `vf_startmenu_set()` (platform.c, IShellLink) keeps `%APPDATA%\Microsoft\Windows\Start Menu\Programs\mic thingy.lnk`
pointing at the running exe (created / repaired at GUI start while `[app] start_menu = 1`, the default; checkbox in Settings ->
Look and window removes it). A moved or rebuilt exe at another path rewrites the link on next start.

**Start menu shortcut target (2026-11-13)**: the `.lnk` points at a private copy, `%LOCALAPPDATA%\Programs\mic thingy\mic-thingy.exe`,
so moving or deleting a build folder does not break it. Any `mic-thingy.exe` started later refreshes that copy when it is newer
(a copy that is running is renamed to `.old` first). Turning the setting off removes only the shortcut, not the copy.
Autostart (Windows `Run` entry) uses the same stable copy: `vf_autostart_set_named` swaps the current exe for the copy (only when the
running file is called mic-thingy.exe, so tests and the CLI never copy themselves), and the GUI re-writes an existing entry at start.

## Auto setup + own neural denoiser (2026-11-14)
**Calibration = auto setup** (`app/calibrate.[ch]`, Tools tab): 5 s silence, then 15 s reading a displayed text
(`S_CAL_READ`, EN/PL). Feeds: meter frames (`vf_cal_frame`) + raw mic samples from the scope ring (`vf_cal_audio`,
the GUI turns the scope ring on while calibrating). Measures noise floor, mains hum (Goertzel 50/60 Hz + harmonics vs
neighbours, > 15 dB -> hum filter on), noise impulsiveness, pitch (autocorrelation, median), speech spectrum balance
(noise PSD subtracted) vs LibriSpeech targets `VF_CAL_T_*` (tools/nn/ltas.py), sibilance (p95 5-10 kHz level minus
median 300-3000 Hz level, > +3 dB -> de-esser on; Polish TTS measures ~0 dB). Proposes high-pass (0.55 x pitch, 60-140 Hz, >= 100 with rumble),
gate, RNNoise strength (by SNR), compressor, EQ (half-way to the target, only beyond 4 dB, +-6 dB), de-esser threshold.
Tests: `test_calibration_audio` (synthetic voice + hum + sibilants).

**"nn" effect (AI denoiser, beta, Extra tab, off by default)**: `dsp/fx_nn.c` + `dsp/nn.c` (GRU inference, torch gate
maths) + `dsp/fft960.c` (mixed radix 2/3/5) + generated `dsp/nn_weights.c`. 960 Vorbis frames, hop 480, 161 bins
(0-8 kHz) -> gains (above 8 kHz: mean gain of bins 140-160), floor param, overlap-add: delay 480 like RNNoise. Writes
`ctx->vad_prob` (sits after RNNoise in the chain, so its VAD wins when both run). Parity: `tests/nn_golden.h` (export.py)
-> test_fx checks features, gains, VAD vs torch (~1e-7) and perfect reconstruction with unity gains.
Training (`tools/nn/`, CPU torch in a venv, requirements.txt): `prep_speech.py` (LibriSpeech split -> int16 bank),
`nnlib.py` (noise zoo, augmentation, model), `train.py`, `export.py`, `evaluate.py` (held-out speakers through
mic-thingy-file, per noise family). Datasets/checkpoints/audio stay out of git. Results and iterations: `docs/NN_LOG.md`.
GUI pages: Extra = aec, hum, nn, dereverb, breath, guard; Effects = agc, deesser, warmth, voice (6 panels per page max -
the de-esser had been invisible before).
Start-menu copy: also refreshed when the size differs (an older-timestamped release zip no longer leaves the old copy).

## In-place update (2026-11-15)
"Update now" (Settings -> About, shown after "Check for updates" found a newer tag) replaces the running program without a
browser or zip (`app/update.[ch]`): downloads `releases/download/<tag>/mic-thingy.exe` (Linux `mic-thingy-linux`) as
`<exe>.new` plus `<file>.sha256`, checks size (256 KB..64 MB), file type (MZ / ELF) and SHA-256, swaps it in
(`vf_swap_file`: Windows renames the running exe to `<exe>.old`, which the next start deletes; Linux rename over it) and
"Restart now" starts `<exe> --after-update <pid>`, which waits for the old process (single-instance lock) before running.
Failure leaves the old program untouched; releases without those assets (<= v1.0.1) give `VF_UPD_NO_PACKAGE` and the page
button. The two assets are uploaded by `.github/workflows/release.yml`. The checksum is from the same release: it guards
against damaged downloads, not a compromised GitHub account. The download still transfers the whole program file (~4.7 MB,
no binary delta). Tests: `test_app` ("in-place update helpers": SHA-256 vectors, verify, swap); the real WinHTTP download
was checked against a public release asset (redirect, progress, 404, size cap), the swap against a really running exe.
`util/sha256.[ch]` is the SHA-256. Linux (curl + fork/exec) is written but never compiled here.

## nn effect weights: nsnet48k, 40 000 steps (2026-11-15)
`src/dsp/nn_weights.c` + `tests/nn_golden.h` now come from `ml/runs/nsnet48k_gpu/best.pt` (`python ml/export_c.py <ckpt>`;
step 40 000, val SI-SDR gain +8.01 dB; trained on the AMD GPU with MIOpen off, see ml/runs/train_gpu.py). test_fx parity
with torch passes. Held-out set (60 x 8 s, LibriSpeech test-clean + synthetic noise, SNR 0-20 dB, `ml/evaluate.py`), the
effect run through `mic-thingy-file` alone (RNNoise bypassed) vs the previous weights: DNSMOS OVRL 3.014 -> 3.042, BAK
3.864 -> 3.915, STOI 0.942 -> 0.947, pause attenuation 41.7 -> 42.8 dB (RNNoise alone: 2.971 / 3.734 / 0.922 / 9.8 dB;
noisy input 2.729 / 3.175 / 0.928). The torch models on the same set: nsnet48k 17.13 dB SI-SDR, erbdf48k (10k steps) 17.00 dB,
DNSMOS OVRL 3.025 / 3.040 - erbdf is not in the C code (a new conv + GRU + deep-filter engine would be needed for a
0.015 DNSMOS gain). SI-SDR of externally processed files is not comparable (delay alignment, high-pass, gate).

## nn effect: model choice nsnet / erbdf (2026-11-15)
Param `nn model` (toggle, default 0 = nsnet; 1 = erbdf; GUI checkbox "erbdf model (off = nsnet)", console `set nn model 1`, stored
in `[fx.nn]`). erbdf = `src/dsp/erbdf.[ch]` (C port of `ml/se/models/erbdf.py`: ERB-band conv + GRU gains, then a second GRU +
conv encoder/decoder predict 5 complex taps per bin that filter the stage-1 output over the last frames on bins 0-4.8 kHz;
bins above the net's band take the mean stage-1 gain) + generated `src/dsp/erbdf_weights.c` (`python ml/export_erbdf_c.py
ml/runs/erbdf48k/best.pt`, 10 000 training steps, val SI-SDR gain +8.06 dB) + `tests/erbdf_golden.h`. Both nets live in the
effect's state; `set_param` only flips the model and zeroes the newly chosen net's state. Delay 480 samples for both.
`test_fx` "erbdf denoiser": C vs torch streaming output on the golden signal differs by 1.7e-5 relative to the frame peak,
voice probability by 1.3e-7; also a run with model switches mid-stream (finite, bounded). Held-out set, effect alone through
`mic-thingy-file` (RNNoise bypassed): nsnet DNSMOS OVRL 3.042 / BAK 3.915 / STOI 0.947 / pause attenuation 42.8 dB, erbdf
3.044 / 3.924 / 0.945 / 50.5 dB (torch erbdf 3.040 / 3.892 / 0.949 / 53.5). `vf-bench`: nsnet 100.7 us per 10 ms frame (1.0 % of
a core), erbdf 151.7 us (1.5 %). erbdf got 10k steps vs nsnet's 40k - it is offered as a choice, nsnet stays the default.
The Linux build of this code is only compiled by CI.

## GUI customisation: the Look tab (2026-11-15)
Tab 5 "Look" (6 panels, `src/look_tab.inc`, included by gui_tools.inc) over the pure-logic module `app/look.[ch]` (tested in
`test_app` "look"): **colours** (the 20 theme colours; the 4th theme "custom" = user colours, editing a colour starts from the
theme on screen; swatch grid + R/G/B sliders), **accent** (8 presets, set accent + ON button colour), **chassis hue** shift
(-180..180, never moves ON/OFF/warning/error colours - tested at every 45 degrees for all themes: they stay distinct and the text
readable), **window** (opacity 40-100 %, always on top, tab at start), **level meters** (amber / red thresholds, peak hold time,
fall speed, live preview), **fonts** (UI + display font from the installed ones, live reload via `reload_fonts()`) and
**decoration** (background grid, display scanlines, glow), **panels shown** (hide effect panels; they only stop being drawn, the
effect keeps running), copy / paste the whole look as text through the clipboard (same `key = value` lines as `[look]`), and one
"Reset look". Stored in settings.ini `[look]` + `[app] theme` (app level: not part of presets or per-mic profiles); the new
keys are in `settings_hash()` (autosave detector - a field missing there would never be saved) and old files load with defaults.
UI scaling was left out on purpose (the layout is fixed pixels; a render-texture upscale would blur all text).
Checked: unit tests (clamps, hue invariants, hidden-panel list, text round trip, settings round trip, old and garbage files),
screenshots in EN/PL with a custom look applied through settings.ini, and a posted-mouse-message click test (accent swatch,
always-on-top box, hiding a panel -> settings.ini).

## "Update now" verified end to end (2026-11-15)
Installed release v1.0.3 (copied from the release asset into `%LOCALAPPDATA%\Programs\mic thingy`, sha256 matched) -> Settings -> Check
for updates -> "Version v1.0.3.5 is available" -> Update now: `mic-thingy.exe.new` + `.new.sha256` appeared, within ~1.5 s the old exe
became `mic-thingy.exe.old` and the new one took its place (sha256 = the v1.0.3.5 asset) -> "v1.0.3.5 installed - restart to use it" ->
Restart now: a process `mic-thingy.exe --after-update <old pid>` started, the old process was gone within 0.5 s, the `.old` file was
removed by the new instance, header "v1.0.3.5", settings untouched (clicks were posted mouse messages, window captured from the screen).
Gotcha when testing: ANY `mic-thingy.exe` started later (also a dev build or `--screenshot` run) refreshes the Start-menu copy
`%LOCALAPPDATA%\Programs\mic thingy\mic-thingy.exe` when its size or time differs - do not run dev builds while a release copy is
installed for an update test. Versions can have a 4th number (revision): v1.0.3.5 > v1.0.3, compared numerically.

## Look tab v2: styles, main colour, draggable panels, logo in the accent (2026-11-15, v1.0.4.0)
- **Styles** (panel 1): 12 ready-made looks (`vf_look_preset*` in app/look.c: the 3 built-in themes + 9 generated from a main
  colour + accent); the 20-colour editor moved behind "Edit single colours...". `[look] preset` remembers the last one ("" after
  a manual change).
- **Main colour** (panel 2): `vf_look_set_main()` regenerates the 13 chassis colours of the custom theme from one colour's hue and
  intensity (table `k_chassis`, measured on graphite: main #395cbf reproduces it within 24 RGB steps) times a brightness
  0.6..1.8; accent / OFF / warning / error stay. Accent chips set accent + ON button (`vf_look_set_accent`). Tests: every style
  keeps ON/OFF/warning apart and text readable, also at brightness 0.6 and 1.8.
- **Draggable panels**: press a panel's title strip (left of ON/OFF and Reset) and drop it on another place of the 3 x 2 grid,
  or on the Basic / Extra / Effects tab to move it there (max 6 visible per page; refused with a note). Order + page are
  `[look] layout = name:page,...` (`vf_look_layout` / `vf_look_move`, tested incl. junk input); only drawing changes, the audio
  chain order is untouched. "Reset panel order" on the Look tab. GuiLock() while carrying, so no slider grabs the mouse.
- **Logo**: `vf_icon_render_accent()` draws the header logo and the window icon with the theme's accent + ON-button colours,
  re-rendered by `retheme()` (`update_logo()`); the tray / overlay keep the default green art.
- New look keys are in `settings_hash()` (they were missing at first: layout changes were not autosaved - caught by the
  posted-mouse drag test). Dev screenshots/click tests now run a renamed copy (`mt-dev.exe`) so the installed Start-menu copy
  is not overwritten.

## One main view instead of Basic / Extra / Effects / Tools (2026-11-15)
Tabs are now Main / Settings / Look (`VF_LOOK_TABS` 3; `U.tab` 0/1/2). The main view is ONE scrolling 3-column grid with every
effect panel (default order: the old Basic, Extra, Effects pages) and the six tool panels (ids `t_mix`, `t_test`, `t_cal`,
`t_board`, `t_quality`, `t_spectrum`; `draw_tool(k, box, ...)` in gui_tools.inc, one branch per tool). Every panel is dragged
by its title strip and dropped on another grid place (`drag_panel` in look_tab.inc; carrying near the top / bottom edge
scrolls); the order is `[look] layout` (pages are always 0 now). `vf_look_flatten_layout()` rewrites a v1.0.4.0 per-tab layout
once (stable by old page, tools last - tested); old `start_tab` 3/4/5 map to 0/1/2. Tools can be hidden like effects.
Scrolling: wheel anywhere in the view except on a slider, or the scrollbar on the right edge; after a scroll step the wheel keeps
scrolling for 0.4 s (`U.scroll_latch`), so a slider that slides under the mouse is not changed (sliders set `U.wheel_used`).
The view is clipped with `BeginScissorMode` and the GUI is locked while the mouse is outside it (scrolled-out widgets cannot
take clicks; a lock that was already set, e.g. by an open dropdown, is kept). The spectrum ring runs while the spectrum panel is
drawn (`U.spectrum_shown`). Checked with posted mouse messages on a renamed dev copy: an old layout + start tab are migrated
and autosaved, 20 wheel steps (some over sliders) changed no [fx.*] value, a tool panel dragged at the end of the scroll range.
**Horizontal scrolling (v1.0.4.1)**: the main view is two rows, scrolled sideways (panel i sits in column i/2, row i%2; 3 columns
visible). The vertical wheel (or a sideways wheel) moves the row; the scrollbar is a thin strip under the panels (y 761, just
above the status bar); a carried panel scrolls the view near the left / right edge; `--scroll PX` = pixels scrolled right.
Same click test as above (wheel over sliders changed nothing, a tool dragged at the far right end, old layout migrated).

## Navigation modes + "Edit layout" (v1.1.0, 2026-11-15)
Settings -> Look and window -> "Moving between panels": Vertical (3 columns, scroll down, bar at the right edge) / Sideways (2 rows,
the default) / Pages (6 panels per page, "<" "Page x / n" ">" on the tab row; the wheel turns one page per gesture; a carried
panel held 0.7 s at the left / right edge flips the page, one empty page is offered at the end). Stored as `[look] nav` 0/1/2
(the theme chooser left the Settings tab - themes are on the Look tab). Geometry: `slot_rect()` / `slot_at()` in look_tab.inc.
"Edit layout" (tab row, every mode) shows every panel, hidden ones too, as a 140 x 62 tile in 6 columns (`draw_layout_editor`):
title, ON/OFF of effects, the page a tile lands on (page mode), a tick that shows / hides the panel; drag a tile onto another to
move it (before it, or after it when moving forward). Dragging panels by their title keeps working in all modes.
Checked with posted clicks on a renamed dev copy: Settings -> Pages, ">" to page 2, Edit layout, a tile dragged forward and one
hidden -> settings.ini `nav = 2`, the new `layout`, `hidden = rnnoise`.

## Security / efficiency / leak round (v1.1.0.1, 2026-11-15)
- **DLL planting**: the .exe imports only Windows "known DLLs" now (kernel32, user32, gdi32, advapi32, shell32, ole32 + the UCRT
  API sets). `winhttp.dll` (update.c), `dwmapi.dll` and `winmm.dll` (win_chrome.c; raylib's `timeBeginPeriod` / `timeEndPeriod` land
  in our `__imp_` pointers) are loaded on first use with `vf_load_system_library()` = `LoadLibraryExW(LOAD_LIBRARY_SEARCH_SYSTEM32)`,
  and `vf_harden_process()` calls `SetDefaultDllDirectories(SYSTEM32 | USER_DIRS)`, which also covers GLFW's and miniaudio's
  `LoadLibrary("name.dll")`. Never add a library to `target_link_libraries` that is not a known DLL - check with
  `objdump -p build/mic-thingy.exe | grep "DLL Name"`. Test: garbage `winmm.dll` / `dwmapi.dll` / `winhttp.dll` / `xinput1_4.dll` next
  to the program - v1.1.0 does not start, this build does; the update check works next to a fake `winhttp.dll`.
- **Build hardening** (CMakeLists.txt, all targets incl. vendored): `-fstack-protector-strong`; Windows `-static` (else WinLibs GCC
  pulls in `libssp-0.dll`) + explicit `--dynamicbase --nxcompat --high-entropy-va`; Linux `-z relro -z now -z noexecstack`.
  `vf-bench` unchanged (default chain 72 us per frame).
- Linux: the autostart `.desktop` `Exec=` path is escaped (`"` `` ` `` `$` `\` `%`), the update check runs curl with `--proto =https`.
- GUI: device indices are clamped after a re-enumeration that shortened the list; a running recording / A-B capture is re-attached
  to the new engine after a stream restart (`engine_init` zeroes the engine, they used to wait forever); a dropped `.ini` over
  1 MB is refused. `vf_ring_write/read` copy in two `memcpy` blocks.
- **Leak soak** (renamed dev copy, `start_processing = 0`, 60 s open + 20 x hide to tray / show via posted WM_CLOSE and WM_APP+2):
  private bytes 192-193 MB open / 124-125 MB in the tray on every cycle, handles 394-398 / 356-360, GDI objects 30 / 19 - flat.
  USER objects grew by exactly 1 per hide/show cycle (15 -> 35 over 20 cycles) - FIXED after the release, see below. Tray CPU 0.00 %.
  Started hidden (`--minimized`, never opened): 3 MB private. After the window was open once the tray keeps ~125 MB of PRIVATE
  bytes (the AMD GL driver does not give its heaps back after `CloseWindow`; only the working set is trimmed) - the 0.8-2.3 MB
  in the M1 note is working set. Fixing that needs the GUI in its own process (M2).
- Not changed, by reasoning: the update's SHA-256 comes from the same release (no code signing); miniaudio decodes dropped
  sound files (user action); CPU of hum (+20 us), warmth (+17), voice (+18) - would change the sound.
- No sanitizers here (MinGW GCC has none, no WSL): memory safety was checked by reading + the tests + the soak above.

**USER object leak fixed (after v1.1.0.1):** the +1 per hide/show cycle came from vsync. A bare raylib loop
(InitWindow / frames / CloseWindow, scratch test) grows by one USER object per cycle with `FLAG_VSYNC_HINT` alone and stays flat
without it; `ClearWindowState(FLAG_VSYNC_HINT)` before `CloseWindow` or turning vsync on after creation does not help - the
graphics driver keeps one object per GL context on `wglSwapIntervalEXT(1)` (AMD driver here). `window_open` no longer asks for vsync
(`SetConfigFlags(0)`; the frame rate comes from `SetTargetFPS`, which raylib applies with or without vsync). Soak after the fix:
20 cycles, USER 13 in the tray / 26 open on every cycle, GDI 19 / 30, handles 352 / 390. CPU with the focused window open
unchanged (~13-14 % of one core, mostly raylib's partial busy-wait frame limiter, same before). Private bytes were ~50 MB lower in
this soak, but the old build also measured 113 MB in one CPU run, so that is not a reliable gain. Also: `SetMouseCursor` is called
only when the cursor changes (`g_cursor_set`) - raylib creates a new GLFW cursor struct on every call, freed only by CloseWindow.
How it was found: `GetGuiResources(GR_USEROBJECTS)` logged after each step of `window_open` / `window_close` (temporary), then the
raylib-only loop with one flag at a time. Window classes and counts (EnumWindows) stayed the same every cycle, so it was never a window.

**Echo canceller added the system sound in the pauses (fixed after v1.1.0.2):** user report - "oscillation / harmonising" when they
stop talking and type or there is background sound (their settings: aec ON, delay 40, strength 0.4, nn erbdf, gate 0.79).
Cause: on a headset there is NO echo path, but NLMS still adapted on voice / key clicks the double-talk detectors missed, the
weights learned garbage and `e = d - y` subtracted a filtered copy of the system sound - in the pauses up to 18-27 dB LOUDER than
the microphone itself (real recordings: Polish speech + keyboard as the mic, English babble as the reference). Fix in
`fx_aec.c`: (1) step size `mu e / (en + AEC_NEAR_K * TAPS * pe)` - the power the reference does not explain slows adaptation
(K = 30: 10 / 30 / 100 compared; 30 kept ERLE 66.6 dB and the best real-recording numbers); (2) the estimate is subtracted only
by `apply` = smoothed projection <d,y>/<y,y> clamped 0..1 (true echo ~1, unrelated estimate ~0), ramped per frame, and cut to
this frame's own projection when the smoothed share would make the frame louder than the mic by > 10 % of its energy. NO weight
decay: a first version shrank the weights while the share was < 0.3 - with speakers, when the far end went quiet (-56 dBFS, not
silent) while the user talked, the echo was then uncancelled for 5+ s after it resumed (0.3 dB instead of 61 dB); removing it
changed nothing measurable in the headset case. Results, real recordings, ref at -32 / -21 / -12 dBFS: added in the pauses
v1.1.0.2 +14.5 / +23.6 / +27.2 dB rel. mic (-52 / -43 / -39 dBFS) -> fixed -7.5 / -7.4 / -8.2 dB (-74 dBFS, mic -66); while
speaking -5.9 / +7.3 / +10.5 -> -24.8 / -13.1 / -4.7 dB. Synthetic room echo test: ERLE 67.9 -> 66.6 dB, double-talk SDR 8.2 -> 7.9 dB.
New `test_fx` case "no echo path (headset)": music reference, voice then keys, the canceller must change the mic by < -20 dB in
the keys part (old: +12.2 dB, fails; new -26.3 dB). Second new case "speakers, far end quiet for 3 s": echo reduction in the
first second after it resumes must be > 20 dB (60.9 dB; the decay version: 0.3 dB, fails). CPU: `+ aec` ~130-145 us per frame.
Full chain with the user's settings (scratch harness: `chain_build` + their [fx.*] as console commands + a reference sink;
`mic-thingy-file` has no loopback, there the canceller is inert): mic = speech, then 7 s of keys at -41 dBFS, reference = babble.
v1.1.0.2: keys-only output -47 / -52 / -58 dBFS for a reference at -32 / -21 / -12 dBFS, the gate opened on 22 / 12 / 41 % of
those frames (nn's VAD reacted to the injected voices), 0.3 s after speech -30 / -27 / -48 dBFS. Fixed: keys-only -162 / -138 /
-128 dBFS (gate never opens), 0.3 s after speech -43 dBFS. Without a reference the rest of the chain (erbdf, gate 0.79, breath)
silences the keys completely (-163 dBFS) - so the artifact only exists while the PC plays sound. `mic-thingy-file` takes up to
128 `-c` commands now (a full [fx.*] section is ~70).
Not done: real-room echo was never recorded here; the user still has to listen. With headphones the canceller is not needed at all.
