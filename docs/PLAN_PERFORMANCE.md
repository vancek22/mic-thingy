# Plan: mniejsze opóźnienie, mniej pamięci, nowe funkcje

Plan wykonawczy dla modelu Opus. Każdy etap kończy się pomiarem i commitem; etap, który nie
daje mierzalnej poprawy, wycofujemy zamiast go zostawiać. Zasady z `CLAUDE.md` obowiązują
(zero malloc/mutex/I/O w callbacku audio, `ctest` po każdej zmianie DSP/silnika, domyślne
ustawienia dźwięku zmieniamy tylko za zgodą użytkownika).

## 1. Stan wyjściowy (pomiary z 2026-11-12, Windows 11, Radeon RX 7700 XT)

| Co | Wynik | Uwagi |
|---|---|---|
| CPU łańcucha DSP | 120 s dźwięku w 0,92 s CPU = ok. 77 µs na ramkę 10 ms (0,8% jednego rdzenia) | `mic-thingy-file` na `noise_hum.wav`. CPU nie jest przyczyną opóźnienia. |
| CPU GUI | 17,7 s CPU na 571 s działania = ok. 3% jednego rdzenia | rysowanie 60 kl./s bez przerwy |
| Pamięć GUI (okno otwarte) | working set 106 MB, private bytes 165 MB | sterownik OpenGL AMD `atio6axx.dll` sam mapuje 80 MB |
| Pamięć samego audio | working set 10,5 MB, private 2,3 MB | `vf-record` (miniaudio + WASAPI, bez RNNoise); wagi RNNoise dodadzą ok. 6 MB |
| Opóźnienie DSP | 11,5 ms = RNNoise 10 ms + limiter 1,5 ms | plus FIFO 0–10 ms (`480 - gcd(blok, 480)`), dziś 5 ms przy okresie 240 |
| Pętla przez VB-Cable | 60–88 ms | `vf-latency`; mierzy odtwarzanie + kabel + nagrywanie, bez prawdziwego mikrofonu |

Wnioski:
- **Pamięć**: prawie całość to kontekst OpenGL i sterownik karty. Sam silnik audio mieści się w ok. 15–20 MB.
- **Opóźnienie**: DSP to 11,5 ms, reszta (ok. 50–70 ms) siedzi w buforach WASAPI, miniaudio i VB-Cable.
  Nie wiemy dokładnie, który bufor ile zabiera, bo `vf-latency` tego nie rozdziela.

## 2. Cele i co jest realne

| Cel | Realny? | Jak |
|---|---|---|
| Opóźnienie dodane przez DSP < 10 ms | Tak, do ok. 0–2 ms | tryb „low latency” (etap L2) |
| Pamięć w tle (tray) < 40 MB | Tak | etap M1, w razie potrzeby M2 |
| Pamięć z otwartym oknem < 40 MB | Niepewne | sterownik GL zabiera 80+ MB; tylko renderer programowy (M3) |
| Mikrofon → słuchawki < 10 ms na Windows | Raczej nie | okres przechwytywania WASAPI shared na tej maszynie to 10 ms; realnie 15–25 ms |
| Mikrofon → Discord przez VB-Cable < 10 ms | Nie | kabel i bufory aplikacji odbierającej są poza naszą kontrolą; realnie 25–40 ms |
| Mikrofon → wirtualne źródło PipeWire < 10 ms (Linux) | Prawdopodobnie tak | quantum 128–256 + tryb low latency; do sprawdzenia na Linuksie |

Raportujemy uczciwie liczby z pomiaru, także wtedy, gdy cel nie wyjdzie.

## 3. Opóźnienie

### L0. Budżet opóźnienia (najpierw pomiar)
Bez tego pozostałe etapy są zgadywaniem.
- `src/audio/engine.c`: w callbacku zapisywać do atomików rozmiar bloku (min/max/ostatni), wypełnienie
  `out_fifo`, a po starcie odczytać z miniaudio faktyczne `internalPeriodSizeInFrames` i `internalPeriods`
  dla capture i playback. Dodać `engine_latency()` → pełny rozkład (capture, FIFO, DSP, playback).
- `tools/vf_latency.c`: nowy tryb `--through`, który mierzy pętlę przez działający silnik
  (`mic-thingy-cli --in-file` gra impulsy do CABLE Input, narzędzie nagrywa CABLE Output), oraz wypisuje
  tabelę „który bufor ile ms”.
