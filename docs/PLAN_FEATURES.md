# Plan: dodatkowe funkcje

Uzupełnienie `docs/PLAN_PERFORMANCE.md` (tam: opóźnienie, pamięć, funkcje F1–F10, sterownik).
Tutaj 19 kolejnych funkcji (N1–N19) i sposób, żeby wdrożyć je tanio: najpierw wspólne fundamenty,
potem funkcje w paczkach, które z nich korzystają.

Zasady stałe (z `CLAUDE.md`): zero malloc/mutex/I/O w callbacku audio, parametry przez atomiki,
każdy efekt to osobny moduł `.c/.h` z tabelą `FxParamInfo`, każdy nowy napis w `i18n.c` po angielsku
i po polsku, `ctest` po każdej zmianie, commit po każdej działającej paczce.

Zasady dla nowych funkcji:
- **Domyślnie wyłączone.** Wyłączony efekt nie kosztuje CPU (bypass przed `process`) ani opóźnienia.
- **Opóźnienie jawne.** Efekt, który opóźnia sygnał, ustawia `Fx.latency`; GUI pokazuje sumę.
- **Budżet pamięci.** Proces w tle ma zostać poniżej 40 MB – bufory funkcji alokować dopiero przy
  włączeniu i zwalniać przy wyłączeniu (poza wątkiem audio).
- **Dźwięku nie stroimy po cichu.** Wartości domyślne nowych efektów proponujemy, użytkownik odsłuchuje.

## 1. Fundamenty (paczka P0)

Bez nich każda funkcja wymagałaby własnych obejść. Robić w tej kolejności.

| # | Fundament | Co i gdzie | Kto korzysta |
|---|---|---|---|
| B1 | Więcej efektów w łańcuchu | `src/dsp/chain.c`: nowa kolejność (niżej). `VF_FX_COUNT` rośnie; ustawienia, presety i komendy konsoli biorą efekty z tabel, więc działają same. Sprawdzić wszystkie miejsca ze stałą liczbą 6 (klawisze 1..6, układ GUI). | N1–N6 |
| B2 | Panel efektów z zakładkami | `src/main_gui.c`: obszar paneli dostaje zakładki „Podstawowe / Dodatkowe / Efekty”; panele dalej budowane z `Fx.params`. Okno nie rośnie. | N1–N6 |
| B3 | Szyna miksu przed limiterem | `src/dsp/pipeline.c`: limiter wychodzi z łańcucha na sam koniec; przed nim punkt sumowania do 4 źródeł (`VfSink` albo bufor w pamięci) z atomowym wzmocnieniem i tłumieniem sterowanym przez VAD (ducking). | N9, N10 |
| B4 | Globalne skróty klawiszowe | `src/app/hotkeys.[ch]`: `RegisterHotKey` na wątku traya (Windows), tabela akcji, zapis w `settings.ini`, edytor skrótów w GUI. Linux/X11: `XGrabKey`; Wayland – brak, opisać. Wspólne z F2 (mute/PTT). | N8, N9, N17 |
| B5 | Wyciszenie z łagodnym zboczem | `Pipeline.mute` (atomik) + 10 ms narastania/opadania na wyjściu, stan widoczny w trayu. Wspólne z F2. | N8, N17, N19 |
| B6 | Wątek zapisu do pliku | `src/app/recorder.[ch]`: tapy silnika → `VfSink` → wątek piszący WAV (1–4 kanały). Wątek audio tylko pisze do pierścienia. Wspólne z F1 (test A/B). | N16 |
| B7 | Statystyki sekundowe | `src/app/stats.[ch]`: wątek UI składa z ringu metrów jedną próbkę na sekundę (poziomy, tłumienie szumu, GR, limiter, niedobory) do pierścienia 3600 pozycji (ok. 100 kB). | N14, N15 |
| B8 | Motyw jako dane | `src/main_gui.c`: stałe `C_*` do struktury `Theme` + tabela motywów; `apply_style()` bierze aktywny. | N18 |

