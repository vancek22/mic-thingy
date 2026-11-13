/* GUI part 2 (included by main_gui.c, same translation unit): everything added after the first
 * release - mute/hotkeys/auto-mute, Tools and Settings tabs, compact window, preset sharing,
 * undo, per-program presets. Shares the drawing helpers and the session with main_gui.c. */

/* ---- state ---- */
#define COMPACT_W 300
#define COMPACT_H 150
#define HK_COUNT  (3 + VF_BOARD_SLOTS)      /* mute, ptt, bypass, board slots */

typedef struct Ui {
    int tab;                                /* 0..2 effect pages, 3 tools, 4 settings */
    int compact;
    VfUndo undo;
    VfSnap seen, cmp_snap;
    int undo_pending, comparing;
    double change_t;
    VfAutoMute am;
    VfHotkey hk[HK_COUNT];
    int hk_was[HK_COUNT];
    int hk_capture;                         /* index being assigned, -1 none */
    int hk_wait_release;
    int ptt_block;
    int locked;
    double next_second, next_lock, next_apps;
    VfStats stats;
    VfCalibrate cal;
    VfAbTest ab;
    VfRecorder rec;
    char rec_path[1024];
    int rec_was_active;
    VfSpectrum spec;
    VfRing scope;
    int scope_ready;
    char board_edit[260];
    char proc_find[VF_APP_NAME];            /* search text of the program picker */
    int proc_find_on;
    char proc_name[VF_APP_NAME];            /* program currently picked (by name: the process list changes) */
    int board_edit_on;
    char procs[256][VF_APP_NAME];
    int n_procs, proc_sel, preset_sel;
    int app_active_rule;                    /* rule currently applied, -1 none */
    char app_prev_preset[64];
    int prev_sel_in;
    int sys_running;
    double sys_retry;
    int idle_noted;
    VfHowl howl;
    long talk_frames;                       /* 10 ms frames with the voice detector above 0.5 */
    int corner_shown;
    unsigned long long hk_hash;
} Ui;

static Ui U;

/* everything below needs these (set by main) */
static char *g_note;
static size_t g_note_cap;

static void note_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_note, g_note_cap, fmt, ap);
    va_end(ap);
}

static float db_to_lin_f(float db) { return powf(10.0f, db * 0.05f); }

/* ---- hotkeys ---- */

static char *hk_text(int i)
{
    if (i == 0) return g_st->hk_mute;
    if (i == 1) return g_st->hk_ptt;
    if (i == 2) return g_st->hk_bypass;
    return g_st->hk_board[i - 3];
}

static void sync_hotkeys(void)
{
    for (int i = 0; i < HK_COUNT; i++)
        if (vf_hotkey_parse(hk_text(i), &U.hk[i]) != 0) { U.hk[i].n = 0; snprintf(hk_text(i), 48, "none"); }
    vf_hotkeys_grab(U.hk, g_st->hk_grab ? HK_COUNT : 0);
}

/* ---- meter stream consumers (called for every 10 ms frame) ---- */

static void ui_frame(const float *f)
{
    vf_stats_frame(&U.stats, f);
    if (f[VF_M_VAD] > 0.5f) U.talk_frames++;
    if (U.cal.state == VF_CAL_SILENCE || U.cal.state == VF_CAL_SPEECH) vf_cal_frame(&U.cal, f);
}

/* ---- window helpers ---- */

static void remember_window_pos(void)
{
    if (!g_win_open) return;
    const Vector2 p = GetWindowPosition();
    if (U.compact) { g_st->cwin_x = (int)p.x; g_st->cwin_y = (int)p.y; }
    else { g_st->win_x = (int)p.x; g_st->win_y = (int)p.y; }
}

static void apply_window_mode(int compact)
{
    if (!g_win_open) { U.compact = compact; return; }
    remember_window_pos();
    U.compact = compact;
    g_st->compact = compact;
    if (compact) {
        SetWindowSize(COMPACT_W, COMPACT_H);
        SetWindowState(FLAG_WINDOW_TOPMOST);
        if (g_st->cwin_x != -32000 && window_pos_ok(g_st->cwin_x, g_st->cwin_y)) SetWindowPosition(g_st->cwin_x, g_st->cwin_y);
    } else {
        ClearWindowState(FLAG_WINDOW_TOPMOST);
        SetWindowSize(WIN_W, WIN_H);
        if (g_st->win_x != -32000 && window_pos_ok(g_st->win_x, g_st->win_y)) SetWindowPosition(g_st->win_x, g_st->win_y);
    }
}

/* ---- small widgets ---- */

static Color dis_col(int on) { return on ? C_TEXT : C_DISABLED; }

/* label + value + slider; returns 1 if the value changed */
static int slider_row(Rectangle box, float *py, const char *label, float *v, float lo, float hi, const char *fmt, int on)
{
    char val[48];
    snprintf(val, sizeof val, fmt, (double)*v);
    text(F.ui, label, box.x + 14, *py, dis_col(on));
    const float vw = MeasureTextEx(F.lcd, val, 14, 0).x;
    text(F.lcd, val, box.x + box.width - 14 - vw, *py + 2, on ? C_NEON : C_DISABLED);
    float nv = *v;
    Rectangle sr_ = { box.x + 14, *py + 19, box.width - 28, 10 };
    GuiSliderBar(sr_, NULL, NULL, &nv, lo, hi);
    const float wheel = GetMouseWheelMove();
    if (on && wheel != 0 && !GuiIsLocked() && CheckCollisionPointRec(GetMousePosition(), (Rectangle){ sr_.x, sr_.y - 8, sr_.width, sr_.height + 14 }))
        nv += wheel * (hi - lo) / 100.0f;
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    *py += 34;
    if (nv != *v) { *v = nv; return 1; }
    return 0;
}

static int check_row(Rectangle box, float *py, const char *label, int *flag, int on)
{
    bool b = *flag != 0;
    if (!on) GuiDisable();
    GuiCheckBox((Rectangle){ box.x + 14, *py + 4, 18, 18 }, label, &b);
    if (!on) GuiEnable();
    *py += 30;
    if ((int)b != *flag) { *flag = b; return 1; }
    return 0;
}

