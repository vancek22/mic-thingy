#include "app/i18n.h"

#include <stdatomic.h>
#include <string.h>

static atomic_int g_lang = VF_LANG_EN;   /* read by the tray thread too */

static const char *const k_str[S_COUNT][VF_LANG_COUNT] = {
    [S_MICROPHONE]        = { "Microphone", "Mikrofon" },
    [S_OUTPUT]            = { "Output", "Wyjście" },
    [S_MODES]             = { "Headphones;Virtual mic;Mic + monitor", "Słuchawki;Wirtualny mic;Mic + odsłuch" },
    [S_SPEAKERS]          = { "Speakers", "Głośniki" },
    [S_MONITOR]           = { "Monitor", "Odsłuch" },
    [S_VMIC_HINT]         = { "In Discord / OBS select the microphone: \"%s\"",
                              "W Discordzie / OBS wybierz mikrofon: \"%s\"" },
    [S_FILTER_ON]         = { "Filter: ON", "Filtr: WŁ." },
    [S_FILTER_OFF]        = { "Filter: OFF (bypass)", "Filtr: WYŁ." },
    [S_START]             = { "#131# Audio: OFF", "#131# Dźwięk: WYŁ." },   /* shown while stopped */
    [S_STOP]              = { "#132# Audio: ON", "#132# Dźwięk: WŁ." },     /* shown while running */
    [S_PRESET]            = { "Preset", "Preset" },
    [S_SAVE_AS]           = { "Save as", "Zapisz jako" },
    [S_DELETE]            = { "Delete", "Usuń" },
    [S_START_WITH_SYSTEM] = { "Start with system", "Start z systemem" },
    [S_FILTER_ON_LAUNCH]  = { "Filter on launch", "Filtruj po starcie" },
    [S_RUN_IN_BACKGROUND] = { "Run in background", "Działaj w tle" },
    [S_LEVELS]            = { "Levels", "Poziomy" },
    [S_INPUT]             = { "Input", "Wejście" },
    [S_OUTPUT_LEVEL]      = { "Output", "Wyjście" },
    [S_VAD]               = { "Voice activity (VAD)", "Aktywność głosu (VAD)" },
    [S_VAD_FMT]           = { "%d %%  (threshold %d %%)", "%d %%  (próg %d %%)" },
    [S_GATE_OPEN]         = { "Gate opening", "Bramka - otwarcie" },
    [S_COMPRESSION]       = { "Compression", "Kompresja" },
    [S_LATENCY]           = { "Latency (estimate)", "Opóźnienie (szacunek)" },
    [S_LAT_PW_FMT]        = { "in %.0f  DSP %.1f\nbuffer %.0f ms", "wej. %.0f  DSP %.1f\nbufor %.0f ms" },
    [S_LAT_FMT]           = { "in %.0f  out %.0f\nFIFO %.0f  DSP %.1f ms",
                              "wej. %.0f  wyj. %.0f\nFIFO %.0f  DSP %.1f ms" },
    [S_DROPOUTS_FMT]      = { "Dropouts: %u", "Przerwy w dźwięku: %u" },
    [S_ENABLED]           = { "enabled", "włączony" },
    [S_DEFAULTS]          = { "Reset", "Domyślne" },
    [S_NO_DEVICES]        = { "(no devices)", "(brak urządzeń)" },
    [S_ST_RUNNING]        = { "Running - %s", "Działa - %s" },
    [S_ST_TESTFILE]       = { " (source: test file)", " (źródło: plik testowy)" },
    [S_ST_STOPPED]        = { "Stopped - choose devices and switch Audio ON", "Zatrzymane - wybierz urządzenia i włącz Dźwięk" },
    [S_ST_ERROR]          = { "Error: %s", "Błąd: %s" },
    [S_N_NAME_MISSING]    = { "Type a preset name in the box first.", "Wpisz nazwę presetu w polu obok." },
    [S_N_BUILTIN_NAME]    = { "\"%s\" is a built-in preset - choose another name.",
                              "\"%s\" to preset wbudowany - wybierz inną nazwę." },
    [S_N_SAVE_FAILED]     = { "Could not save the preset.", "Nie udało się zapisać presetu." },
    [S_N_SAVED]           = { "Preset \"%s\" saved.", "Zapisano preset \"%s\"." },
    [S_N_DELETED]         = { "Preset \"%s\" deleted.", "Usunięto preset \"%s\"." },
    [S_N_LOADED]          = { "Preset \"%s\" loaded.", "Wczytano preset \"%s\"." },
    [S_N_LOAD_FAILED]     = { "Could not load the preset.", "Nie udało się wczytać presetu." },
    [S_N_AUTOSTART_ON]    = { "After login mic thingy starts in the background and filters right away.",
                              "Po zalogowaniu mic thingy wystartuje w tle i od razu zacznie filtrować." },
    [S_N_AUTOSTART_OFF]   = { "Start with system turned off.", "Autostart wyłączony." },
    [S_N_AUTOSTART_FAIL]  = { "Could not change start with system.", "Nie udało się zmienić autostartu." },
    [S_N_BG_TRAY]         = { "Closing the window hides mic thingy to the tray - the filter keeps running.",
                              "Zamknięcie okna schowa mic thingy do zasobnika - filtr działa dalej." },
    [S_N_BG_MIN]          = { "Closing the window minimizes mic thingy - the filter keeps running.",
                              "Zamknięcie okna zminimalizuje mic thingy - filtr działa dalej." },
    [S_N_BG_OFF]          = { "Closing the window quits mic thingy.", "Zamknięcie okna zakończy mic thingy." },
    [S_N_IN_TRAY]         = { "mic thingy is running in the background - icon in the system tray.",
                              "mic thingy działa w tle - ikona w zasobniku systemowym." },
    [S_N_SAVE_SETTINGS_FAIL] = { "Could not save %s", "Nie udało się zapisać %s" },
    [S_N_DEVICE_LOST]     = { "Audio device stopped - restarting...", "Urządzenie audio przestało działać - ponawiam uruchomienie..." },
    [S_N_RESAMPLE_FMT]    = { "Device runs at %u Hz, not 48000: Windows resamples (more latency). Set 48000 Hz in Sound settings.",
                              "Urządzenie działa z %u Hz, nie 48000: Windows przelicza próbki (większe opóźnienie). Ustaw 48000 Hz w ustawieniach dźwięku." },
    [S_N_RETRY_FMT]       = { "%s - retrying in 3 s", "%s - ponowna próba za 3 s" },
    [S_TRAY_SHOW]         = { "Show window", "Pokaż okno" },
    [S_TRAY_PROCESSING]   = { "Audio processing", "Przetwarzanie dźwięku" },
    [S_TRAY_FILTER]       = { "Filter enabled", "Filtr włączony" },
    [S_TRAY_QUIT]         = { "Quit mic thingy", "Zakończ mic thingy" },
    [S_TRAY_TIP_STOPPED]  = { "stopped", "zatrzymany" },
    [S_TRAY_TIP_BYPASS]   = { "filter off (bypass)", "filtr wyłączony (bypass)" },
    [S_TRAY_TIP_ON]       = { "filtering", "filtruje" },
    [S_TRAY_MUTE]         = { "Microphone muted", "Mikrofon wyciszony" },
    [S_TRAY_TIP_MUTED]    = { "microphone muted", "mikrofon wyciszony" },
    [S_SUBTITLE]          = { "real-time microphone filter", "filtr mikrofonu w czasie rzeczywistym" },
    [S_LANGUAGE]          = { "EN", "PL" },
    [S_UNDO] = { "Undo", "Cofnij" },
    [S_REDO] = { "Redo", "Ponów" },
    [S_COMPARE] = { "Compare", "Porównaj" },
    [S_MIC_ON] = { "MIC ON", "MIKROFON" },
    [S_MIC_MUTED] = { "MUTED", "WYCISZONY" },
    [S_COMPACT_BTN] = { "MINI", "MINI" },
    [S_TABS_ALL] = { "Basic;Extra;Effects;Tools;Settings", "Podstawowe;Dodatkowe;Efekty;Narzędzia;Ustawienia" },
    [S_PN_MIX] = { "Monitor and mix", "Odsłuch i miks" },
    [S_MON_GAIN] = { "Own voice level", "Głośność własnego głosu" },
    [S_MON_RAW] = { "Monitor the unfiltered voice", "Odsłuch głosu bez filtrów" },
    [S_MON_HINT] = { "Needs the mode Mic + monitor", "Wymaga trybu Mic + odsłuch" },
    [S_SYS_AUDIO] = { "Mix system audio into the mic", "Dodaj dźwięk systemu do mikrofonu" },
    [S_SYS_GAIN] = { "System audio level", "Głośność dźwięku systemu" },
    [S_SYS_DUCK] = { "Lower it while I speak", "Ścisz, gdy mówię" },
    [S_SYS_WARN] = { "It includes the other people in a call", "Zawiera też rozmówców z połączenia" },
    [S_SYS_NA] = { "System audio mixing is Windows only", "Miks dźwięku systemu tylko na Windows" },
    [S_BOARD_GAIN] = { "Soundboard level", "Głośność soundboardu" },
    [S_PN_TEST] = { "Test and record", "Test i nagrywanie" },
    [S_AB_REC] = { "Record 10 s", "Nagraj 10 s" },
    [S_AB_RAW] = { "Raw", "Surowy" },
    [S_AB_FILT] = { "Filtered", "Filtrowany" },
    [S_AB_ALT] = { "Alternate", "Na zmianę" },
    [S_AB_MATCH] = { "Match loudness", "Wyrównaj głośność" },
    [S_AB_IDLE] = { "Press record and talk", "Naciśnij nagraj i mów" },
    [S_AB_REC_FMT] = { "Recording... %.0f s", "Nagrywam... %.0f s" },
    [S_AB_READY_FMT] = { "Recorded %.0f s - listen:", "Nagrano %.0f s - posłuchaj:" },
    [S_AB_PLAY_FMT] = { "Playing %.1f s", "Odtwarzam %.1f s" },
    [S_NEEDS_RUN] = { "Start the audio first", "Najpierw włącz dźwięk" },
    [S_REC_START] = { "Record to file", "Nagrywaj do pliku" },
    [S_REC_STOP] = { "Stop recording", "Zatrzymaj nagrywanie" },
    [S_REC_FMT] = { "Recording %.0f s", "Nagrywam %.0f s" },
    [S_REC_NOTE] = { "Left: raw mic, right: filtered. Your mic only.", "Lewy: surowy mikrofon, prawy: filtrowany. Tylko Twój mikrofon." },
    [S_PN_CAL] = { "Calibration", "Kalibracja" },
    [S_CAL_START] = { "Start calibration", "Rozpocznij kalibrację" },
    [S_CAL_SIL_FMT] = { "Step 1: stay quiet (%d s)", "Krok 1: milcz (%d s)" },
    [S_CAL_SPEECH_FMT] = { "Step 2: talk normally (%d s)", "Krok 2: mów normalnie (%d s)" },
    [S_CAL_DONE_FMT] = { "Noise %.0f dB, speech %.0f dB", "Szum %.0f dB, mowa %.0f dB" },
    [S_CAL_PROP_FMT] = { "Gate %.2f, comp. %.0f dB, make-up %.0f dB", "Bramka %.2f, komp. %.0f dB, wzmocn. %.0f dB" },
    [S_CAL_APPLY] = { "Apply", "Zastosuj" },
    [S_CAL_FAIL_FMT] = { "Failed: %s", "Nie udało się: %s" },
    [S_CAL_HELP] = { "Measures your room and voice and sets the gate and the compressor.", "Mierzy pokój i głos i ustawia bramkę oraz kompresor." },
    [S_PN_BOARD] = { "Soundboard", "Soundboard" },
    [S_BOARD_ADD] = { "Add", "Dodaj" },
    [S_BOARD_EMPTY] = { "(empty)", "(pusty)" },
    [S_BOARD_FULL] = { "No free slot", "Brak wolnego miejsca" },
    [S_BOARD_FAIL_FMT] = { "Cannot load %s", "Nie można wczytać %s" },
    [S_BOARD_LONG] = { "The clip is longer than 15 s", "Klip jest dłuższy niż 15 s" },
    [S_BOARD_MEM] = { "Soundboard memory is full (8 MB)", "Pamięć soundboardu pełna (8 MB)" },
    [S_BOARD_STOP] = { "Stop all", "Zatrzymaj" },
    [S_BOARD_HINT] = { "Type a file path or drop a sound file here", "Wpisz ścieżkę albo upuść tu plik dźwiękowy" },
    [S_PN_QUALITY] = { "Quality", "Jakość" },
    [S_Q_NODATA] = { "Collecting data...", "Zbieram dane..." },
    [S_Q_NOSPEECH] = { "No speech heard yet", "Jeszcze nie słychać mowy" },
    [S_Q_QUIET] = { "Microphone too quiet - raise its level", "Mikrofon za cichy - zwiększ jego poziom" },
    [S_Q_LOUD] = { "Microphone too loud - it clips", "Mikrofon za głośny - przesterowuje" },
    [S_Q_LIMITER] = { "Limiter works all the time - lower the input", "Limiter pracuje ciągle - zmniejsz poziom wejścia" },
    [S_Q_OK] = { "Level is good", "Poziom jest dobry" },
    [S_Q_SPEECH_FMT] = { "Speech %.0f dB  noise reduced by %.0f dB", "Mowa %.0f dB  szum zmniejszony o %.0f dB" },
    [S_Q_LIM_FMT] = { "Limiter active in %.0f%% of speech", "Limiter działa w %.0f%% mowy" },
    [S_Q_HISTORY] = { "Dropouts per minute, last 30 min", "Zacięcia na minutę, ostatnie 30 min" },
    [S_PN_SPECTRUM] = { "Spectrum", "Widmo" },
    [S_PN_LOOK] = { "Look and window", "Wygląd i okno" },
    [S_THEME] = { "Theme", "Motyw" },
    [S_THEMES] = { "Graphite;Amber;Classic", "Grafit;Bursztyn;Klasyk" },
    [S_COMPACT_MODE] = { "Compact window (meters only)", "Małe okno (tylko mierniki)" },
    [S_OVERLAY] = { "Mic indicator in a screen corner", "Wskaźnik mikrofonu w rogu ekranu" },
    [S_OVERLAY_NA] = { "The indicator is Windows only", "Wskaźnik tylko na Windows" },
    [S_CORNERS] = { "TL;TR;BL;BR", "LG;PG;LD;PD" },
    [S_PN_BEHAVIOUR] = { "Behaviour", "Zachowanie" },
    [S_PER_MIC] = { "Separate settings per microphone", "Osobne ustawienia dla każdego mikrofonu" },
    [S_AUTOMUTE_LOCK] = { "Mute while the screen is locked", "Wycisz przy zablokowanym ekranie" },
    [S_AUTOMUTE_IDLE] = { "Mute after silence (minutes, 0 = off)", "Wycisz po ciszy (minuty, 0 = wył.)" },
    [S_PTT] = { "Push-to-talk", "Push-to-talk" },
    [S_PTT_HINT] = { "Muted unless the key is held", "Wyciszony, dopóki nie trzymasz klawisza" },
    [S_PN_HOTKEYS] = { "Hotkeys", "Skróty klawiszowe" },
    [S_HK_MUTE] = { "Mute", "Wycisz" },
    [S_HK_PTT] = { "Push-to-talk key", "Klawisz push-to-talk" },
    [S_HK_BYPASS] = { "Filter on/off", "Filtr włącz/wyłącz" },
    [S_HK_SET] = { "Set", "Ustaw" },
    [S_HK_PRESS] = { "press keys...", "naciśnij klawisze..." },
    [S_HK_NOTE] = { "Work with the window hidden.", "Działają przy ukrytym oknie." },
    [S_PN_APPS] = { "Presets per program", "Presety dla programów" },
    [S_APPS_ENABLE] = { "Switch preset when a program runs", "Zmień preset, gdy działa program" },
    [S_APPS_ADD] = { "Add rule", "Dodaj regułę" },
    [S_APPS_NONE] = { "(none)", "(brak)" },
    [S_PN_SHARE] = { "Share presets", "Udostępnianie presetów" },
    [S_EXPORT_CLIP] = { "Copy preset", "Kopiuj preset" },
    [S_IMPORT_CLIP] = { "Paste preset", "Wklej preset" },
    [S_EXPORT_FILE] = { "Save as file", "Zapisz do pliku" },
    [S_IMPORT_DROP] = { "Or drop a .ini preset on the window", "Albo upuść plik .ini na okno" },
    [S_N_COPIED] = { "Preset copied to the clipboard", "Preset skopiowany do schowka" },
    [S_N_IMPORTED_FMT] = { "Imported %d settings", "Zaimportowano %d ustawień" },
    [S_N_IMPORT_FAIL] = { "That is not a mic thingy preset", "To nie jest preset mic thingy" },
    [S_N_EXPORTED_FMT] = { "Saved: %s", "Zapisano: %s" },
    [S_N_MUTED] = { "Microphone muted", "Mikrofon wyciszony" },
    [S_N_UNMUTED] = { "Microphone on", "Mikrofon włączony" },
    [S_N_IDLE_MUTED] = { "Muted after silence - press mute to talk", "Wyciszono po ciszy - naciśnij wycisz, by mówić" },
    [S_N_APP_FMT] = { "%s is running: preset %s", "%s działa: preset %s" },
    [S_N_APP_BACK] = { "Previous preset restored", "Przywrócono poprzedni preset" },
    [S_N_PROFILE_FMT] = { "Settings of %s", "Ustawienia: %s" },
    [S_N_REC_SAVED_FMT] = { "Recording saved: %s", "Nagranie zapisane: %s" },
    [S_N_REC_FAIL] = { "Recording failed", "Nagrywanie nie powiodło się" },
    [S_N_CAL_APPLIED] = { "Calibration applied", "Zastosowano kalibrację" },
    [S_COMPACT_TITLE] = { "mic thingy", "mic thingy" },

};