Nowa kolejność toru:
```
mikrofon → highpass → [N2 przydźwięk] → RNNoise → [N1 pogłos] → bramka → [N3 oddechy]
         → [N6 krzyk] → kompresor → [F5 de-esser] → EQ → [N4 ocieplenie] → [N5 efekty głosu]
         → szyna miksu (N9 soundboard, N10 dźwięk systemu) → limiter → wyjście (+ N7 odsłuch)
```
Testy P0: istniejące testy łańcucha (opóźnienie, zgodność bit w bit przy wyłączonych nowych efektach
i pustej szynie), limiter trzyma sufit przy pełnej szynie, wyciszenie bez trzasku (brak skoku > 0,01).

## 2. Paczka P1 – małe efekty dźwięku

Każdy: moduł w `src/dsp/`, wpis w `chain.c`, napisy w `i18n.c`, test w `tests/test_dsp.c`,
przebieg macierzy audio z efektem włączonym (`run_audio_tests.ps1 -Tag <efekt>`).

| # | Funkcja | Realizacja | Test | Rozmiar |
|---|---|---|---|---|
| N2 | Filtr przydźwięku 50/60 Hz | `fx_hum.c`: kaskada wąskich filtrów notch (biquad, Q ok. 30) na podstawowej i 3–5 harmonicznych. Parametry: częstotliwość (auto/50/60), liczba harmonicznych, głębokość. Auto: w pauzach mowy (VAD) porównanie energii dwóch filtrów pasmowych 50 i 60 Hz. 0 ms opóźnienia. | sinus 50 Hz + harmoniczne tłumione ≥ 30 dB, mowa 300–3000 Hz zmieniona < 0,5 dB; istniejący szum `noise_hum.wav` w macierzy | mały |
| N6 | Ochrona przed krzykiem | `fx_guard.c`: szybki leveler przed kompresorem – detektor szczytowy z atakiem 1 ms, ściszanie powyżej progu ze stosunkiem 8:1, powolny powrót (300–500 ms). Parametry: próg, powrót. 0 ms. | skok +20 dB: wyjście nie przekracza progu + 3 dB po 5 ms; sygnał poniżej progu bez zmian | mały |
| N4 | Ocieplenie głosu | `fx_warmth.c`: jedno pokrętło „ilość” sterujące łagodnym nasyceniem (`tanh` z kompensacją głośności) i półką niskich tonów +0…4 dB przy 150 Hz. Nadpróbkowanie 2× tylko gdy pomiar pokaże słyszalny aliasing. | przy ilości 0 sygnał bit w bit; głośność (RMS) stała ±0,5 dB w całym zakresie; brak NaN | mały |
| N3 | Tłumik oddechów i mlaśnięć | `fx_breath.c`: po bramce; gdy VAD niski, a obwiednia rośnie krótko (20–300 ms) – ściszenie o ustawioną głębokość. Mlaśnięcia: detektor transjentu w paśmie 2–8 kHz poza mową. Wspólny detektor transjentów z F6 (stuki klawiatury) – jeden plik `dsp/transient.[ch]`. | syntetyczny „oddech” (szum pasmowy 200 ms bez VAD) ściszony ≥ 10 dB; początki słów w macierzy nie gorsze niż −0,5 dB | średni |

## 3. Paczka P2 – wygoda

