# Local changes to RNNoise v0.2 (xiph/rnnoise tag v0.2, commit 904a876)

Model weights: `rnnoise_data-0b50c45` (`src/rnnoise_data.c/.h`), compiled in.

1. `rnnoise_set_lookahead(st, enabled)` (`include/rnnoise.h`, `src/denoise.c`).
   Upstream applies the gains of frame *k* to the spectrum of frame *k-1* (`delayed_X`),
   a one-frame look-ahead that makes the output 2 frames (20 ms) late. With look-ahead
   disabled the gains are applied to the current frame, so the delay is 1 frame (10 ms,
   the overlap-add minimum). The library default (enabled) is upstream behaviour, bit-exact;
   VoiceFilter disables it by default (`rnnoise lookahead` parameter = 0).
