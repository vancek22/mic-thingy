/* VoiceFilter GUI (raylib + raygui).
 *
 *   voicefilter [--autostart] [--mode 0|1|2] [--in-file f.wav]
 *               [--screenshot shot.png --frames N]      (testing: render N frames, save, quit)
 *
 * The audio runs on miniaudio's thread; this thread only writes parameters
 * (atomics via pipeline_set_param) and drains the meter ring buffer. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "app/session.h"
#include "app/settings.h"
#include "util/platform.h"
#include "util/wavio.h"

#define WIN_W 1180
#define WIN_H 754
#define TOP   134          /* y of the meter / effect panels */
#define LIST_CHARS 8192
#define NAME_MAX_CHARS 44

/* ------------------------------------------------------------------ labels */

static const char *fx_title(const char *name)
{
    if (!strcmp(name, "highpass")) return "Filtr górnoprzepustowy";
    if (!strcmp(name, "rnnoise"))  return "Odszumianie (RNNoise)";
    if (!strcmp(name, "gate"))     return "Bramka szumów (VAD)";
    if (!strcmp(name, "comp"))     return "Kompresor";
    if (!strcmp(name, "eq"))       return "Korektor 3-pasmowy";
    if (!strcmp(name, "limiter"))  return "Limiter";
    return name;
}

static const char *param_label(const char *fx, const char *p)
{
    static const char *map[][3] = {
        { "highpass", "freq", "Częstotliwość" },
        { "rnnoise", "wet", "Siła" },
        { "rnnoise", "lookahead", "Wyprzedzenie (+10 ms, lepsza jakość)" },
        { "gate", "threshold", "Próg głosu" },
        { "gate", "hold", "Podtrzymanie" },
        { "gate", "attack", "Otwieranie" },
        { "gate", "release", "Zamykanie" },
        { "gate", "floor", "Tłumienie" },
        { "comp", "threshold", "Próg" },
        { "comp", "ratio", "Stosunek" },
        { "comp", "attack", "Atak" },
        { "comp", "release", "Powrót" },
        { "comp", "makeup", "Wzmocnienie" },
        { "comp", "knee", "Kolano" },
        { "eq", "low", "Basy" },
        { "eq", "mid", "Środek" },
        { "eq", "high", "Góra" },
        { "eq", "lowfreq", "Basy - częst." },
        { "eq", "midfreq", "Środek - częst." },
        { "eq", "midq", "Środek - Q" },
        { "eq", "highfreq", "Góra - częst." },
        { "limiter", "ceiling", "Sufit" },
        { "limiter", "release", "Powrót" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(map[i][0], fx) && !strcmp(map[i][1], p)) return map[i][2];
    return p;
}

/* ------------------------------------------------------------------ helpers */

/* raygui list text: "a;b;c" (';' inside names replaced, long names shortened). */
static void build_list(char *out, size_t cap, const VfDeviceList *l)
{
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < l->count && len + 8 < cap; i++) {
        char name[NAME_MAX_CHARS + 8];
        int n = 0, chars = 0;
        for (const unsigned char *p = (const unsigned char *)l->dev[i].name; *p && n < (int)sizeof name - 4; p++) {
            if ((*p & 0xC0) != 0x80 && ++chars > NAME_MAX_CHARS) { name[n++] = '.'; name[n++] = '.'; break; }
            name[n++] = (*p == ';') ? ',' : (char)*p;
        }
        name[n] = '\0';
        len += (size_t)snprintf(out + len, cap - len, "%s%s", i ? ";" : "", name);
    }
    if (l->count == 0) snprintf(out, cap, "(brak urządzeń)");
}

static Font load_ui_font(int size)
{
    static const char *paths[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    };
    int cps[95 + 224];
    int n = 0;
    for (int c = 32; c < 127; c++) cps[n++] = c;
    for (int c = 160; c < 384; c++) cps[n++] = c;      /* Latin-1 + Latin Extended-A (Polish) */
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        if (FileExists(paths[i])) {
            Font f = LoadFontEx(paths[i], size, cps, n);
            if (f.texture.id != 0) {
                SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
                return f;
            }
        }
    return GetFontDefault();
}

static float lin_to_db(float x) { return x > 1e-6f ? 20.0f * log10f(x) : -120.0f; }

typedef struct {
    float peak_in, peak_out, hold_in, hold_out, hold_t_in, hold_t_out;
    float vad, gate, gr, rms_out;
} Meters;