| # | Funkcja | Realizacja | Test | Rozmiar |
|---|---|---|---|---|
| N7 | Odsłuch własnego głosu | Monitor już istnieje (`src/audio/monitor.c`). Dodać atomowe wzmocnienie monitora, suwak w GUI i wybór „po filtrach / przed filtrami” (drugi tap z wejścia). Cel `VfSink` dla odsłuchu obniżyć do 240–480 próbek, bo własny głos z opóźnieniem > 30 ms przeszkadza; pokazać zmierzone opóźnienie odsłuchu. | test tapów: wzmocnienie stosowane, brak trzasku przy zmianie | mały |
| N11 | Profile per mikrofon | `settings.c`: sekcje `[device.<nazwa>]` z nazwą presetu albo pełnym zestawem `fx.*`. Przy zmianie urządzenia wejściowego zapis bieżących i wczytanie profilu. Opcja „wspólne ustawienia dla wszystkich mikrofonów” (domyślnie wspólne, żeby nie zaskoczyć obecnych użytkowników). | test settings: dwa urządzenia, przełączanie zachowuje oba zestawy; stary plik bez sekcji działa | mały |
| N12 | Eksport i import presetów | Przyciski „Eksportuj / Importuj”: plik `.ini` (ten sam format co `presets/`) oraz schowek (`SetClipboardText` / `GetClipboardText` z raylib). Import przechodzi przez istniejący parser z walidacją zakresów; nieznane klucze pomijane. | test: eksport → import daje identyczne parametry; uszkodzony tekst odrzucony bez zmian | mały |
| N13 | Cofnij/ponów + porównanie | `main_gui.c`: stos 64 migawek tablicy parametrów (zapis po puszczeniu suwaka lub zatwierdzeniu wartości). Ctrl+Z / Ctrl+Y. Przycisk „Porównaj” – przytrzymanie przełącza chwilowo na zapisany preset (to samo miejsce, w którym warto dodać „Wczytaj ponownie preset”). | test interakcji przez PostMessage: zmiana, Ctrl+Z, odczyt `settings.ini` | mały |
| N8 | Automatyczne wyciszenie | Dwa niezależne przełączniki: (a) blokada ekranu → wyciszenie, odblokowanie → powrót (`WTSRegisterSessionNotification` na wątku traya; Linux: sygnał logind, opcjonalnie); (b) brak mowy przez X minut → wyciszenie, powrót tylko ręcznie (skrót, tray, okno) – inaczej funkcja nie chroni prywatności. Korzysta z B5. | test logiki czasowej na sztucznym zegarze; ręcznie: Win+L | mały |

## 4. Paczka P3 – diagnostyka

| # | Funkcja | Realizacja | Test | Rozmiar |
|---|---|---|---|---|
| N14 | Miernik jakości na żywo | Z B7: zdjęty szum (poziom wejścia minus wyjścia w pauzach), odsetek czasu z pracującym limiterem, średni poziom mowy. Komunikaty: „mikrofon za cicho” (mowa < −35 dBFS), „za głośno” (szczyty wejścia > −1 dBFS), „limiter pracuje ciągle”. Mały panel w GUI. | test progów komunikatów na syntetycznych statystykach | mały |
| N15 | Historia zacięć | Z B7: wykres słupkowy niedoborów/przepełnień z ostatniej godziny + znaczniki zmian ustawień buforów. Przydatne do oceny etapu L1 z planu wydajności. | test pierścienia statystyk (zawijanie, sumy) | mały |
| N16 | Nagrywanie do pliku | Z B6: przycisk „Nagrywaj” → WAV 2 kanały (surowy, przefiltrowany) w folderze użytkownika (domyślnie Dokumenty\mic thingy), limit długości i wolnego miejsca, czerwona dioda w GUI i w trayu. Nagrywa tylko własny mikrofon – bez dźwięku rozmówców. | porównanie nagranego kanału z wyjściem offline (`vf-eval match`, r ≥ 0,9999), brak utraconych próbek przez 10 minut | średni |

## 5. Paczka P4 – interfejs

| # | Funkcja | Realizacja | Test | Rozmiar |
|---|---|---|---|---|
| N17 | Tryb kompaktowy | Przycisk w nagłówku: okno zmienia rozmiar na ok. 300×120 (`SetWindowSize`), zawsze na wierzchu (`FLAG_WINDOW_TOPMOST`), tylko mierniki we/wy, dioda VAD, mute, bypass. Pozycje obu trybów zapisywane w ustawieniach. Rysowanie w trybie kompaktowym 20 kl./s. | zrzut `--screenshot` w trybie kompaktowym; test PostMessage przełączania | mały |
| N18 | Motywy kolorów | Z B8: obecny (grafit + zieleń), bursztynowy terminal, szary Windows 98. Każdy motyw musi zachować znaczenie kolorów: włączone / wyłączone / ostrzeżenie mają trzy różne barwy. Wybór w nagłówku, `[app] theme`. Logo i ikona zostają w barwach podstawowych. | zrzut każdego motywu; test, że żaden motyw nie ma dwóch identycznych kolorów stanu | mały |

## 6. Paczka P5 – duże funkcje

Każda osobno, z własnym commitem i odsłuchem użytkownika. Kolejność według stosunku korzyści do ryzyka.