- GUI/CLI: pokazać rozkład zamiast jednej liczby.
- Wynik etapu: tabela w `CLAUDE.md` z rozbiciem obecnych 60–88 ms.

### L1. Bufory urządzeń (tanie, największy zysk)
Zmiany w `engine_init()` (`src/audio/engine.c:112`), każdą mierzyć osobno 6 przebiegami `vf-latency`:
1. `cfg.noFixedSizedCallback = MA_TRUE` – silnik i tak przyjmuje dowolne bloki, a miniaudio przestaje
   doklejać własny bufor pośredni.
2. `cfg.wasapi.usage = ma_wasapi_usage_pro_audio` – wątek audio w MMCSS „Pro Audio”, mniej zacięć,
   więc można zejść z buforami.
3. `cfg.noPreSilencedOutputBuffer = MA_TRUE`, `cfg.noClip = MA_TRUE` (limiter już pilnuje sufitu).
4. Sprawdzić w logu miniaudio (`MA_DEBUG_OUTPUT`), czy `IAudioClient3_InitializeSharedAudioStream`
   faktycznie działa dla obu urządzeń i jaki okres dostajemy; częstotliwość urządzenia inna niż 48 kHz
   wyłącza ten tryb i włącza resampler – wtedy pokazać ostrzeżenie w GUI.
5. `periods = 1` zamiast 2 dla playback, okres playback 128–160.
6. Priming FIFO: dziś stałe `480 - gcd(B, 480)`. Zamienić na adaptacyjne: start od 0, podnosić o brakujące
   próbki dopiero po niedoborze (jak `VfSink`). Przy bloku capture 480 daje to 0 ms.
7. Tryb vmic: zamiast duplexu miniaudio spróbować „capture → tap → `VfSink` → osobne urządzenie playback”
   z `target` 240 zamiast 960. Zostawić wariant, który wygra w pomiarze.
8. Dokumentacja dla użytkownika: w VB-Cable Control Panel zmniejszyć „Max Latency” (domyślnie 7168 próbek)
   do 1024–2048; opisać w README, bo tego nie ustawimy z programu.

Kryterium: pętla przez VB-Cable poniżej 45 ms bez niedoborów przez 10 minut (`xruns = 0`).

### L2. Tryb „low latency” w DSP (cel: DSP ≤ 2 ms)
RNNoise potrzebuje ramki 480 próbek i nakładania okien, więc w torze sygnału zawsze kosztuje 10 ms.
Rozwiązanie: RNNoise jako **analizator w torze bocznym**, a dźwięk idzie bez opóźnienia przez filtr,
którego wzmocnienia pasm pochodzą z RNNoise.
- Patch RNNoise (`third_party/rnnoise/src/denoise.c`, opisać w `PATCHES.md`): funkcja
  `rnnoise_get_gains(st, float *gains, int n)` zwracająca wzmocnienia pasm z ostatniej ramki.
- Nowy efekt `src/dsp/fx_rnnoise_ll.c` (albo parametr `mode` w `fx_rnnoise`): co ramkę z wzmocnień
  budować minimalnofazowy FIR (64–128 współczynników, metoda cepstralna, FFT z kiss_fft już w repo),
  przełączać współczynniki z przenikaniem 5 ms. Splot liczony próbka po próbce, czyli efekt przestaje
  wymagać ramek 480 w torze sygnału.
- Skutek uboczny: wzmocnienia spóźniają się o 10–20 ms względem dźwięku. Na początku słowa przez chwilę
  tłumienie jest jak dla ciszy, a krótkie stuki (klawiatura) przechodzą słabiej wytłumione.
  Zmierzyć macierzą audio (`run_audio_tests.ps1 -Tag ll`): SDR, tłumienie w pauzach, utrata początków słów.
- Limiter: parametr `lookahead` (0 = natychmiastowy atak z miękkim obcinaniem, 0 ms).
- Silnik: gdy cały łańcuch nie wymaga ramek, przetwarzać blok urządzenia bez FIFO (analizator dalej
  zbiera 480 próbek po cichu). `Fx` dostaje flagę „potrzebuję ramek 480”.
- Bramka: VAD też spóźniony o ramkę – wydłużyć domyślny `hold` w tym trybie albo otwierać bramkę
  dodatkowo detektorem poziomu.
- Przełącznik w GUI/CLI/presetach: „Jakość (11,5 ms)” / „Niskie opóźnienie (≈1 ms)”. Domyślny zostaje
  „Jakość”, dopóki użytkownik nie posłucha i nie zdecyduje.