static void note_line(Rectangle box, float *py, const char *s, Color c)
{
    text(F.ui, s, box.x + 14, *py, c);
    *py += 20;
}

/* ---- tools tab ---- */

/* Draws s in one line of at most maxw pixels; a too long text keeps its tail ("...C:\folder"). */
static void text_fit(Font f, const char *s, float x, float y, float size, float maxw, Color c)
{
    if (MeasureTextEx(f, s, size, 0).x <= maxw) { DrawTextEx(f, s, (Vector2){ x, y }, size, 0, c); return; }
    char buf[VF_PATH_MAX];
    size_t off = 0;
    const size_t len = strlen(s);
    while (off < len) {
        snprintf(buf, sizeof buf, "...%s", s + off);
        if (MeasureTextEx(f, buf, size, 0).x <= maxw) break;
        off++;
    }
    DrawTextEx(f, buf, (Vector2){ x, y }, size, 0, c);
}

static const char *quality_text(const VfQualityInfo *q)
{
    switch (q->verdict) {
    case VF_Q_NO_DATA:    return tr(S_Q_NODATA);
    case VF_Q_NO_SPEECH:  return tr(S_Q_NOSPEECH);
    case VF_Q_TOO_QUIET:  return tr(S_Q_QUIET);
    case VF_Q_TOO_LOUD:   return tr(S_Q_LOUD);
    case VF_Q_LIMITER_BUSY: return tr(S_Q_LIMITER);
    default:              return tr(S_Q_OK);
    }
}

static void spectrum_panel(Rectangle box)
{
    Rectangle r = { box.x + 12, box.y + 40, box.width - 24, box.height - 54 };
    lcd(r);
    const float f_lo = 50.0f, f_hi = 20000.0f;
    const int w = (int)r.width - 4;
    for (int g = 0; g < 4; g++) {                                   /* frequency grid: 100 Hz, 1 k, 10 k */
        static const float fg[] = { 100.0f, 500.0f, 2000.0f, 10000.0f };
        const float x = r.x + 2 + w * logf(fg[g] / f_lo) / logf(f_hi / f_lo);
        DrawLine((int)x, (int)r.y + 2, (int)x, (int)(r.y + r.height) - 2, Fade(C_TEXT, 0.1f));
    }
    /* bin range of every pixel column: computed once per panel width, not every frame */
    static short kat[640], kbt[640];
    static int table_w;
    if (table_w != w && w <= 640) {
        for (int px = 0; px < w; px++) {
            const float fa = f_lo * powf(f_hi / f_lo, (float)px / (float)w), fb = f_lo * powf(f_hi / f_lo, (float)(px + 1) / (float)w);
            int a = (int)(fa * VF_SPEC_N / VF_SAMPLE_RATE), b = (int)(fb * VF_SPEC_N / VF_SAMPLE_RATE);
            if (b < a) b = a;
            if (b >= VF_SPEC_BINS) b = VF_SPEC_BINS - 1;
            kat[px] = (short)a; kbt[px] = (short)b;
        }
        table_w = w;
    }
    for (int px = 0; px < w && px < 640; px++) {
        const int ka = kat[px], kb = kbt[px];
        float vi = -120.0f, vo = -120.0f;
        for (int k = ka; k <= kb; k++) { if (U.spec.in_db[k] > vi) vi = U.spec.in_db[k]; if (U.spec.out_db[k] > vo) vo = U.spec.out_db[k]; }
        const float hi_ = fminf(1.0f, fmaxf(0.0f, (vi + 90.0f) / 90.0f)), ho_ = fminf(1.0f, fmaxf(0.0f, (vo + 90.0f) / 90.0f));
        const float h = r.height - 6;
        DrawRectangle((int)r.x + 2 + px, (int)(r.y + 3 + h * (1 - ho_)), 1, (int)(h * ho_) + 1, Fade(C_NEON, 0.75f));
        DrawRectangle((int)r.x + 2 + px, (int)(r.y + 3 + h * (1 - hi_)), 1, 1, C_AMBER);
    }
    text(F.lcd, "in", r.x + 6, r.y + 4, C_AMBER);
    text(F.lcd, "out", r.x + 28, r.y + 4, C_NEON);
    text(F.lcd, "100", r.x + 2 + w * logf(100.0f / f_lo) / logf(f_hi / f_lo) - 8, r.y + r.height - 16, C_DISABLED);
    text(F.lcd, "1k", r.x + 2 + w * logf(1000.0f / f_lo) / logf(f_hi / f_lo) - 6, r.y + r.height - 16, C_DISABLED);
    text(F.lcd, "10k", r.x + 2 + w * logf(10000.0f / f_lo) / logf(f_hi / f_lo) - 10, r.y + r.height - 16, C_DISABLED);
}