typedef struct { const char *key; const char *en, *pl; } Pair;

static const Pair k_fx[] = {
    { "highpass", "High-pass filter", "Filtr górnoprzepustowy" },
    { "rnnoise",  "Noise removal (RNNoise)", "Odszumianie (RNNoise)" },
    { "gate",     "Noise gate (VAD)", "Bramka szumów (VAD)" },
    { "comp",     "Compressor", "Kompresor" },
    { "eq",       "3-band equalizer", "Korektor 3-pasmowy" },
    { "aec",      "Echo cancellation (beta)", "Redukcja echa (beta)" },
    { "hum",      "Hum remover", "Usuwanie przydźwięku" },
    { "dereverb", "Room reverb reducer", "Redukcja pogłosu pokoju" },
    { "breath",   "Breaths and clicks", "Oddechy i klikanie" },
    { "guard",    "Shout guard", "Ochrona przed krzykiem" },
    { "agc",      "Auto volume (AGC)", "Automatyczna głośność (AGC)" },
    { "deesser",  "De-esser", "De-esser (sybilanty)" },
    { "warmth",   "Voice warmth", "Ciepło głosu" },
    { "voice",    "Voice effects", "Efekty głosu" },
    { "limiter",  "Limiter", "Limiter" },
};

static const Pair k_params[] = {
    { "highpass.freq",     "Frequency", "Częstotliwość" },
    { "rnnoise.wet",       "Strength", "Siła" },
    { "rnnoise.lookahead", "Look-ahead (+10 ms, better quality)", "Wyprzedzenie (+10 ms, lepsza jakość)" },
    { "rnnoise.lowlat",    "Low latency (~0.5 ms, rougher)", "Niskie opóźnienie (~0,5 ms, mniej gładkie)" },
    { "limiter.lookahead", "Look-ahead", "Wyprzedzenie" },
    { "aec.delay",         "Reference delay", "Opóźnienie referencji" },
    { "aec.strength",      "Adaptation", "Adaptacja" },
    { "hum.freq",          "Mains frequency", "Częstotliwość sieci" },
    { "hum.harmonics",     "Harmonics", "Harmoniczne" },
    { "hum.auto",          "Detect 50/60 Hz automatically", "Wykrywaj 50/60 Hz automatycznie" },
    { "dereverb.strength", "Strength", "Siła" },
    { "dereverb.depth",    "Maximum reduction", "Maksymalna redukcja" },
    { "dereverb.reach",    "Starts below speech", "Start poniżej mowy" },
    { "breath.breath",     "Breaths", "Oddechy" },
    { "breath.clicks",     "Clicks", "Kliknięcia" },
    { "breath.sens",       "Sensitivity", "Czułość" },
    { "guard.threshold",   "Threshold", "Próg" },
    { "guard.ratio",       "Ratio", "Stosunek" },
    { "guard.release",     "Recovery", "Powrót" },
    { "agc.target",        "Target level", "Docelowy poziom" },
    { "agc.maxgain",       "Maximum boost", "Maksymalne podbicie" },
    { "agc.speed",         "Speed", "Szybkość" },
    { "deesser.freq",      "Sibilance from", "Sybilanty od" },
    { "deesser.threshold", "Threshold", "Próg" },
    { "deesser.ratio",     "Ratio", "Stosunek" },
    { "deesser.range",     "Maximum reduction", "Maksymalna redukcja" },
    { "warmth.amount",     "Warmth", "Ciepło" },
    { "voice.pitch",       "Pitch", "Wysokość" },
    { "voice.robot",       "Robot", "Robot" },
    { "voice.radio",       "Radio / telephone", "Radio / telefon" },
    { "gate.threshold",    "Voice threshold", "Próg głosu" },
    { "gate.hold",         "Hold", "Podtrzymanie" },
    { "gate.attack",       "Attack", "Otwieranie" },
    { "gate.release",      "Release", "Zamykanie" },
    { "gate.floor",        "Attenuation", "Tłumienie" },
    { "comp.threshold",    "Threshold", "Próg" },
    { "comp.ratio",        "Ratio", "Stosunek" },
    { "comp.attack",       "Attack", "Atak" },
    { "comp.release",      "Release", "Powrót" },
    { "comp.makeup",       "Make-up gain", "Wzmocnienie" },
    { "comp.knee",         "Knee", "Kolano" },
    { "eq.low",            "Bass", "Basy" },
    { "eq.mid",            "Mid", "Środek" },
    { "eq.high",           "Treble", "Góra" },
    { "eq.lowfreq",        "Bass - freq.", "Basy - częst." },
    { "eq.midfreq",        "Mid - freq.", "Środek - częst." },
    { "eq.midq",           "Mid - Q", "Środek - Q" },
    { "eq.highfreq",       "Treble - freq.", "Góra - częst." },
    { "limiter.ceiling",   "Ceiling", "Sufit" },
    { "limiter.release",   "Release", "Powrót" },
};