- Testy w `tests/test_dsp.c`: zmierzone opóźnienie łańcucha w trybie LL ≤ 96 próbek, brak NaN,
  stabilność przy skokach wzmocnień, zgodność odpowiedzi FIR z zadanymi wzmocnieniami (±1 dB).

### L3. Opcjonalnie
- WASAPI exclusive jako ukryta opcja tylko dla trybu słuchawek (wcześniejszy pomiar wypadł gorzej –
  powtórzyć po L1 z `noFixedSizedCallback`).
- RNNoise z AVX2 (ścieżka RTCD upstreamu): CPU spadnie ok. 2×, ale to 77 µs na ramkę, więc niski priorytet.

## 4. Pamięć

Metryka: kolumna „Pamięć” w Menedżerze zadań (private working set) oraz private bytes, po 60 s działania.
Dodać skrypt `tools/measure_mem.ps1`, żeby pomiar był powtarzalny.

### M1. Zwalnianie okna w tle (cel: tray < 40 MB)
- `src/main_gui.c:438`: usunąć `FLAG_MSAA_4X_HINT` (interfejs jest prostokątny; krawędzie logo są już
  wygładzane programowo). Zmierzyć zysk.
- Po schowaniu do traya: `CloseWindow()` (niszczy kontekst GL, tekstury i fonty), pętla główna przechodzi
  w tryb bez okna (`Sleep(100)`, obsługa traya, ponawianie urządzeń, autozapis). „Pokaż okno” robi
  `InitWindow()` + `load_fonts()` + `apply_style()` + tekstura logo. Start z `--minimized` w ogóle nie
  tworzy okna. Wymaga wydzielenia stanu GUI z `main()` do struktury, żeby przeżył zamknięcie okna.
- Po zamknięciu okna wywołać `SetProcessWorkingSetSize(GetCurrentProcess(), -1, -1)`.
- Sprawdzić, czy `atio6axx.dll` się wyładowuje i ile zostaje private bytes. Jeśli < 40 MB – koniec.

### M2. Dwa procesy (jeśli M1 nie wystarczy)
- `mic-thingy` = mały proces bez raylib: silnik audio, tray, ustawienia (ok. 15–20 MB zawsze).
- `mic-thingy-ui` = okno raylib uruchamiane na żądanie, zamykane przy schowaniu.
- Komunikacja: nazwane mapowanie pamięci (`CreateFileMapping` / `shm_open`) ze strukturą z atomikami –
  te same `params[fx][id]`, bypassy, liczniki oraz ring metrów (`Pipeline.meter_ring` jest już SPSC bez
  blokad, wystarczy przenieść go do pamięci współdzielonej). Polecenia rzadkie (zmiana urządzenia,
  preset, start/stop) przez małą kolejkę SPSC w tym samym mapowaniu.
- Zysk dodatkowy: awaria GUI nie przerywa dźwięku.

### M3. Okno otwarte < 40 MB (tylko jeśli użytkownik tego chce)
Wymaga rezygnacji ze sterownika OpenGL: renderer programowy (sprawdzić, czy nowsze raylib ma backend
programowy `rlsw`; alternatywnie własny rysunek przez GDI). Interfejs to prostokąty i tekst, więc
jest to wykonalne, ale to duża zmiana – nie robić bez decyzji użytkownika.

### M4. Drobne
- Rysować tylko, gdy coś się zmieniło, albo 30 kl./s; 10 kl./s gdy okno nie ma fokusu (CPU GUI 3% → < 1%).
- Narzędzia testowe nie linkują niczego ponad potrzebę (już tak jest); wagi RNNoise (6 MB w .exe)
  są współdzielone z dyskiem i nie liczą się do private bytes – nie ruszać.

## 5. Kolejność i kryteria

1. L0 (pomiar) → 2. L1 (bufory) → 3. M1 (pamięć w tle) → 4. L2 (tryb low latency) → 5. M2 tylko w razie
potrzeby → 6. nowe funkcje → 7. własny sterownik wirtualnego mikrofonu (sekcja 7, po decyzji o podpisie).

Po każdym etapie: `cmake --build build`, `ctest --test-dir build`, pomiar (opóźnienie albo pamięć),
wpis z liczbami do `CLAUDE.md`, commit. Użytkownik odsłuchuje tryb LL przed zmianą domyślnych ustawień.
Kod linuksowy nadal jest niekompilowany na tej maszynie – każdą zmianę w silniku pisać przenośnie
i zaznaczyć, co trzeba sprawdzić na Linuksie.

