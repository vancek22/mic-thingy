# Third-party notices

mic thingy itself is released under the MIT License (see [LICENSE](LICENSE)).
It bundles the following third-party code in `third_party/`, each under its own
permissive license. When you distribute binaries, include this file and the
license texts referenced below.

| Component | Version | License | License text | Used for |
|---|---|---|---|---|
| [miniaudio](https://miniaud.io) by David Reid | 0.11.26 | Public domain (Unlicense) or MIT-0, your choice | [third_party/miniaudio/LICENSE](third_party/miniaudio/LICENSE) | Audio I/O (WASAPI, PulseAudio, ALSA), WAV I/O |
| [RNNoise](https://github.com/xiph/rnnoise) by Jean-Marc Valin, Xiph.Org, Mozilla, Amazon | v0.2 + model `rnnoise_data-0b50c45` | BSD-3-Clause | [third_party/rnnoise/COPYING](third_party/rnnoise/COPYING) | Noise suppression and voice activity detection. Locally modified - see [third_party/rnnoise/PATCHES.md](third_party/rnnoise/PATCHES.md) |
| [raylib](https://www.raylib.com) by Ramon Santamaria | 5.5 | zlib | [third_party/raylib/LICENSE](third_party/raylib/LICENSE) | GUI window, rendering, input (built without its audio module) |
| [GLFW](https://www.glfw.org) (bundled inside raylib) | 3.4 | zlib | [third_party/raylib/src/external/glfw/LICENSE.md](third_party/raylib/src/external/glfw/LICENSE.md) | Windowing (via raylib) |
| [stb](https://github.com/nothings/stb) single-file libraries (bundled inside raylib) | - | Public domain or MIT, your choice | end of each `third_party/raylib/src/external/stb_*.h` file | Fonts/images in raylib; PNG output in `tools/make_icon.c` |
| [raygui](https://github.com/raysan5/raygui) by Ramon Santamaria | 4.0 | zlib | [third_party/raygui/LICENSE](third_party/raygui/LICENSE) | GUI controls |

Notes

- The RNNoise BSD-3-Clause license requires its copyright notice and disclaimer
  to be reproduced in the documentation of binary distributions - ship
  `third_party/rnnoise/COPYING` (or this file plus that text) with release builds.
- Fonts are not bundled: the GUI loads system fonts at runtime (Tahoma / Lucida
  Console on Windows, DejaVu or Noto on Linux) and falls back to raylib's
  built-in font.
- VB-Audio Virtual Cable (Windows virtual microphone) is a separate product by
  VB-Audio Software and is not bundled; users install it themselves.
- The icon and logo (`assets/`) are generated from `src/app/icon_art.c` and are
  part of mic thingy (MIT).