static void drain_meters(VfSession *s, Meters *m, float dt)
{
    float buf[VF_METER_FIELDS * 32];
    float pin = 0, pout = 0, rms = 0, gr = 0;
    int frames = 0;
    float vad = m->vad, gate = m->gate;
    unsigned got;
    while ((got = vf_ring_read(&s->meters, buf, sizeof buf / sizeof buf[0])) > 0) {
        for (unsigned k = 0; k + VF_METER_FIELDS <= got; k += VF_METER_FIELDS) {
            const float *f = buf + k;
            if (f[VF_M_PEAK_IN] > pin) pin = f[VF_M_PEAK_IN];
            if (f[VF_M_PEAK_OUT] > pout) pout = f[VF_M_PEAK_OUT];
            if (f[VF_M_RMS_OUT] > rms) rms = f[VF_M_RMS_OUT];
            if (f[VF_M_GR] < gr) gr = f[VF_M_GR];
            vad = f[VF_M_VAD];
            gate = f[VF_M_GATE];
            frames++;
        }
    }
    const float fall = 30.0f * dt;                     /* dB per frame of decay */
    float din = lin_to_db(pin), dout = lin_to_db(pout);
    m->peak_in = din > m->peak_in ? din : m->peak_in - fall;
    m->peak_out = dout > m->peak_out ? dout : m->peak_out - fall;
    if (din >= m->hold_in) { m->hold_in = din; m->hold_t_in = 1.5f; }
    else if ((m->hold_t_in -= dt) <= 0) m->hold_in -= 20.0f * dt;
    if (dout >= m->hold_out) { m->hold_out = dout; m->hold_t_out = 1.5f; }
    else if ((m->hold_t_out -= dt) <= 0) m->hold_out -= 20.0f * dt;
    if (frames) {
        m->vad = vad;
        m->gate = gate;
        m->gr = gr < m->gr ? gr : m->gr + (gr - m->gr) * fminf(1.0f, 6.0f * dt);
        m->rms_out = rms;
    } else if (!s->running) {
        m->vad = 0; m->gate = 1; m->gr = 0;
    }
}

/* Horizontal dB meter: -60..0 dBFS, green/yellow/red, with a peak-hold tick. */
static void draw_level(Rectangle r, float db, float hold_db)
{
    DrawRectangleRec(r, (Color){ 30, 34, 40, 255 });
    float t = (db + 60.0f) / 60.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float y1 = (-18.0f + 60.0f) / 60.0f, y2 = (-6.0f + 60.0f) / 60.0f;
    float w = r.width;
    DrawRectangle((int)r.x, (int)r.y, (int)(w * fminf(t, y1)), (int)r.height, (Color){ 60, 190, 90, 255 });
    if (t > y1) DrawRectangle((int)(r.x + w * y1), (int)r.y, (int)(w * (fminf(t, y2) - y1)), (int)r.height, (Color){ 220, 200, 60, 255 });
    if (t > y2) DrawRectangle((int)(r.x + w * y2), (int)r.y, (int)(w * (t - y2)), (int)r.height, (Color){ 220, 70, 60, 255 });
    float h = (hold_db + 60.0f) / 60.0f;
    if (h > 0 && h <= 1) DrawRectangle((int)(r.x + w * h) - 1, (int)r.y, 2, (int)r.height, RAYWHITE);
    for (int d = -48; d < 0; d += 12) {
        int x = (int)(r.x + w * (d + 60.0f) / 60.0f);
        DrawLine(x, (int)(r.y + r.height - 4), x, (int)(r.y + r.height), (Color){ 120, 120, 120, 255 });
    }
}

static void draw_fill(Rectangle r, float t, Color c)
{
    DrawRectangleRec(r, (Color){ 30, 34, 40, 255 });
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    DrawRectangle((int)r.x, (int)r.y, (int)(r.width * t), (int)r.height, c);
}

static int find_name(const VfDeviceList *l, const char *name)
{
    if (!name[0]) return -1;
    for (int i = 0; i < l->count; i++)
        if (!strcmp(l->dev[i].name, name)) return i;
    return -1;
}