## 6. Propozycje nowych funkcji

| # | Funkcja | Opis | Rozmiar |
|---|---|---|---|
| F1 | Test A/B w programie | „Nagraj 10 s” → odsłuch surowego i przefiltrowanego na zmianę, bez Discorda. Korzysta z `wavio` i tapów silnika. | mały |
| F2 | Globalny skrót mute / push-to-talk | `RegisterHotKey` (Windows), wyciszenie z łagodnym zboczem, kolor ikony w trayu. | mały |
| F3 | Kreator kalibracji | 5 s ciszy + 5 s mowy → automatycznie próg bramki, próg kompresora i makeup. | średni |
| F4 | Wyrównanie głośności (AGC) | Nowy moduł `fx_agc`: powolne dociąganie do docelowego poziomu, działa tylko gdy VAD wykrywa mowę. | średni |
| F5 | De-esser | Nowy moduł `fx_deesser`: kompresja pasma 5–9 kHz sterowana detektorem sybilantów. | mały |
| F6 | Tłumik stuków klawiatury | Detektor transjentów + VAD: krótkie stuki poza mową ściszane mocniej niż robi to RNNoise. Szczególnie przydatny w trybie LL. | średni |
| F7 | Analizator widma | Retro wyświetlacz widma wejścia/wyjścia w GUI (FFT w wątku UI z ringu próbek). | mały |
| F8 | Presety per aplikacja | Automatyczna zmiana presetu, gdy działa wskazany program (np. gra → „Głośne otoczenie”). | średni |
| F9 | CI i paczki wydań | GitHub Actions: build + `ctest` na Windows i Linuksie, artefakt .zip. Pierwsza prawdziwa kompilacja kodu linuksowego. | mały |
| F10 | Redukcja echa (AEC) | Usuwanie dźwięku z głośników z sygnału mikrofonu; wymaga nowej biblioteki (np. speexdsp, BSD). | duży |

Sugerowana kolejność: F9 (zabezpiecza resztę prac), F1, F2, F4, F6, potem pozostałe.

Kolejne 19 funkcji (N1–N19) wraz z planem wdrożenia: `docs/PLAN_FEATURES.md`.

## 7. Własny sterownik wirtualnego mikrofonu (Windows, zamiast VB-Cable)

Cel: użytkownik nie instaluje VB-Cable; w systemie pojawia się urządzenie nagrywające
„mic thingy microphone”, do którego program pisze dźwięk bezpośrednio. Znika urządzenie odtwarzające
„CABLE Input”, bufor kabla i jedna para buforów WASAPI, więc opóźnienie do aplikacji spada o
szacunkowo 20–40 ms (do potwierdzenia pomiarem po L0).

### Ograniczenia, które trzeba znać przed startem
- **Podpis sterownika.** Windows 11 ładuje tylko sterowniki jądra podpisane przez Microsoft
  (attestation signing w Partner Center, wymaga certyfikatu EV, ok. 300–500 USD rocznie, i firmy lub
  osoby zweryfikowanej). Bez tego sterownik działa wyłącznie w trybie testowym
  (`bcdedit /set testsigning on`), który wyświetla znak wodny na pulpicie i bywa blokowany przez
  anty-cheaty gier. Dla projektu open source bez budżetu oznacza to: sterownik tylko dla chętnych,
  VB-Cable zostaje jako domyślna ścieżka.
  **Decyzja użytkownika (2026-11-12): tylko tryb testowy.** Nie kupujemy certyfikatu; sterownik jest
  opcją dla chętnych, VB-Cable pozostaje domyślną i w pełni wspieraną ścieżką. W D5 robimy wyłącznie
  wariant B.
- **Inny kompilator.** Sterowniki buduje się MSVC + Windows Driver Kit (WDK), nie MinGW. Sterownik
  będzie osobnym projektem w `driver/windows/` z własnym buildem; reszta programu zostaje na GCC.
- **Ryzyko.** Błąd w sterowniku jądra to niebieski ekran. Rozwój i testy wyłącznie w maszynie
  wirtualnej (Hyper-V/VirtualBox z Windows 11 w trybie testowym), nigdy na komputerze użytkownika,
  dopóki nie przejdzie testów z D4.