static void draw_tools(Rectangle area, VfSession *s, VfSettings *st, int sel_mode, int sel_out, int sel_mon)
{
    const float pw = 292, ph = 276;
    char t[160];
    Rectangle box;
    const int running = s->running;

    /* --- 1: monitor and mix --- */
    box = (Rectangle){ area.x, area.y, pw, ph };
    panel(box, tr(S_PN_MIX), -1);
    float py = box.y + 36;
    const int mon_mode = sel_mode == VF_OUT_VMIC_MONITOR;
    int on = mon_mode;
    if (!on) GuiDisable();
    float g = st->monitor_gain_db;
    if (slider_row(box, &py, tr(S_MON_GAIN), &g, -40.0f, 6.0f, "%.0f dB", on)) st->monitor_gain_db = g;
    int raw = st->monitor_raw;
    if (check_row(box, &py, tr(S_MON_RAW), &raw, on)) st->monitor_raw = raw;
    if (!on) GuiEnable();
    if (!mon_mode) text(F.ui, tr(S_MON_HINT), box.x + 14, py - 4, C_DISABLED);
    if (mon_mode && running) vf_session_monitor_set(s, st->monitor_gain_db, st->monitor_raw);
    py += 16;
#ifdef _WIN32
    int sys = st->sys_enabled;
    if (check_row(box, &py, tr(S_SYS_AUDIO), &sys, 1)) st->sys_enabled = sys;
    float sg = st->sys_gain_db, sd = st->sys_duck_db;
    if (slider_row(box, &py, tr(S_SYS_GAIN), &sg, -40.0f, 6.0f, "%.0f dB", st->sys_enabled)) st->sys_gain_db = sg;
    if (slider_row(box, &py, tr(S_SYS_DUCK), &sd, -30.0f, 0.0f, "%.0f dB", st->sys_enabled)) st->sys_duck_db = sd;
    text(F.ui, tr(S_SYS_WARN), box.x + 14, py - 4, C_AMBER);
#else
    text(F.ui, tr(S_SYS_NA), box.x + 14, py, C_DISABLED);
#endif
    (void)sel_out; (void)sel_mon;

    /* --- 2: test and record --- */
    box = (Rectangle){ area.x + (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_TEST), U.ab.state == VF_AB_RECORDING || vf_rec_active(&U.rec) ? 0 : -1);
    py = box.y + 36;
    const VfAbState abs_ = (VfAbState)atomic_load(&U.ab.state);
    if (!running) GuiDisable();
    if (GuiButton((Rectangle){ box.x + 14, py, 130, 26 }, tr(S_AB_REC)) && running && abs_ != VF_AB_RECORDING) {
        vf_ab_stop(&U.ab);
        if (vf_ab_record(&U.ab, &s->eng, 10) != 0) note_set("%s", tr(S_N_REC_FAIL));
    }
    GuiEnable();
    int match = atomic_load(&U.ab.match);
    bool mb = match != 0;
    GuiCheckBox((Rectangle){ box.x + 156, py + 4, 18, 18 }, tr(S_AB_MATCH), &mb);
    atomic_store(&U.ab.match, mb);
    py += 32;
    if (!running) snprintf(t, sizeof t, "%s", tr(S_NEEDS_RUN));
    else if (abs_ == VF_AB_RECORDING) snprintf(t, sizeof t, tr(S_AB_REC_FMT), vf_ab_seconds(&U.ab));
    else if (abs_ == VF_AB_PLAYING) snprintf(t, sizeof t, tr(S_AB_PLAY_FMT), vf_ab_play_seconds(&U.ab));
    else if (abs_ == VF_AB_READY) snprintf(t, sizeof t, tr(S_AB_READY_FMT), vf_ab_seconds(&U.ab));
    else snprintf(t, sizeof t, "%s", tr(S_AB_IDLE));
    text(F.ui, t, box.x + 14, py, abs_ == VF_AB_RECORDING ? C_BURG_HI : C_TEXT);
    py += 24;
    if (abs_ != VF_AB_READY && abs_ != VF_AB_PLAYING) GuiDisable();
    static const VfAbWhich which_of[3] = { VF_AB_RAW, VF_AB_FILTERED, VF_AB_ALTERNATE };
    const char *lab[3] = { tr(S_AB_RAW), tr(S_AB_FILT), tr(S_AB_ALT) };
    for (int i = 0; i < 3; i++)
        if (GuiButton((Rectangle){ box.x + 14 + i * 88, py, 84, 26 }, lab[i])) {
            const ma_device_id *pid = NULL;
            const int pi_ = sel_mode == VF_OUT_HEADPHONES ? sel_out : sel_mon;
            if (pi_ >= 0 && pi_ < s->plays.count && sel_mode != VF_OUT_VMIC) pid = &s->plays.dev[pi_].id;
            if (vf_ab_play(&U.ab, &s->ctx, pid, which_of[i]) != 0) note_set("%s", tr(S_N_REC_FAIL));
        }
    GuiEnable();
    py += 38;
    /* recorder */
    const int recording = vf_rec_active(&U.rec);
    if (!running && !recording) GuiDisable();
    if (onoff((Rectangle){ box.x + 14, py, box.width - 28, 28 }, recording, tr(S_REC_STOP), tr(S_REC_START)) != recording) {
        if (recording) {
            const int ok = vf_rec_stop(&U.rec) == 0;
            if (ok) note_set(tr(S_N_REC_SAVED_FMT), U.rec_path); else note_set("%s", tr(S_N_REC_FAIL));
        } else if (vf_rec_start(&U.rec, &s->eng, NULL, 0, U.rec_path) != 0) note_set("%s", tr(S_N_REC_FAIL));
    }
    GuiEnable();
    py += 34;
    if (recording) { snprintf(t, sizeof t, tr(S_REC_FMT), vf_rec_seconds(&U.rec)); text(F.lcd, t, box.x + 14, py, C_BURG_HI); }
    else text(F.ui, tr(S_REC_NOTE), box.x + 14, py, C_DISABLED);

    /* --- 3: calibration --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_CAL), (U.cal.state == VF_CAL_SILENCE || U.cal.state == VF_CAL_SPEECH) ? 0 : -1);
    py = box.y + 36;
    const int cal_busy = U.cal.state == VF_CAL_SILENCE || U.cal.state == VF_CAL_SPEECH;
    if (!running || cal_busy) GuiDisable();
    if (GuiButton((Rectangle){ box.x + 14, py, box.width - 28, 28 }, tr(S_CAL_START)) && running) vf_cal_start(&U.cal);
    GuiEnable();
    py += 38;
    if (U.cal.state == VF_CAL_IDLE) {
        text(F.ui, running ? tr(S_CAL_HELP) : tr(S_NEEDS_RUN), box.x + 14, py, C_DISABLED);
    } else {
        if (U.cal.state == VF_CAL_SILENCE) snprintf(t, sizeof t, tr(S_CAL_SIL_FMT), VF_CAL_SILENCE_S - U.cal.frames / 100);
        else if (U.cal.state == VF_CAL_SPEECH) snprintf(t, sizeof t, tr(S_CAL_SPEECH_FMT), VF_CAL_SPEECH_S - U.cal.frames / 100);
        else if (U.cal.state == VF_CAL_DONE) snprintf(t, sizeof t, tr(S_CAL_DONE_FMT), (double)U.cal.result.noise_db, (double)U.cal.result.speech_db);
        else snprintf(t, sizeof t, tr(S_CAL_FAIL_FMT), U.cal.why);
        text(F.ui, t, box.x + 14, py, U.cal.state == VF_CAL_FAILED ? C_RED : C_TEXT);
        py += 24;
        led_bar((Rectangle){ box.x + 14, py, box.width - 28, 16 }, vf_cal_progress(&U.cal), C_NEON);
        py += 30;
        if (U.cal.state == VF_CAL_DONE) {
            snprintf(t, sizeof t, tr(S_CAL_PROP_FMT), (double)U.cal.result.gate_threshold, (double)U.cal.result.comp_threshold_db, (double)U.cal.result.comp_makeup_db);
            text(F.lcd, t, box.x + 14, py, C_NEON);
            py += 28;
            if (GuiButton((Rectangle){ box.x + 14, py, 120, 28 }, tr(S_CAL_APPLY))) {
                vf_cal_apply(&U.cal, &s->pipe);
                note_set("%s", tr(S_N_CAL_APPLIED));
                U.cal.state = VF_CAL_IDLE;
            }
        }
    }

    /* --- 4: soundboard --- */
    box = (Rectangle){ area.x, area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_BOARD), -1);
    py = box.y + 36;
    const Vector2 mouse = GetMousePosition();
    for (int i = 0; i < VF_BOARD_SLOTS; i++) {
        const int col = i / 4, row = i % 4;
        Rectangle b = { box.x + 12 + col * 136, py + row * 32, 108, 26 };
        VfClip *c = &s->board.clip[i];
        const int has = c->pcm != NULL, playing = vf_board_playing(&s->board, i);
        snprintf(t, sizeof t, "%d %s", i + 1, has ? c->name : tr(S_BOARD_EMPTY));
        if (!has) GuiDisable();
        if (onoff(b, playing, t, t) != playing) {
            if (playing) vf_board_stop(&s->board, i); else vf_board_play(&s->board, i);
        }
        if (!has) GuiEnable();
        if (has && GuiButton((Rectangle){ b.x + 110, b.y, 18, 26 }, "x")) { vf_board_clear(&s->board, i); st->board_path[i][0] = '\0'; }
        if (CheckCollisionPointRec(mouse, b) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && has) {   /* assign a hotkey */
            U.hk_capture = 3 + i;
            U.hk_wait_release = 1;
            note_set("%s", tr(S_HK_PRESS));
        }
        if (U.hk[3 + i].n > 0) {
            char hk[48];
            vf_hotkey_format(&U.hk[3 + i], hk, sizeof hk);
            text(F.lcd, hk, b.x + 2, b.y + 27, C_DISABLED);
        }
    }
    py += 4 * 32 + 6;
    float bg = st->board_gain_db;
    if (slider_row(box, &py, tr(S_BOARD_GAIN), &bg, -30.0f, 12.0f, "%.0f dB", 1)) st->board_gain_db = bg;
    if (GuiTextBox((Rectangle){ box.x + 12, py, box.width - 24 - 96, 26 }, U.board_edit, (int)sizeof U.board_edit - 1, U.board_edit_on)) U.board_edit_on = !U.board_edit_on;
    if (GuiButton((Rectangle){ box.x + box.width - 12 - 88, py, 88, 26 }, tr(S_BOARD_ADD)) && U.board_edit[0]) {
        int slot = -1;
        for (int i = 0; i < VF_BOARD_SLOTS; i++) if (!s->board.clip[i].pcm) { slot = i; break; }
        if (slot < 0) note_set("%s", tr(S_BOARD_FULL));
        else {
            const int r = vf_board_load(&s->board, slot, U.board_edit);
            if (r == 0) { snprintf(st->board_path[slot], sizeof st->board_path[slot], "%s", U.board_edit); U.board_edit[0] = '\0'; }
            else note_set(r == -2 ? "%s" : r == -3 ? "%s" : tr(S_BOARD_FAIL_FMT), r == -2 ? tr(S_BOARD_LONG) : r == -3 ? tr(S_BOARD_MEM) : U.board_edit);
        }
    }
    text(F.ui, tr(S_BOARD_HINT), box.x + 12, py + 30, C_DISABLED);

    /* --- 5: quality and dropout history --- */
    box = (Rectangle){ area.x + (pw + 6), area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_QUALITY), -1);
    py = box.y + 36;
    VfQualityInfo q;
    vf_stats_quality(&U.stats, 15, &q);
    const Color qc = q.verdict == VF_Q_OK ? C_NEON : q.verdict == VF_Q_NO_DATA || q.verdict == VF_Q_NO_SPEECH ? C_TEXT_DIM : C_AMBER;
    text(F.bold, quality_text(&q), box.x + 14, py, qc);
    py += 26;
    if (q.verdict != VF_Q_NO_DATA && q.verdict != VF_Q_NO_SPEECH) {
        snprintf(t, sizeof t, tr(S_Q_SPEECH_FMT), (double)q.speech_db, (double)q.noise_cut_db);
        text(F.lcd, t, box.x + 14, py, C_TEXT);
        py += 20;
        snprintf(t, sizeof t, tr(S_Q_LIM_FMT), (double)q.limiter_pct);
        text(F.lcd, t, box.x + 14, py, q.limiter_pct > 30 ? C_AMBER : C_TEXT);
        py += 26;
    } else py += 12;
    { const long ts = U.talk_frames / 100; snprintf(t, sizeof t, tr(S_TALK_FMT), (int)(ts / 60), (int)(ts % 60)); text(F.lcd, t, box.x + 14, box.y + ph - 22, C_TEXT_DIM); }
    text(F.ui, tr(S_Q_HISTORY), box.x + 14, py, C_TEXT_DIM);
    py += 22;
    unsigned hist[30];
    const int nh = vf_stats_xrun_history(&U.stats, 30, hist);
    unsigned hmax = 4;
    for (int i = 0; i < nh; i++) if (hist[i] > hmax) hmax = hist[i];
    Rectangle hr = { box.x + 14, py, box.width - 28, 70 };
    lcd(hr);
    for (int i = 0; i < nh; i++) {
        const float hgt = (hr.height - 8) * (float)hist[i] / (float)hmax;
        const float bw = (hr.width - 4) / (float)nh;
        DrawRectangle((int)(hr.x + 2 + i * bw), (int)(hr.y + hr.height - 4 - hgt), (int)bw - 1, hist[i] ? (int)hgt + 1 : 1, hist[i] ? C_AMBER : C_SEG_OFF);
    }

    /* --- 6: spectrum --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_SPECTRUM), -1);
    spectrum_panel(box);
}

/* ---- settings tab ---- */

