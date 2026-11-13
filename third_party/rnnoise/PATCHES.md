# Local changes to RNNoise v0.2 (xiph/rnnoise tag v0.2, commit 904a876)

Model weights: `rnnoise_data-0b50c45` (`src/rnnoise_data.c/.h`), compiled in.

1. `rnnoise_set_lookahead(st, enabled)` (`include/rnnoise.h`, `src/denoise.c`).
   Upstream applies the gains of frame *k* to the spectrum of frame *k-1* (`delayed_X`),
   a one-frame look-ahead that makes the output 2 frames (20 ms) late. With look-ahead
   disabled the gains are applied to the current frame, so the delay is 1 frame (10 ms,
   the overlap-add minimum). The library default (enabled) is upstream behaviour, bit-exact;
   VoiceFilter disables it by default (`rnnoise lookahead` parameter = 0).

2. `rnnoise_analyze_frame(st, in, gains)` and `rnnoise_get_freq_size()` (`include/rnnoise.h`, end of
   `src/denoise.c`). Same analysis + network + pitch filter + gain smoothing as the no-look-ahead
   branch of `rnnoise_process_frame()`, but the per-bin gains (481 bins, 50 Hz apart) are returned
   instead of being applied and overlap-added. mic thingy turns them into a minimum-phase FIR and
   filters the current frame directly ("low latency" mode, `src/dsp/fx_rnnoise.c`): no 10 ms
   overlap-add delay. Do not mix with `rnnoise_process_frame()` on one state (the synthesis memory
   is not updated).

3. Build configuration (CMakeLists.txt, no source change): `DISABLE_DEBUG_FLOAT` drops the float copies of the
   network weights, so the int8 weights (the production model) are used: network 92 -> 52 us per frame with the
   portable C kernels, 18 us with the AVX2 kernels. The x86 run-time dispatch (SSE4.1 / AVX2+FMA, upstream
   `src/x86/`) is enabled on x86-64 GCC/Clang builds (`VF_RNNOISE_SIMD`); `src/x86/celt_lpc_sse.h` is an empty
   stub (the SSE LPC helpers are not vendored, the C versions are used). Audio matrix, float vs int8 model:
   SDR 13.07 / 13.07 dB, pause attenuation 23.19 / 23.16 dB, onset loss -6.58 / -6.55 dB - identical.