- **„Od podstaw” w praktyce.** Szkielet sterownika audio (PortCls/WaveRT, topologia, INF) to kilka
  tysięcy linii wymaganego przez Windows kodu szablonowego. Punkt wyjścia: przykład Microsoftu
  „Simple Audio Sample” (SYSVAD, licencja MIT, zgodna z naszą) – usuwamy z niego wszystko poza jednym
  wirtualnym mikrofonem i piszemy własną część danych. Pisanie szablonu od zera nie daje nic poza
  ryzykiem błędów; wpisać pochodzenie do `THIRD_PARTY_NOTICES.md`.
- Linux nie potrzebuje sterownika: źródło PipeWire już to robi.

### Architektura
- Sterownik jądra (WDM, PortCls + miniport WaveRT), jedno urządzenie capture: 48 kHz, mono i stereo,
  16-bit i float, bez głośnika. Nazwa przyjazna „mic thingy microphone”.
- Kanał danych program → sterownik: bufor pierścieniowy we współdzielonej pamięci (sekcja utworzona
  przez sterownik, mapowana do procesu przez IOCTL na interfejsie urządzenia), indeksy zapisu/odczytu
  jako wartości atomowe – ten sam wzorzec SPSC co `src/util/ringbuf.c`. Bez blokad po obu stronach.
- Sterownik w swoim timerze/DPC kopiuje z pierścienia do bufora WaveRT; przy braku danych podaje ciszę.
  Dryf zegarów: logika z `VfSink` (cel wypełnienia, pomijanie/powtarzanie jednej próbki) po stronie
  programu, sterownik udostępnia swój licznik pozycji.
- Program: nowy backend `src/audio/vmic_windows.c` obok `vmic_pipewire.c` (interfejs `vmic.h` już jest).
  Wykrywanie: jest sterownik → używamy go; nie ma → dotychczasowa ścieżka VB-Cable. W GUI informacja,
  która ścieżka działa.
- Zabezpieczenia: IOCTL-e dostępne tylko dla jednego klienta naraz, walidacja wszystkich rozmiarów,
  żadnych wskaźników z trybu użytkownika używanych bez sprawdzenia; mapowanie tylko do zapisu danych audio.

### Etapy
- **D0. Środowisko.** Maszyna wirtualna z Windows 11, tryb testowy, Visual Studio Build Tools + WDK,
  debugowanie jądra (WinDbg po sieci), Driver Verifier włączony. Zbudować i zainstalować niezmieniony
  przykład Microsoftu, potwierdzić, że urządzenie się pojawia.
- **D1. Minimalny mikrofon.** Okrojony sterownik z jednym urządzeniem capture, które generuje ciszę
  (potem ton testowy). Kryterium: widoczny w Ustawieniach dźwięku, Discord/OBS nagrywa ton, brak
  błędów Driver Verifier przez godzinę.
- **D2. Kanał danych.** Sekcja współdzielona + IOCTL-e + narzędzie `tools/vf_drvtest.c`, które wpisuje
  sinus. Kryterium: `vf-record` na nowym urządzeniu + `vf-eval match` daje r ≥ 0,9999 i 0 przerw przez
  10 minut; odłączenie klienta w trakcie nie wywołuje błędu.
- **D3. Integracja.** `vmic_windows.c`, wybór ścieżki w sesji, kompensacja dryfu, pomiar opóźnienia
  (`vf-latency --through`) w porównaniu z VB-Cable. Wpis z liczbami do `CLAUDE.md`.
- **D4. Odporność.** Uśpienie/wybudzenie, zmiana formatu przez aplikację, dwóch odbiorców naraz,
  zabicie programu w trakcie, instalacja/odinstalowanie 20 razy, testy HLK dla audio na tyle, na ile
  działają bez certyfikacji. Driver Verifier przez cały czas.
- **D5. Instalacja i podpis.** INF + katalog, instalator (`pnputil` / `devcon`) z wyraźną zgodą
  użytkownika i deinstalacją. Wariant A: podpis attestation (jeśli użytkownik zdobędzie certyfikat EV).
  Wariant B: paczka „tylko tryb testowy” z opisem skutków w README.

Wielkość: to największy punkt planu, porównywalny z całym dotychczasowym projektem, i jedyny,
którego nie da się przetestować automatycznie na tej maszynie. Robić po L0–L2 i M1, bo dopiero pomiar
z L0 pokaże, ile milisekund VB-Cable faktycznie zabiera.