static const char *hk_row_label(int i)
{
    return i == 0 ? tr(S_HK_MUTE) : i == 1 ? tr(S_HK_PTT) : tr(S_HK_BYPASS);
}

static void draw_settings(Rectangle area, VfSession *s, VfSettings *st, VfPresetInfo *presets, int n_presets, int *sel_preset, int tray_ok)
{
    const float pw = 292, ph = 276;
    char t[200];
    Rectangle box;
    (void)tray_ok;

    /* --- 1: look and window --- */
    box = (Rectangle){ area.x, area.y, pw, ph };
    panel(box, tr(S_PN_LOOK), -1);
    float py = box.y + 36;
    text(F.ui, tr(S_THEME), box.x + 14, py, C_TEXT);
    py += 24;
    int th = vf_theme_find(st->theme);
    const int th0 = th;
    GuiToggleGroup((Rectangle){ box.x + 14, py, 84, 26 }, tr(S_THEMES), &th);
    if (th != th0) { snprintf(st->theme, sizeof st->theme, "%s", vf_theme_get(th)->id); retheme(th); }
    py += 40;
    int cmp = U.compact;
    if (check_row(box, &py, tr(S_COMPACT_MODE), &cmp, 1)) apply_window_mode(cmp);
#ifdef _WIN32
    int ov = st->overlay;
    if (check_row(box, &py, tr(S_OVERLAY), &ov, 1)) st->overlay = ov;
    if (!st->overlay) GuiDisable();
    int corner = st->overlay_corner;
    GuiToggleGroup((Rectangle){ box.x + 14, py, 48, 26 }, tr(S_CORNERS), &corner);
    st->overlay_corner = corner & 3;
    GuiEnable();
#else
    text(F.ui, tr(S_OVERLAY_NA), box.x + 14, py, C_DISABLED);
#endif

    /* --- 2: behaviour --- */
    box = (Rectangle){ area.x + (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_BEHAVIOUR), -1);
    py = box.y + 36;
    int pm = st->per_mic;
    if (check_row(box, &py, tr(S_PER_MIC), &pm, 1)) st->per_mic = pm;
    int hg = st->howl_guard;
    if (check_row(box, &py, tr(S_HOWL_GUARD), &hg, 1)) st->howl_guard = hg;
#ifdef _WIN32
    int al = st->auto_mute_lock;
    if (check_row(box, &py, tr(S_AUTOMUTE_LOCK), &al, 1)) { st->auto_mute_lock = al; }
#endif
    float idle = (float)st->auto_mute_idle_min;
    if (slider_row(box, &py, tr(S_AUTOMUTE_IDLE), &idle, 0.0f, 60.0f, "%.0f", 1)) st->auto_mute_idle_min = (int)(idle + 0.5f);
    int ptt = st->ptt_enabled;
    if (check_row(box, &py, tr(S_PTT), &ptt, 1)) st->ptt_enabled = ptt;
    text(F.ui, tr(S_PTT_HINT), box.x + 34, py - 8, C_DISABLED);
    py += 12;
    int ap = st->apps_enabled;
    if (check_row(box, &py, tr(S_APPS_ENABLE), &ap, 1)) { st->apps_enabled = ap; }

    /* --- 3: hotkeys --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_HOTKEYS), U.hk_capture >= 0 ? 0 : -1);
    py = box.y + 40;
    for (int i = 0; i < 3; i++) {
        text(F.ui, hk_row_label(i), box.x + 14, py + 4, C_TEXT);
        char hk[48];
        if (U.hk_capture == i) snprintf(hk, sizeof hk, "%s", tr(S_HK_PRESS));
        else vf_hotkey_format(&U.hk[i], hk, sizeof hk);
        Rectangle lr = { box.x + 112, py, 110, 26 };
        lcd(lr);
        text(F.lcd, hk, lr.x + 6, lr.y + 6, U.hk_capture == i ? C_AMBER : C_NEON);
        if (GuiButton((Rectangle){ box.x + 228, py, 50, 26 }, U.hk[i].n ? "x" : tr(S_HK_SET))) {
            if (U.hk[i].n && U.hk_capture != i) { snprintf(hk_text(i), 48, "none"); sync_hotkeys(); }
            else { U.hk_capture = i; U.hk_wait_release = 1; }
        }
        py += 38;
    }
    text(F.ui, tr(S_HK_NOTE), box.x + 14, py, C_DISABLED);
    py += 22;
    { int g = st->hk_grab; if (check_row(box, &py, tr(S_HK_GRAB), &g, 1)) { st->hk_grab = g; sync_hotkeys(); } }

    /* --- 4: presets per program --- */
    box = (Rectangle){ area.x, area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_APPS), st->apps_enabled ? 2 : -1);
    py = box.y + 36;
    for (int i = 0; i < st->n_apps; i++) {
        snprintf(t, sizeof t, "%s  >  %s", st->apps[i].exe, st->apps[i].preset);
        text(F.lcd, t, box.x + 14, py + 4, i == U.app_active_rule ? C_NEON : C_TEXT);
        if (GuiButton((Rectangle){ box.x + box.width - 38, py, 24, 22 }, "x")) {
            for (int k = i; k + 1 < st->n_apps; k++) st->apps[k] = st->apps[k + 1];
            st->n_apps--;
            U.app_active_rule = -1;
            break;
        }
        py += 26;
    }
    if (st->n_apps == 0) { text(F.ui, tr(S_APPS_NONE), box.x + 14, py, C_DISABLED); py += 26; }
    py = box.y + ph - 98;
    /* program picker: search box, then < name > over the matching running programs (sorted) */
    static int hits[256];
    const int nh = vf_appwatch_filter(U.procs, U.n_procs, U.proc_find, hits, 256);
    {
        Rectangle sr = { box.x + 14, py - 32, box.width - 28, 26 };
        if (GuiTextBox(sr, U.proc_find, (int)sizeof U.proc_find - 1, U.proc_find_on)) U.proc_find_on = !U.proc_find_on;
        if (!U.proc_find[0] && !U.proc_find_on) text(F.ui, tr(S_APPS_SEARCH), sr.x + 8, sr.y + 5, C_DISABLED);
    }
    if (nh == 0) { text(F.ui, tr(S_APPS_NONE), box.x + 14, py + 4, C_DISABLED); U.proc_sel = 0; }
    else {
        /* keep the pick when the list refreshes: find it by name, else fall back to the first hit */
        int cur = -1;
        for (int k = 0; k < nh; k++) if (!strcmp(U.procs[hits[k]], U.proc_name)) { cur = k; break; }
        if (cur < 0) { cur = 0; snprintf(U.proc_name, sizeof U.proc_name, "%s", U.procs[hits[0]]); }
        if (GuiButton((Rectangle){ box.x + 14, py, 24, 24 }, "<")) cur = (cur + nh - 1) % nh;
        if (GuiButton((Rectangle){ box.x + box.width - 38, py, 24, 24 }, ">")) cur = (cur + 1) % nh;
        snprintf(U.proc_name, sizeof U.proc_name, "%s", U.procs[hits[cur]]);
        U.proc_sel = hits[cur];
        BeginScissorMode((int)box.x + 44, (int)py, (int)box.width - 88, 24);      /* long names must not run under the buttons */
        text(F.lcd, U.proc_name, box.x + 46, py + 5, C_TEXT);
        EndScissorMode();
    }
    py += 30;
    if (n_presets > 0) {
        if (U.preset_sel >= n_presets) U.preset_sel = 0;
        if (GuiButton((Rectangle){ box.x + 14, py, 24, 24 }, "<")) U.preset_sel = (U.preset_sel + n_presets - 1) % n_presets;
        if (GuiButton((Rectangle){ box.x + box.width - 38, py, 24, 24 }, ">")) U.preset_sel = (U.preset_sel + 1) % n_presets;
        text(F.lcd, vf_preset_display_name(&presets[U.preset_sel]), box.x + 46, py + 5, C_NEON);
    }
    py += 32;
    if (GuiButton((Rectangle){ box.x + 14, py, box.width - 28, 26 }, tr(S_APPS_ADD)) && U.n_procs > 0 && n_presets > 0 && st->n_apps < VF_MAX_APP_RULES) {
        VfAppRule *r = &st->apps[st->n_apps++];
        snprintf(r->exe, sizeof r->exe, "%s", U.procs[U.proc_sel]);
        snprintf(r->preset, sizeof r->preset, "%s", presets[U.preset_sel].name);
    }

    /* --- 5: share presets --- */
    box = (Rectangle){ area.x + (pw + 6), area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_SHARE), -1);
    py = box.y + 40;
    static char buf[8192];
    if (GuiButton((Rectangle){ box.x + 14, py, box.width - 28, 28 }, tr(S_EXPORT_CLIP))) {
        if (vf_preset_export_text(&s->pipe, st->preset, buf, sizeof buf) > 0) { SetClipboardText(buf); note_set("%s", tr(S_N_COPIED)); }
    }
    py += 36;
    if (GuiButton((Rectangle){ box.x + 14, py, box.width - 28, 28 }, tr(S_IMPORT_CLIP))) {
        const int n = vf_preset_import_text(&s->pipe, GetClipboardText());
        if (n > 0) note_set(tr(S_N_IMPORTED_FMT), n); else note_set("%s", tr(S_N_IMPORT_FAIL));
    }
    py += 36;
    if (GuiButton((Rectangle){ box.x + 14, py, box.width - 28, 28 }, tr(S_EXPORT_FILE))) {
        char dir[VF_PATH_MAX - 128], safe[64], path[VF_PATH_MAX];
        vf_documents_dir(dir, sizeof dir);
        if (vf_preset_sanitize(st->preset, safe, sizeof safe) != 0) snprintf(safe, sizeof safe, "preset");
        snprintf(path, sizeof path, "%s/%s.ini", dir, safe);
        FILE *f = vf_fopen(path, "wb");
        if (f && vf_preset_export_text(&s->pipe, st->preset, buf, sizeof buf) > 0) { fputs(buf, f); note_set(tr(S_N_EXPORTED_FMT), path); }
        else note_set("%s", tr(S_N_REC_FAIL));
        if (f) fclose(f);
    }
    py += 40;
    text(F.ui, tr(S_IMPORT_DROP), box.x + 14, py, C_DISABLED);

    /* --- 6: about --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y + (ph + 6), pw, ph };
    panel(box, "mic thingy", -1);
    py = box.y + 40;
    char cfg[VF_PATH_MAX - 64];
    vf_config_dir(cfg, sizeof cfg);
    text(F.ui, "MIT license - open source", box.x + 14, py, C_TEXT);
    py += 26;
    text(F.ui, "RNNoise (BSD-3), miniaudio, raylib, raygui", box.x + 14, py, C_TEXT_DIM);
    py += 26;
    text(F.lcd, "settings:", box.x + 14, py, C_TEXT_DIM);
    py += 20;
    text_fit(F.lcd, cfg, box.x + 14, py, 12, box.width - 28, C_NEON);
    (void)sel_preset;
}

/* ---- compact window ---- */

