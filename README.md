# VoiceFilter

Filtr mikrofonu w czasie rzeczywistym (C11, Windows 11 + Linux/PipeWire), w stylu NVIDIA Broadcast / SteelSeries Sonar.

```
mikrofon → high-pass → RNNoise → bramka (VAD) → kompresor → korektor 3-pasmowy → limiter → wyjście
```

Wyjście: słuchawki, **wirtualny mikrofon** (Windows: VB-Cable, w aplikacji wybierasz „CABLE Output”;
Linux: źródło PipeWire „VoiceFilter Microphone”) albo wirtualny mikrofon + odsłuch na słuchawkach.

## Budowanie

Windows (MinGW-w64 GCC, CMake, Ninja — `winget install BrechtSanders.WinLibs.POSIX.UCRT Kitware.CMake Ninja-build.Ninja`):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Linux (Debian/Ubuntu):

```
sudo apt install build-essential cmake ninja-build pkg-config libpipewire-0.3-dev \
     libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
```

Fedora: `gcc cmake ninja-build pkgconf pipewire-devel libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel mesa-libGL-devel`.

Opcje CMake: `-DVF_GUI=OFF` (bez GUI), `-DVF_PIPEWIRE=OFF` (Linux bez wirtualnego mikrofonu).

## Użycie

- `build/voicefilter` — GUI (domyślnie po angielsku, przełącznik EN/PL w prawym górnym rogu): urządzenia,
  tryb wyjścia, suwaki efektów, wskaźniki, presety, autostart.
  Ustawienia zapisują się same (`%APPDATA%\VoiceFilter\settings.ini`, `~/.config/voicefilter/settings.ini`),
  presety użytkownika w podkatalogu `presets/`.
- **Praca w tle:** zaznacz „Start z systemem” — po zalogowaniu VoiceFilter uruchomi się schowany w zasobniku
  (ikona mikrofonu przy zegarze) i od razu zacznie filtrować. Z „Działaj w tle” zamknięcie okna (X) tylko je chowa;
  filtr działa dalej, dopóki nie wybierzesz „Zakończ VoiceFilter” z menu ikony (prawy przycisk). Kliknięcie ikony
  lub ponowne uruchomienie programu pokazuje okno. Po odłączeniu urządzenia program sam próbuje je ponownie otworzyć.
  Na Linuksie nie ma ikony w zasobniku — okno zamiast tego się minimalizuje.
- `build/voicefilter-cli` — wersja konsolowa (`--help`): `--vmic`, `--monitor N`, `--preset NAZWA`,
  klawisze `b` (bypass), `1..6` (efekty), `:` (komendy `show`, `set comp ratio 4`, `bypass gate`, `reset`).
- Narzędzia testowe: `voicefilter-file` (przetwarzanie pliku), `vf-eval` (pomiary), `vf-latency`, `vf-record`
  — opis w `CLAUDE.md`.

## Opóźnienie

Przetwarzanie dodaje ok. 11,5 ms (RNNoise 10 ms + limiter 1,5 ms). Całość z buforami WASAPI i VB-Cable
to zwykle 50–100 ms — cel 20 ms nie jest osiągalny w trybie współdzielonym WASAPI z VB-Cable.
Szczegóły pomiarów: `CLAUDE.md`.

## Licencje i podziękowania

| Biblioteka | Licencja | |
|---|---|---|
| [miniaudio](https://miniaud.io) 0.11.26 | public domain / MIT-0 | `third_party/miniaudio/LICENSE` |
| [RNNoise](https://github.com/xiph/rnnoise) v0.2 (Xiph.Org, Jean-Marc Valin) | BSD-3-Clause | `third_party/rnnoise/COPYING`, zmiany: `third_party/rnnoise/PATCHES.md` |
| [raylib](https://www.raylib.com) 5.5 | zlib | `third_party/raylib/LICENSE` |
| [raygui](https://github.com/raysan5/raygui) 4.0 | zlib | `third_party/raygui/LICENSE` |

Wszystkie licencje są permisywne. Przy dystrybucji binarnej dołącz treść licencji BSD RNNoise.
