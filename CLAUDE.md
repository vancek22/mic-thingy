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
Screenshot flags: `--tab N` (0..4), settings.ini `theme = amber|classic`, `compact = 1`.
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