static void draw_compact(VfSession *s, VfSettings *st, const Meters *m_)
{
    const int W = COMPACT_W;
    (void)st;
    DrawRectangleGradientV(0, 0, W, COMPACT_H, C_BG_TOP, C_BG_BOTTOM);
    DrawRectangleGradientV(0, 0, W, 26, header_top(), C_SLATE_B);
    DrawRectangle(0, 24, W, 2, C_NEON);
    text(F.bold, "MIC THINGY", 8, 4, on_dark(C_NEON));
    const int voice = s->running && m_->vad > 0.5f;
    DrawCircle(W - 52, 13, 5, voice ? C_NEON : C_SEG_OFF);
    if (voice) DrawCircle(W - 52, 13, 9, Fade(C_NEON, 0.2f));
    if (GuiButton((Rectangle){ W - 34, 3, 28, 20 }, "[]")) apply_window_mode(0);
    text(F.ui, tr(S_INPUT), 8, 33, C_TEXT_DIM);
    led_meter((Rectangle){ 62, 32, W - 72, 16 }, m_->peak_in, m_->hold_in);
    text(F.ui, tr(S_OUTPUT_LEVEL), 8, 55, C_TEXT_DIM);
    led_meter((Rectangle){ 62, 54, W - 72, 16 }, m_->peak_out, m_->hold_out);
    const int muted = vf_automute_effective(&U.am) || U.ptt_block;
    GuiSetStyle(DEFAULT, TEXT_SIZE, 18);
    if (onoff((Rectangle){ 8, 84, 150, 54 }, !muted, tr(S_MIC_ON), tr(S_MIC_MUTED)) != !muted) vf_automute_toggle(&U.am, vf_now_s());
    GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
    const bool filt = !atomic_load(&s->pipe.bypass_all);
    if (onoff((Rectangle){ 166, 84, 126, 54 }, filt, tr(S_FILTER_ON), tr(S_FILTER_OFF)) != filt) atomic_store(&s->pipe.bypass_all, filt ? 1 : 0);
}