void vf_i18n_set(VfLang lang) { atomic_store(&g_lang, (lang >= 0 && lang < VF_LANG_COUNT) ? lang : VF_LANG_EN); }
VfLang vf_i18n_get(void) { return (VfLang)atomic_load(&g_lang); }

const char *tr(VfStr id)
{
    if (id < 0 || id >= S_COUNT) return "?";
    const char *s = k_str[id][vf_i18n_get()];
    return s ? s : k_str[id][VF_LANG_EN];
}

const char *vf_lang_code(VfLang lang) { return lang == VF_LANG_PL ? "pl" : "en"; }

VfLang vf_lang_from_code(const char *code)
{
    return (code && (!strcmp(code, "pl") || !strcmp(code, "PL"))) ? VF_LANG_PL : VF_LANG_EN;
}

static const char *pick(const Pair *p) { return vf_i18n_get() == VF_LANG_PL ? p->pl : p->en; }

const char *tr_fx(const char *fx)
{
    for (size_t i = 0; i < sizeof k_fx / sizeof k_fx[0]; i++)
        if (!strcmp(k_fx[i].key, fx)) return pick(&k_fx[i]);
    return fx;
}

const char *tr_param(const char *fx, const char *param)
{
    char key[96];
    size_t a = strlen(fx), b = strlen(param);
    if (a + b + 2 > sizeof key) return param;
    memcpy(key, fx, a);
    key[a] = '.';
    memcpy(key + a + 1, param, b + 1);
    for (size_t i = 0; i < sizeof k_params / sizeof k_params[0]; i++)
        if (!strcmp(k_params[i].key, key)) return pick(&k_params[i]);
    return param;
}