/* Cheap fingerprint of everything that is saved, to detect changes. */
static unsigned long long settings_hash(const VfSession *s, const VfSettings *st, int a, int b, int c, int d)
{
    unsigned long long h = 1469598103934665603ULL;
#define MIX(x) (h = (h ^ (unsigned long long)(x)) * 1099511628211ULL)
    MIX(a); MIX(b); MIX(c); MIX(d); MIX(st->start_processing);
    MIX(atomic_load(&((VfSession *)s)->pipe.bypass_all));
    for (const char *p = st->preset; *p; p++) MIX(*p);
    for (int i = 0; i < s->pipe.count; i++) {
        MIX(atomic_load(&s->pipe.fx[i]->bypass));
        for (int j = 0; j < s->pipe.fx[i]->n_params; j++) {
            float v = pipeline_get_param(&s->pipe, i, j);
            unsigned u;
            memcpy(&u, &v, sizeof u);
            MIX(u);
        }
    }
#undef MIX
    return h;
}

static void build_preset_list(char *out, size_t cap, VfPresetInfo *list, int *n)
{
    *n = vf_presets_list(list, VF_MAX_PRESETS);
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < *n && len + 4 < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, "%s%s%s", i ? ";" : "", list[i].name, list[i].builtin ? "" : " *");
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    int autostart = 0, mode = -1, frames_to_shot = 0, verbose = 0, minimized = 0;
    const char *in_file = NULL, *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--autostart")) autostart = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--in-file") && i + 1 < argc) in_file = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames_to_shot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--minimized")) minimized = 1;
    }

    static VfSession s;
    int init_ok = vf_session_init(&s, 1) == 0;

    /* settings.ini: devices by name, mode, effect parameters */
    VfSettings st;
    vf_settings_defaults(&st);
    char settings_file[VF_PATH_MAX] = "";
    int have_settings_path = vf_settings_path(settings_file, sizeof settings_file) == 0;
    if (init_ok && have_settings_path) vf_settings_load(&st, &s.pipe, settings_file);
    int autostart_on = vf_autostart_get();

    float *file_buf = NULL;
    long file_len = 0;
    if (in_file) file_buf = vf_wav_read_mono(in_file, VF_SAMPLE_RATE, &file_len);

    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    SetTraceLogLevel(verbose ? LOG_INFO : LOG_WARNING);
    InitWindow(WIN_W, WIN_H, "VoiceFilter");
    SetTargetFPS(60);
    if (minimized) MinimizeWindow();
    /* Order matters: GuiLoadStyleDefault() unloads the texture of a previously set
     * custom font, so reset the style first and set our font afterwards. */
    GuiLoadStyleDefault();
    Font font = load_ui_font(36);
    GuiSetFont(font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 17);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt((Color){ 245, 246, 248, 255 }));

    /* selections */
    int sel_in = init_ok ? vf_session_default_mic(&s) : 0;
    if (sel_in < 0) sel_in = 0;
    int sel_out = 0, sel_mon = 0;
    for (int i = 0; i < s.plays.count; i++)
        if (s.plays.dev[i].is_default) sel_out = sel_mon = i;
    if (sel_mon < s.plays.count && vf_contains_ci(s.plays.dev[sel_mon].name, "CABLE"))
        for (int i = 0; i < s.plays.count; i++)
            if (!vf_contains_ci(s.plays.dev[i].name, "CABLE")) { sel_mon = i; break; }
    int k;
    if ((k = find_name(&s.caps, st.input)) >= 0) sel_in = k;
    if ((k = find_name(&s.plays, st.output)) >= 0) sel_out = k;
    if ((k = find_name(&s.plays, st.monitor)) >= 0) sel_mon = k;
    int sel_mode = mode >= 0 && mode <= 2 ? mode : (st.mode >= 0 && st.mode <= 2 ? st.mode : VF_OUT_VMIC);
    int edit_in = 0, edit_out = 0, edit_preset = 0, edit_name = 0;
    static char list_in[LIST_CHARS], list_out[LIST_CHARS], list_presets[LIST_CHARS];
    build_list(list_in, sizeof list_in, &s.caps);
    build_list(list_out, sizeof list_out, &s.plays);
    static VfPresetInfo presets[VF_MAX_PRESETS];
    int n_presets = 0, sel_preset = 0, shown_preset = 0;
    build_preset_list(list_presets, sizeof list_presets, presets, &n_presets);
    for (int i = 0; i < n_presets; i++) if (!strcmp(presets[i].name, st.preset)) sel_preset = shown_preset = i;
    char preset_name[64] = "";

    char status[300] = "", note[200] = "";
    if (!init_ok) snprintf(status, sizeof status, "%s", s.error);

    Meters m = { -120, -120, -120, -120, 0, 0, 0, 1, 0, 0 };
    int want_running = (autostart || st.start_processing) && init_ok;
    int applied_in = -1, applied_out = -1, applied_mon = -1, applied_mode = -1;
    int frame_no = 0;
    unsigned long long saved_hash = settings_hash(&s, &st, sel_in, sel_out, sel_mon, sel_mode);
    double dirty_since = -1;

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        /* ---- (re)start the session when the wanted configuration changes ---- */
        if (init_ok) {
            int cfg_changed = sel_in != applied_in || sel_out != applied_out || sel_mon != applied_mon ||
                              sel_mode != applied_mode;
            if (want_running && (!s.running || cfg_changed)) {
                VfSessionConfig c;
                memset(&c, 0, sizeof c);
                c.capture = file_buf ? -1 : sel_in;
                c.mode = (VfOutMode)sel_mode;
                c.playback = sel_mode == VF_OUT_HEADPHONES ? sel_out : -1;
                c.monitor = sel_mode == VF_OUT_VMIC_MONITOR ? sel_mon : -1;
                c.file_src = file_buf;
                c.file_len = file_len;
                if (vf_session_start(&s, &c) == 0) status[0] = '\0';
                else { snprintf(status, sizeof status, "%s", s.error); want_running = 0; }
                applied_in = sel_in; applied_out = sel_out; applied_mon = sel_mon; applied_mode = sel_mode;
            } else if (!want_running && s.running) {
                vf_session_stop(&s);
            }
            drain_meters(&s, &m, dt);
        }

        /* ---- autosave settings.ini one second after the last change ---- */
        if (init_ok && have_settings_path) {
            unsigned long long h = settings_hash(&s, &st, sel_in, sel_out, sel_mon, sel_mode);
            if (h != saved_hash) {
                if (dirty_since < 0) dirty_since = GetTime();
                if (GetTime() - dirty_since > 1.0) {
                    snprintf(st.input, sizeof st.input, "%s", sel_in < s.caps.count ? s.caps.dev[sel_in].name : "");
                    snprintf(st.output, sizeof st.output, "%s", sel_out < s.plays.count ? s.plays.dev[sel_out].name : "");
                    snprintf(st.monitor, sizeof st.monitor, "%s", sel_mon < s.plays.count ? s.plays.dev[sel_mon].name : "");
                    st.mode = sel_mode;
                    st.bypass = atomic_load(&s.pipe.bypass_all);
                    if (vf_settings_save(&st, &s.pipe, settings_file) != 0)
                        snprintf(status, sizeof status, "Nie udało się zapisać %s", settings_file);
                    saved_hash = h;
                    dirty_since = -1;
                }
            }
        }

        BeginDrawing();
        ClearBackground((Color){ 245, 246, 248, 255 });
        if (edit_in || edit_out || edit_preset) GuiLock();

        /* ---- header: devices and mode ---- */
        GuiLabel((Rectangle){ 16, 14, 80, 28 }, "Mikrofon");
        Rectangle r_in = { 96, 14, 400, 28 };
        GuiLabel((Rectangle){ 516, 14, 60, 28 }, "Wyjście");
        GuiToggleGroup((Rectangle){ 580, 14, 134, 28 }, "Słuchawki;Wirtualny mic;Mic + odsłuch", &sel_mode);
        Rectangle r_out = { 96, 52, 400, 28 };
        if (sel_mode == VF_OUT_HEADPHONES) {
            GuiLabel((Rectangle){ 16, 52, 80, 28 }, "Głośniki");
        } else if (sel_mode == VF_OUT_VMIC_MONITOR) {
            GuiLabel((Rectangle){ 16, 52, 80, 28 }, "Odsłuch");
        }
        char vm[160];
        if (sel_mode != VF_OUT_HEADPHONES) {
            snprintf(vm, sizeof vm, "W Discordzie / OBS wybierz mikrofon: \"%s\"", vf_session_vmic_name(&s));
            GuiLabel((Rectangle){ 516, 52, 470, 28 }, vm);
        }

        int bypass = atomic_load(&s.pipe.bypass_all);
        bool byp = bypass != 0;
        GuiToggle((Rectangle){ 1000, 52, 164, 28 }, byp ? "Filtr WYŁĄCZONY" : "Filtr włączony", &byp);
        if ((int)byp != bypass) atomic_store(&s.pipe.bypass_all, byp ? 1 : 0);
        if (GuiButton((Rectangle){ 1000, 14, 164, 28 }, s.running ? "#132# Stop" : "#131# Start")) {
            want_running = !s.running;
            if (want_running) applied_in = -2;            /* force (re)start */
        }

        /* ---- presets + app options ---- */
        GuiLabel((Rectangle){ 16, 90, 80, 28 }, "Preset");
        Rectangle r_preset = { 96, 90, 250, 28 };
        if (GuiTextBox((Rectangle){ 360, 90, 190, 28 }, preset_name, sizeof preset_name - 1, edit_name)) edit_name = !edit_name;
        if (GuiButton((Rectangle){ 556, 90, 120, 28 }, "Zapisz jako")) {
            char safe[64];
            if (vf_preset_sanitize(preset_name, safe, sizeof safe) != 0)
                snprintf(note, sizeof note, "Wpisz nazwę presetu w polu obok.");
            else if (vf_preset_is_builtin(safe))
                snprintf(note, sizeof note, "\"%s\" to preset wbudowany - wybierz inną nazwę.", safe);
            else if (vf_preset_save(&s.pipe, safe) != 0)
                snprintf(note, sizeof note, "Nie udało się zapisać presetu.");
            else {
                build_preset_list(list_presets, sizeof list_presets, presets, &n_presets);
                for (int i = 0; i < n_presets; i++) if (!strcmp(presets[i].name, safe)) sel_preset = shown_preset = i;
                snprintf(st.preset, sizeof st.preset, "%s", safe);
                snprintf(note, sizeof note, "Zapisano preset \"%s\".", safe);
                preset_name[0] = '\0';
                edit_name = 0;
            }
        }
        const int user_preset = sel_preset < n_presets && !presets[sel_preset].builtin;
        if (!user_preset) GuiDisable();
        if (GuiButton((Rectangle){ 682, 90, 80, 28 }, "Usuń") && user_preset) {
            char gone[64];
            snprintf(gone, sizeof gone, "%s", presets[sel_preset].name);
            if (vf_preset_delete(gone) == 0) {
                build_preset_list(list_presets, sizeof list_presets, presets, &n_presets);
                sel_preset = shown_preset = 0;
                snprintf(st.preset, sizeof st.preset, "%s", presets[0].name);
                snprintf(note, sizeof note, "Usunięto preset \"%s\".", gone);
            }
        }
        GuiEnable();
        bool as = autostart_on != 0;
        GuiCheckBox((Rectangle){ 790, 95, 18, 18 }, "Start z systemem", &as);
        if ((int)as != autostart_on) {
            if (vf_autostart_set(as) == 0) {
                autostart_on = as;
                snprintf(note, sizeof note, as ? "VoiceFilter uruchomi się z systemem (zminimalizowany)."
                                               : "Autostart wyłączony.");
            } else snprintf(status, sizeof status, "Nie udało się zmienić autostartu.");
        }
        bool sp = st.start_processing != 0;
        GuiCheckBox((Rectangle){ 960, 95, 18, 18 }, "Filtruj od razu po starcie", &sp);
        st.start_processing = sp;

        /* ---- meters ---- */
        GuiGroupBox((Rectangle){ 16, TOP, 250, 586 }, "Poziomy");
        int y = TOP + 18;
        char t[96];
        GuiLabel((Rectangle){ 28, y, 220, 20 }, "Wejście"); y += 22;
        draw_level((Rectangle){ 28, y, 226, 16 }, m.peak_in, m.hold_in);
        snprintf(t, sizeof t, "%.1f dBFS", (double)(m.hold_in < -99 ? -99 : m.hold_in));
        GuiLabel((Rectangle){ 28, y + 18, 226, 20 }, t); y += 52;
        GuiLabel((Rectangle){ 28, y, 220, 20 }, "Wyjście"); y += 22;
        draw_level((Rectangle){ 28, y, 226, 16 }, m.peak_out, m.hold_out);
        snprintf(t, sizeof t, "%.1f dBFS", (double)(m.hold_out < -99 ? -99 : m.hold_out));
        GuiLabel((Rectangle){ 28, y + 18, 226, 20 }, t); y += 60;

        /* VAD with the gate threshold marker */
        const int gi = pipeline_find_fx(&s.pipe, "gate");
        const float thr = gi >= 0 ? pipeline_get_param(&s.pipe, gi, 0) : 0.5f;
        const int voice = m.vad >= thr;
        GuiLabel((Rectangle){ 28, y, 220, 20 }, "Aktywność głosu (VAD)"); y += 22;
        draw_fill((Rectangle){ 28, y, 190, 16 }, m.vad, voice ? (Color){ 60, 190, 90, 255 } : (Color){ 130, 140, 150, 255 });
        DrawRectangle((int)(28 + 190 * thr) - 1, y - 2, 2, 20, (Color){ 230, 120, 40, 255 });
        DrawCircle(240, y + 8, 9, voice ? (Color){ 60, 200, 90, 255 } : (Color){ 200, 205, 210, 255 });
        snprintf(t, sizeof t, "%d %%  (próg %d %%)", (int)(m.vad * 100 + 0.5f), (int)(thr * 100 + 0.5f));
        GuiLabel((Rectangle){ 28, y + 18, 226, 20 }, t); y += 52;

        GuiLabel((Rectangle){ 28, y, 220, 20 }, "Bramka - otwarcie"); y += 22;
        draw_fill((Rectangle){ 28, y, 226, 16 }, m.gate, (Color){ 80, 140, 220, 255 });
        snprintf(t, sizeof t, "%d %%", (int)(m.gate * 100 + 0.5f));
        GuiLabel((Rectangle){ 28, y + 18, 226, 20 }, t); y += 52;

        GuiLabel((Rectangle){ 28, y, 220, 20 }, "Kompresja"); y += 22;
        draw_fill((Rectangle){ 28, y, 226, 16 }, -m.gr / 24.0f, (Color){ 200, 110, 60, 255 });
        snprintf(t, sizeof t, "%.1f dB", (double)m.gr);
        GuiLabel((Rectangle){ 28, y + 18, 226, 20 }, t); y += 60;

        GuiLabel((Rectangle){ 28, y, 226, 20 }, "Opóźnienie (szacunek)"); y += 22;
        if (s.running) {
            EngineLatency l;
            engine_latency(&s.eng, &l);
            const double sink_ms = 1000.0 * VF_SINK_TARGET / VF_SAMPLE_RATE;
            const double total = s.use_pw ? l.capture_ms + l.dsp_ms + sink_ms : l.total_ms;
            snprintf(t, sizeof t, "~%.0f ms%s", total,
                     s.use_pw ? " + PipeWire" : sel_mode != VF_OUT_HEADPHONES ? " + VB-Cable" : "");
            DrawTextEx(font, t, (Vector2){ 28, (float)y }, 17, 0, DARKGRAY); y += 20;
            if (s.use_pw) snprintf(t, sizeof t, "wej. %.0f + DSP %.1f + bufor %.0f ms", l.capture_ms, l.dsp_ms, sink_ms);
            else snprintf(t, sizeof t, "wej. %.0f + wyj. %.0f + FIFO %.0f + DSP %.1f ms",
                          l.capture_ms, l.playback_ms, l.fifo_ms, l.dsp_ms);
            DrawTextEx(font, t, (Vector2){ 28, (float)y }, 14, 0, GRAY); y += 30;
        } else {
            DrawTextEx(font, "-", (Vector2){ 28, (float)y }, 17, 0, DARKGRAY); y += 50;
        }
        snprintf(t, sizeof t, "Przerwy w dźwięku: %u", vf_session_xruns(&s));
        GuiLabel((Rectangle){ 28, y, 226, 20 }, t);

        /* ---- effect panels: 3 columns x 2 rows, built from the parameter tables ---- */
        for (int i = 0; i < s.pipe.count; i++) {
            Fx *fx = s.pipe.fx[i];
            const float pw = 292, ph = 290;
            Rectangle box = { 282 + (i % 3) * (pw + 6), TOP + (i / 3) * (ph + 6), pw, ph };
            GuiGroupBox(box, fx_title(fx->name));
            bool on = !atomic_load(&fx->bypass);
            GuiCheckBox((Rectangle){ box.x + 12, box.y + 16, 18, 18 }, "włączony", &on);
            if (on == (bool)atomic_load(&fx->bypass)) pipeline_set_bypass(&s.pipe, i, !on);
            if (GuiButton((Rectangle){ box.x + box.width - 92, box.y + 12, 80, 24 }, "Domyślne"))
                for (int j = 0; j < fx->n_params; j++) pipeline_set_param(&s.pipe, i, j, fx->params[j].def);
            if (!on) GuiDisable();
            float py = box.y + 48;
            for (int j = 0; j < fx->n_params; j++) {
                const FxParamInfo *pi = &fx->params[j];
                float v = pipeline_get_param(&s.pipe, i, j);
                if (pi->toggle) {
                    bool b = v >= 0.5f;
                    GuiCheckBox((Rectangle){ box.x + 12, py + 4, 18, 18 }, param_label(fx->name, pi->name), &b);
                    if (b != (v >= 0.5f)) pipeline_set_param(&s.pipe, i, j, b ? 1.0f : 0.0f);
                    py += 34;
                    continue;
                }
                GuiLabel((Rectangle){ box.x + 12, py, 170, 18 }, param_label(fx->name, pi->name));
                char val[48];
                const int ints = pi->max - pi->min >= 50.0f;
                snprintf(val, sizeof val, ints ? "%.0f %s" : "%.2f %s", (double)v, pi->unit);
                GuiLabel((Rectangle){ box.x + 180, py, 100, 18 }, val);
                float nv = v;
                GuiSliderBar((Rectangle){ box.x + 12, py + 19, box.width - 24, 10 }, NULL, NULL, &nv, pi->min, pi->max);
                if (nv != v) {
                    if (ints) nv = roundf(nv);
                    pipeline_set_param(&s.pipe, i, j, nv);
                }
                py += 34;
            }
            GuiEnable();
        }

        /* ---- status bar ---- */
        char sb[600];
        if (status[0]) snprintf(sb, sizeof sb, "Błąd: %s", status);
        else if (s.running) snprintf(sb, sizeof sb, "Działa - %s%s%s%s", s.use_pw ? "PipeWire" : ma_get_backend_name(s.ctx.backend),
                                     file_buf ? " (źródło: plik testowy)" : "", note[0] ? "   |   " : "", note);
        else snprintf(sb, sizeof sb, "Zatrzymane - wybierz urządzenia i naciśnij Start%s%s", note[0] ? "   |   " : "", note);
        GuiStatusBar((Rectangle){ 0, WIN_H - 26, WIN_W, 26 }, sb);
        if (status[0]) DrawRectangle(0, WIN_H - 26, 4, 26, RED);

        /* ---- dropdowns last, so their open lists cover the rest ---- */
        GuiUnlock();
        if (sel_mode != VF_OUT_VMIC) {
            int *target = sel_mode == VF_OUT_HEADPHONES ? &sel_out : &sel_mon;
            if (GuiDropdownBox(r_out, list_out, target, edit_out)) edit_out = !edit_out;
        }
        if (s.running && file_buf) GuiDisable();
        if (GuiDropdownBox(r_in, list_in, &sel_in, edit_in)) edit_in = !edit_in;
        GuiEnable();
        if (GuiDropdownBox(r_preset, list_presets, &sel_preset, edit_preset)) edit_preset = !edit_preset;
        if (!edit_preset && sel_preset != shown_preset && sel_preset < n_presets) {
            if (vf_preset_apply(&s.pipe, presets[sel_preset].name) == 0) {
                snprintf(st.preset, sizeof st.preset, "%s", presets[sel_preset].name);
                snprintf(note, sizeof note, "Wczytano preset \"%s\".", presets[sel_preset].name);
            } else snprintf(status, sizeof status, "Nie udało się wczytać presetu.");
            shown_preset = sel_preset;
        }

        EndDrawing();

        if (shot && ++frame_no >= (frames_to_shot > 0 ? frames_to_shot : 60)) {
            TakeScreenshot(shot);
            break;
        }
    }

    /* final save (also catches changes made within the last second) */
    if (init_ok && have_settings_path && !shot) {
        snprintf(st.input, sizeof st.input, "%s", sel_in < s.caps.count ? s.caps.dev[sel_in].name : "");
        snprintf(st.output, sizeof st.output, "%s", sel_out < s.plays.count ? s.plays.dev[sel_out].name : "");
        snprintf(st.monitor, sizeof st.monitor, "%s", sel_mon < s.plays.count ? s.plays.dev[sel_mon].name : "");
        st.mode = sel_mode;
        st.bypass = atomic_load(&s.pipe.bypass_all);
        vf_settings_save(&st, &s.pipe, settings_file);
    }
    vf_session_free(&s);
    UnloadFont(font);
    CloseWindow();
    free(file_buf);
    return 0;
}