/* ---- per-frame logic ---- */

typedef struct UiLink {
    int *sel_in, *sel_preset, *shown_preset;
    VfPresetInfo *presets;
    int n_presets;
    float vad;
    int typing;
} UiLink;

static void apply_app_preset(UiLink *L, const char *name, const char *exe)
{
    for (int i = 0; i < L->n_presets; i++)
        if (!strcmp(L->presets[i].name, name)) {
            if (U.app_active_rule < 0) snprintf(U.app_prev_preset, sizeof U.app_prev_preset, "%s", g_st->preset);
            *L->sel_preset = i;
            *L->shown_preset = -1;                    /* forces the normal preset loader to apply it */
            if (exe) note_set(tr(S_N_APP_FMT), exe, name);
            return;
        }
}

static void ui_tick(VfSession *s, double now, int running_window, UiLink *L)
{
    VfSettings *st = g_st;
    const int voice = L->vad > 0.5f;

    /* hotkeys */
    if (U.hk_capture >= 0) {
        if (U.hk_wait_release) { if (!vf_hotkey_scan()) U.hk_wait_release = 0; }
        else {
            static const int mods[] = { 0x11, 0x12, 0x10, 0x5B };
            const int key = vf_hotkey_scan();
            if (key == 0x1B) { U.hk_capture = -1; U.hk_wait_release = 1; }
            else if (key) {
                int vks[VF_HK_MAX], n = 0;
                for (int i = 0; i < 4; i++) { VfHotkey one; one.n = 1; one.vk[0] = mods[i]; if (vf_hotkey_down(&one) && n < VF_HK_MAX - 1) vks[n++] = mods[i]; }
                vks[n++] = key;
                VfHotkey hk;
                vf_hotkey_from_text_keys(vks, n, &hk);
                vf_hotkey_format(&hk, hk_text(U.hk_capture), 48);
                sync_hotkeys();
                U.hk_capture = -1;
                U.hk_wait_release = 1;
            }
        }
    } else {
        if (vf_hotkey_pressed(&U.hk[0], &U.hk_was[0])) {
            const int m = vf_automute_toggle(&U.am, now);
            note_set("%s", tr(m ? S_N_MUTED : S_N_UNMUTED));
        }
        if (vf_hotkey_pressed(&U.hk[2], &U.hk_was[2])) atomic_store(&s->pipe.bypass_all, !atomic_load(&s->pipe.bypass_all));
        U.hk_was[1] = 0;
        for (int i = 0; i < VF_BOARD_SLOTS; i++)
            if (vf_hotkey_pressed(&U.hk[3 + i], &U.hk_was[3 + i])) vf_board_play(&s->board, i);
    }
    U.ptt_block = st->ptt_enabled && U.hk[1].n > 0 && !vf_hotkey_down(&U.hk[1]);

    /* automatic mute */
    if (now >= U.next_lock) { U.locked = vf_screen_locked(); U.next_lock = now + 1.0; }
    U.am.on_lock = st->auto_mute_lock;
    U.am.idle_minutes = st->auto_mute_idle_min;
    const int was_idle = U.am.by_idle;
    vf_automute_update(&U.am, now, U.locked, voice && s->running);
    if (U.am.by_idle && !was_idle) note_set("%s", tr(S_N_IDLE_MUTED));
    const int mute = vf_automute_effective(&U.am) || U.ptt_block;
    pipeline_set_mute(&s->pipe, mute);
    vf_tray_set_mute(mute);
    if (st->overlay) {
        if (!vf_overlay_running()) vf_overlay_start(st->overlay_corner);
        vf_overlay_set_corner(st->overlay_corner);
        vf_overlay_set_state(!s->running ? VF_OVL_IDLE : mute ? VF_OVL_MUTED : voice ? VF_OVL_SPEAKING : VF_OVL_IDLE);
    } else if (vf_overlay_running()) vf_overlay_stop();

    /* mix bus levels (atomics, applied by the audio thread) */
    atomic_store(&s->mix_board.gain, db_to_lin_f(st->board_gain_db));
    atomic_store(&s->mix_sys.gain, db_to_lin_f(st->sys_gain_db));
    atomic_store(&s->mix_sys.duck, db_to_lin_f(st->sys_duck_db));
    const int aec_on = !atomic_load(&s->chain.aec.bypass);
    const int want_sys = st->sys_enabled || aec_on;               /* the echo canceller needs the loopback too */
    if (!st->sys_enabled) atomic_store(&s->mix_sys.gain, 0.0f);
    if (want_sys && !U.sys_running && now >= U.sys_retry) {
        int idx = -1;
        for (int i = 0; i < s->plays.count; i++) if (st->sys_device[0] && !strcmp(s->plays.dev[i].name, st->sys_device)) idx = i;
        U.sys_running = vf_session_sys_start(s, idx) == 0;
        if (!U.sys_running) U.sys_retry = now + 5.0;
    } else if (!want_sys && U.sys_running) { vf_session_sys_stop(s); U.sys_running = 0; }

    /* once a second: statistics */
    if (now >= U.next_second) {
        U.next_second = now + 1.0;
        vf_stats_tick(&U.stats, vf_session_xruns(s));
    }

    /* A/B test and recorder housekeeping */
    vf_ab_update(&U.ab, s->running ? &s->eng : NULL);
    if (!s->running && U.ab.state == VF_AB_RECORDING) vf_ab_free(&U.ab, NULL);
    if (vf_rec_active(&U.rec) && !s->running) { vf_rec_stop(&U.rec); note_set(tr(S_N_REC_SAVED_FMT), U.rec_path); }
    if (U.rec_was_active && !vf_rec_active(&U.rec) && U.rec.thread.started) {      /* hit the size limit or a disk error */
        const int ok = vf_rec_stop(&U.rec) == 0;
        if (ok) note_set(tr(S_N_REC_SAVED_FMT), U.rec_path); else note_set("%s", tr(S_N_REC_FAIL));
    }
    U.rec_was_active = vf_rec_active(&U.rec);

    /* spectrum feed only while it is visible */
    const int howl_on = st->howl_guard && s->running && s->has_mon && U.scope_ready;
    const int want_scope = ((running_window && !U.compact && U.tab == 3) || howl_on) && U.scope_ready;
    if (want_scope) {
        if (atomic_load(&s->pipe.scope_ring) != &U.scope) atomic_store(&s->pipe.scope_ring, &U.scope);
        float blk[2 * VF_FRAME_SIZE];
        int got = 0;
        while (vf_ring_available(&U.scope) >= 2 * VF_FRAME_SIZE && got < 6) {
            vf_ring_read(&U.scope, blk, 2 * VF_FRAME_SIZE);
            vf_spec_push(&U.spec, blk, blk + VF_FRAME_SIZE, VF_FRAME_SIZE);
            got++;
        }
        vf_spec_update(&U.spec, 0.85f);
        if (howl_on) {
            const float hz = vf_howl_update(&U.howl, st->monitor_raw ? U.spec.in_db : U.spec.out_db);
            if (hz > 0.0f) {
                st->monitor_gain_db = fmaxf(-40.0f, st->monitor_gain_db - 12.0f);
                vf_session_monitor_set(s, st->monitor_gain_db, st->monitor_raw);
                note_set(tr(S_N_HOWL_FMT), (double)hz);
            }
        }
    } else if (atomic_load(&s->pipe.scope_ring) != NULL) {
        atomic_store(&s->pipe.scope_ring, NULL);
        vf_ring_read(&U.scope, NULL, vf_ring_available(&U.scope));
    }

    /* undo history: commit when an edit has settled and no slider is being dragged */
    VfSnap now_snap;
    vf_snap_capture(&s->pipe, &now_snap);
    if (U.comparing) { U.seen = now_snap; U.undo_pending = 0; }                   /* hold-to-compare is not an edit */
    else if (!vf_snap_equal(&now_snap, &U.seen)) { U.seen = now_snap; U.undo_pending = 1; U.change_t = now; }
    if (U.undo_pending && now - U.change_t > 0.4 && !(running_window && IsMouseButtonDown(MOUSE_BUTTON_LEFT))) {
        vf_undo_commit(&U.undo, &s->pipe);
        U.undo_pending = 0;
    }
    if (running_window && !L->typing && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
        const int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (IsKeyPressed(KEY_Z) && !shift) { vf_undo_commit(&U.undo, &s->pipe); vf_undo_undo(&U.undo, &s->pipe); vf_snap_capture(&s->pipe, &U.seen); U.undo_pending = 0; }
        else if (IsKeyPressed(KEY_Y) || (IsKeyPressed(KEY_Z) && shift)) { vf_undo_redo(&U.undo, &s->pipe); vf_snap_capture(&s->pipe, &U.seen); U.undo_pending = 0; }
    }

    /* presets per program */
    if (now >= U.next_apps) {
        U.next_apps = now + 3.0;
        if (st->apps_enabled || running_window) U.n_procs = vf_appwatch_list(U.procs, 256);
        if (st->apps_enabled && st->n_apps > 0) {
            const int r = vf_appwatch_match(st->apps, st->n_apps, U.procs, U.n_procs);
            if (r != U.app_active_rule) {
                if (r >= 0) { apply_app_preset(L, st->apps[r].preset, st->apps[r].exe); U.app_active_rule = r; }
                else if (U.app_active_rule >= 0) {
                    U.app_active_rule = -1;
                    apply_app_preset(L, U.app_prev_preset, NULL);
                    note_set("%s", tr(S_N_APP_BACK));
                }
            }
        } else if (U.app_active_rule >= 0) U.app_active_rule = -1;
    }

    /* per-microphone settings: save the old microphone's profile, load the new one's */
    if (L->sel_in && *L->sel_in != U.prev_sel_in) {
        if (st->per_mic && U.prev_sel_in >= 0 && U.prev_sel_in < s->caps.count && *L->sel_in < s->caps.count && *L->sel_in >= 0) {
            vf_profile_save(&s->pipe, s->caps.dev[U.prev_sel_in].name);
            if (vf_profile_load(&s->pipe, s->caps.dev[*L->sel_in].name) == 0) note_set(tr(S_N_PROFILE_FMT), s->caps.dev[*L->sel_in].name);
        }
        U.prev_sel_in = *L->sel_in;
    }
}

