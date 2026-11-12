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
    [S_SUBTITLE]          = { "real-time microphone filter", "filtr mikrofonu w czasie rzeczywistym" },
    [S_LANGUAGE]          = { "EN", "PL" },
};

typedef struct { const char *key; const char *en, *pl; } Pair;

static const Pair k_fx[] = {
    { "highpass", "High-pass filter", "Filtr górnoprzepustowy" },
    { "rnnoise",  "Noise removal (RNNoise)", "Odszumianie (RNNoise)" },
    { "gate",     "Noise gate (VAD)", "Bramka szumów (VAD)" },
    { "comp",     "Compressor", "Kompresor" },
    { "eq",       "3-band equalizer", "Korektor 3-pasmowy" },
    { "limiter",  "Limiter", "Limiter" },
};

static const Pair k_params[] = {
    { "highpass.freq",     "Frequency", "Częstotliwość" },
    { "rnnoise.wet",       "Strength", "Siła" },
    { "rnnoise.lookahead", "Look-ahead (+10 ms, better quality)", "Wyprzedzenie (+10 ms, lepsza jakość)" },
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