### N9. Soundboard
- Do 12 klipów pod skrótami (B4), miksowane w szynie (B3), z osobną głośnością i opcją „słyszę też w odsłuchu”.
- Pliki WAV wczytywane istniejącym `wavio` przy przypisaniu, przeliczane do 48 kHz mono i trzymane jako
  int16. Limit: 15 s na klip i 8 MB łącznie (budżet pamięci); dłuższe pliki odrzucane z komunikatem.
- Odtwarzanie: wątek UI ustawia atomowo „klip K od pozycji 0”, wątek audio tylko czyta bufor. Podmiana
  klipu: nowy bufor publikowany atomowym wskaźnikiem, stary zwalniany po potwierdzeniu, że wątek audio
  go nie używa (licznik ramek).
- Test: klip w szynie zgodny z plikiem, limiter trzyma sufit przy mowie + klipie, podmiana w trakcie
  odtwarzania bez błędów (pętla 1000 podmian).

### N10. Miksowanie dźwięku systemu
- Windows: urządzenie loopback WASAPI (miniaudio `ma_device_type_loopback`) → `VfSink` → szyna (B3).
  Linux: monitor wyjścia PipeWire.
- Ducking: gdy VAD wykrywa mowę, dźwięk systemu ściszany o ustawioną wartość (atak 50 ms, powrót 400 ms).
- Pułapka: rozmówcy z Discorda też są „dźwiękiem systemu” i usłyszeliby samych siebie. Windows 10 2004+
  pozwala przechwycić dźwięk jednego procesu (process loopback) – miniaudio tego nie ma, wymaga
  własnego kodu WASAPI (`ActivateAudioInterfaceAsync`). Etap 1: całe urządzenie + wyraźne ostrzeżenie;
  etap 2: wybór aplikacji.
- Test: sztuczne źródło w szynie, krzywa duckingu, kompensacja dryfu (`VfSink` już testowany).

### N5. Zmiana wysokości głosu i efekty
- `fx_voice.c` z trybami: wysokość (±12 półtonów), robot (modulacja pierścieniowa), radio/telefon
  (filtr pasmowy 300–3400 Hz + nasycenie z N4).
- Zmiana wysokości: metoda z dwiema przenikającymi się liniami opóźniającymi (prosta, ok. 20–30 ms
  opóźnienia, lekko „ziarnista”) jako etap 1; wokoder fazowy z zachowaniem formantów tylko jeśli jakość
  okaże się niewystarczająca. Opóźnienie zgłaszane w `Fx.latency` i pokazane przy przełączniku.
- Robot i radio: 0 ms.
- Test: sinus 440 Hz przesunięty o +12 półtonów daje 880 Hz ±1%; przy 0 półtonów i wyłączonych trybach
  sygnał bit w bit; brak NaN.

### N1. Usuwanie pogłosu pokoju
- Etap 1 (tani, 0 ms): ekspander ogona – po końcu słowa (VAD opada) obwiednia jest ściągana szybciej,
  niż wybrzmiewa pokój. Parametry: siła, czas. Wspólna obwiednia z bramką.
- Etap 2 (jeśli etap 1 nie wystarczy po odsłuchu): tłumienie widmowe późnego pogłosu – STFT 960/480,
  estymata pogłosu jako wygładzona, opóźniona moc widma, wzmocnienie typu Wienera. Kosztuje 10 ms
  opóźnienia, chyba że zostanie połączone z torem bocznym trybu „low latency” (L2 w planie wydajności) –
  dlatego robić po L2.
- Test: mowa TTS spleciona z syntetyczną odpowiedzią pokoju (wykładniczo gasnący szum, RT60 0,3 i 0,6 s):
  energia ogona po słowach niższa o ≥ 6 dB, początki słów nie gorsze niż −0,5 dB. Nowy generator
  w `vf-eval` („reverb”).

### N19. Nakładka w grze
- Małe okno zawsze na wierzchu, przezroczyste dla kliknięć (`WS_EX_LAYERED | WS_EX_TRANSPARENT |
  WS_EX_TOPMOST | WS_EX_NOACTIVATE`), rysowane GDI na wątku traya: ikona mikrofonu zielona (mówisz),
  szara (cisza), burgundowa (mute). Pozycja: wybór rogu ekranu.