/* Drag and drop: presets (.ini) and sounds. */
static void ui_handle_drops(VfSession *s, VfSettings *st)
{
    if (!IsFileDropped()) return;
    FilePathList files = LoadDroppedFiles();
    for (unsigned i = 0; i < files.count; i++) {
        const char *p = files.paths[i];
        if (IsFileExtension(p, ".ini")) {
            char *data = LoadFileText(p);
            const int k = data ? vf_preset_import_text(&s->pipe, data) : -1;
            if (data) UnloadFileText(data);
            if (k > 0) note_set(tr(S_N_IMPORTED_FMT), k); else note_set("%s", tr(S_N_IMPORT_FAIL));
        } else {
            int slot = -1;
            for (int j = 0; j < VF_BOARD_SLOTS; j++) if (!s->board.clip[j].pcm) { slot = j; break; }
            if (slot < 0) { note_set("%s", tr(S_BOARD_FULL)); continue; }
            const int r = vf_board_load(&s->board, slot, p);
            if (r == 0) snprintf(st->board_path[slot], sizeof st->board_path[slot], "%s", p);
            else note_set(r == -2 ? "%s" : r == -3 ? "%s" : tr(S_BOARD_FAIL_FMT), r == -2 ? tr(S_BOARD_LONG) : r == -3 ? tr(S_BOARD_MEM) : p);
        }
    }
    UnloadDroppedFiles(files);
}