- Bez wstrzykiwania kodu do gry (anty-cheaty), więc działa w grach w oknie i w „oknie bez ramek”,
  nie działa w wyłącznym trybie pełnoekranowym – opisać w README.
- Działa także wtedy, gdy główne okno jest zwolnione (etap M1 planu wydajności), dlatego GDI, nie raylib.
- Linux: pominąć (Wayland nie pozwala), zaznaczyć w dokumentacji.
- Test ręczny + zrzut okna nakładki.

## 7. Kolejność

P0 (fundamenty) → P1 (N2, N6, N4, N3) → P2 (N7, N11, N12, N13, N8) → P3 (N14, N15, N16) →
P4 (N17, N18) → P5 (N9, N10, N5, N1, N19).

Zależności od planu wydajności:
- B4 i B5 są wspólne z F2 (mute/push-to-talk), B6 z F1 (test A/B) – zrobić razem.
- Detektor transjentów wspólny dla N3 i F6.
- N1 etap 2 po L2; N19 i tray po M1 (okno zwalniane w tle); N15 pomaga ocenić L1.
- Jeśli powstaną dwa procesy (M2), hotkeys, mute, nagrywanie, soundboard i nakładka należą do procesu
  silnika, a GUI tylko nimi steruje przez pamięć współdzieloną.

Po każdej paczce: build bez ostrzeżeń, `ctest`, zrzut GUI, pomiar pamięci w tle (< 40 MB) i sumy
opóźnień, aktualizacja `CLAUDE.md` i README, commit.

## Stan realizacji (2026-11-13)

Fundamenty: B1 łańcuch 14 efektów, B2 zakładki (Podstawowe / Dodatkowe / Efekty / Narzędzia / Ustawienia), B3 szyna miksu
przed limiterem (z duckingiem), B4 skróty (odpytywanie klawiatury), B5 wyciszenie z rampą 10 ms, B6 wątek nagrywania,
B7 statystyki sekundowe, B8 motywy jako dane — wszystkie zrobione.

| # | Funkcja | Stan |
|---|---|---|
| N1 | redukcja pogłosu | etap 1 (ekspander ogona, 0 ms); etap 2 (spektralny) nie zrobiony |
| N2 | przydźwięk 50/60 Hz | zrobione, z autodetekcją |
| N3 | oddechy i mlaśnięcia (+ F6 kliknięcia) | zrobione (heurystyka VAD + transjenty HF) |
| N4 | ocieplenie | zrobione |
| N5 | efekty głosu | zrobione (wysokość: dwa odczyty z crossfade, 15 ms opóźnienia przy przesunięciu; robot; radio) |
| N6 | ochrona przed krzykiem | zrobione |
| N7 | odsłuch własnego głosu | zrobione (poziom + surowy/przetworzony, tryb „Mic + monitor”) |
| N8 | automatyczne wyciszenie | zrobione (blokada ekranu, cisza po N minutach) |
| N9 | soundboard | zrobione (8 klipów, 15 s / 8 MB, upuszczanie plików, skrót pod prawym przyciskiem) |
| N10 | dźwięk systemu | etap 1 (cały dźwięk urządzenia, ostrzeżenie o rozmówcach); wybór aplikacji nie zrobiony |
| N11 | profile per mikrofon | zrobione |
| N12 | eksport/import presetów | zrobione (schowek, plik, upuszczenie .ini) |
| N13 | cofnij / ponów / porównaj | zrobione (Ctrl+Z / Ctrl+Y, przytrzymaj „Compare”) |
| N14 | miernik jakości | zrobione |
| N15 | historia zacięć | zrobione (30 minut) |
| N16 | nagrywanie do pliku | zrobione (stereo: surowy + filtrowany) |
| N17 | tryb kompaktowy | zrobione |
| N18 | motywy | zrobione (3 motywy, test rozróżnialności kolorów stanu) |
| N19 | nakładka w grze | zrobione (Windows; okno warstwowe, bez wstrzykiwania kodu) |

Nie przetestowane na żywo: dźwięk systemu z prawdziwą aplikacją, AEC w prawdziwym pokoju, skróty klawiszowe wciskane
fizycznie (logika i parsowanie testowane), nakładka w grze pełnoekranowej. Linux: nic z tej listy nie było
kompilowane poza przenośną logiką (CI jest pierwszą okazją).
