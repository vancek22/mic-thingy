/* ---- look tab: colours, accent, window, meters, fonts, panels (included by gui_tools.inc) ---- */

/* One line of at most maxw pixels; a too long text is cut at the end ("Voice effe..."). UTF-8 safe. */
static void text_fit_head(Font f, const char *s, float x, float y, float size, float maxw, Color c)
{
    if (MeasureTextEx(f, s, size, 0).x <= maxw) { DrawTextEx(f, s, (Vector2){ x, y }, size, 0, c); return; }
    char buf[96];
    size_t n = strlen(s);
    if (n > sizeof buf - 4) n = sizeof buf - 4;
    for (; n > 0; n--) {
        if (((unsigned char)s[n] & 0xC0) == 0x80) continue;          /* not in the middle of a UTF-8 sequence */
        snprintf(buf, sizeof buf, "%.*s..", (int)n, s);
        if (MeasureTextEx(f, buf, size, 0).x <= maxw) break;
    }
    DrawTextEx(f, buf, (Vector2){ x, y }, size, 0, c);
}

static int theme_index(const VfSettings *st) { return !strcmp(st->theme, "custom") ? vf_theme_count() : vf_theme_find(st->theme); }

static void theme_select(VfSettings *st, int th)
{
    if (th >= vf_theme_count()) snprintf(st->theme, sizeof st->theme, "custom");
    else snprintf(st->theme, sizeof st->theme, "%s", vf_theme_get(th)->id);
    retheme();
}

/* editing a colour starts from what is on screen: the current theme becomes the custom theme first */
static void ensure_custom(VfSettings *st)
{
    if (strcmp(st->theme, "custom")) { vf_look_make_custom(&st->look, st->theme); snprintf(st->theme, sizeof st->theme, "custom"); }
}

static void set_accent(VfSettings *st, unsigned rgb)
{
    ensure_custom(st);
    st->look.custom[TC_ACCENT] = rgb;
    st->look.custom[TC_ON_BTN] = ((rgb >> 16 & 255) * 6 / 10) << 16 | ((rgb >> 8 & 255) * 6 / 10) << 8 | ((rgb & 255) * 6 / 10);
    retheme();
}

static void nth_token(const char *list, int n, char *out, size_t cap)
{
    const char *p = list;
    for (; n > 0 && p; n--) { p = strchr(p, ';'); if (p) p++; }
    if (!p) { out[0] = '\0'; return; }
    const char *e = strchr(p, ';');
    snprintf(out, cap, "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
}

/* label, then a "<" value ">" row; returns -1 / +1 when a button was pressed, else 0 */
static int stepper(Rectangle box, float *py, const char *label, const char *value)
{
    text(F.ui, label, box.x + 14, *py, C_TEXT);
    *py += 22;
    int r = 0;
    if (GuiButton((Rectangle){ box.x + 14, *py, 28, 24 }, "<")) r = -1;
    Rectangle lr = { box.x + 46, *py, box.width - 92, 24 };
    lcd(lr);
    text_fit(F.lcd, value, lr.x + 6, lr.y + 5, 14, lr.width - 12, C_NEON);
    if (GuiButton((Rectangle){ box.x + box.width - 42, *py, 28, 24 }, ">")) r = 1;
    *py += 32;
    return r;
}

static int rgb_row(Rectangle box, float y, const char *lbl, float *v)
{
    char val[8];
    snprintf(val, sizeof val, "%.0f", (double)*v);
    text(F.lcd, lbl, box.x + 14, y - 2, C_TEXT_DIM);
    float nv = *v;
    GuiSliderBar((Rectangle){ box.x + 30, y, box.width - 30 - 52, 12 }, NULL, NULL, &nv, 0, 255);
    const float vw = MeasureTextEx(F.lcd, val, 14, 0).x;
    text(F.lcd, val, box.x + box.width - 14 - vw, y - 2, C_NEON);
    if ((int)(nv + 0.5f) != (int)(*v + 0.5f)) { *v = nv; return 1; }
    return 0;
}

static void draw_look(Rectangle area, VfSession *s, VfSettings *st)
{
    static const unsigned k_accents[8] = { 0x39ff14, 0x00e5ff, 0x508cff, 0xb266ff, 0xff40c8, 0xff9a1e, 0xffd23c, 0xdcf0ff };
    const float pw = 292, ph = 276;
    VfLook *lk = &st->look;
    Rectangle box;
    float py;
    char t[200];

    /* --- 1: colours --- */
    box = (Rectangle){ area.x, area.y, pw, ph };
    panel(box, tr(S_LK_COLORS), -1);
    py = box.y + 34;
    int th = theme_index(st);
    const int th0 = th;
    GuiToggleGroup((Rectangle){ box.x + 14, py, 64, 24 }, tr(S_THEMES), &th);
    if (th != th0) theme_select(st, th);
    py += 32;
    for (int i = 0; i < TC_COUNT; i++) {
        Rectangle c = { box.x + 14 + (float)(i % 5) * 52, py + (float)(i / 5) * 24, 50, 22 };
        DrawRectangleRec(c, g_c[i]);
        DrawRectangleLinesEx(c, i == U.look_sel ? 2 : 1, i == U.look_sel ? C_TITLE : C_EDGE);
        if (!GuiIsLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), c)) U.look_sel = i;
    }
    py += 100;
    VfRgba base[TC_COUNT];
    vf_look_colors(lk, st->theme, base, 0);                      /* edited without the hue shift */
    const int sel = U.look_sel;
    char hex[8];
    vf_rgb_format((unsigned)base[sel].r << 16 | (unsigned)base[sel].g << 8 | base[sel].b, hex);
    text(F.ui, tr((VfStr)(S_TCOL_0 + sel)), box.x + 14, py - 2, C_TEXT);
    text(F.lcd, hex, box.x + box.width - 14 - MeasureTextEx(F.lcd, hex, 14, 0).x, py, C_NEON);
    py += 24;
    float rgb[3] = { (float)base[sel].r, (float)base[sel].g, (float)base[sel].b };
    int ch = 0;
    ch |= rgb_row(box, py, "R", &rgb[0]);
    ch |= rgb_row(box, py + 26, "G", &rgb[1]);
    ch |= rgb_row(box, py + 52, "B", &rgb[2]);
    if (ch) {
        ensure_custom(st);
        lk->custom[sel] = (unsigned)(rgb[0] + 0.5f) << 16 | (unsigned)(rgb[1] + 0.5f) << 8 | (unsigned)(rgb[2] + 0.5f);
        retheme();
    }

    /* --- 2: accent, hue, share --- */
    box = (Rectangle){ area.x + (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_LK_ACCENT), -1);
    py = box.y + 34;
    text(F.ui, tr(S_LK_ACCENT), box.x + 14, py, C_TEXT);
    py += 24;
    for (int i = 0; i < 8; i++) {
        Rectangle c = { box.x + 14 + (float)(i % 4) * 66, py + (float)(i / 4) * 32, 60, 26 };
        const Color ac = { (unsigned char)(k_accents[i] >> 16), (unsigned char)(k_accents[i] >> 8), (unsigned char)k_accents[i], 255 };
        DrawRectangleRec(c, ac);
        const int cur = lk->custom[TC_ACCENT] == k_accents[i] && !strcmp(st->theme, "custom");
        DrawRectangleLinesEx(c, cur ? 2 : 1, cur ? C_TITLE : C_EDGE);
        if (!GuiIsLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), c)) set_accent(st, k_accents[i]);
    }
    py += 70;
    float hue = (float)lk->hue;
    if (slider_row(box, &py, tr(S_LK_HUE), &hue, -180.0f, 180.0f, "%+.0f", 1)) { lk->hue = (int)(hue + (hue < 0 ? -0.5f : 0.5f)); retheme(); }
    py += 4;
    if (GuiButton((Rectangle){ box.x + 14, py, 124, 26 }, tr(S_LK_COPY))) {
        char buf[2048];
        if (vf_look_to_text(lk, st->theme, buf, sizeof buf) > 0) { SetClipboardText(buf); note_set("%s", tr(S_LK_COPIED)); }
    }
    if (GuiButton((Rectangle){ box.x + 144, py, 134, 26 }, tr(S_LK_PASTE))) {
        const char *clip = GetClipboardText();
        const int n = clip ? vf_look_from_text(lk, st->theme, sizeof st->theme, clip) : 0;
        if (n > 0) { retheme(); reload_fonts(); apply_window_look(); note_set(tr(S_LK_PASTED_FMT), n); }
        else note_set("%s", tr(S_LK_PASTE_FAIL));
    }
    py += 34;
    if (GuiButton((Rectangle){ box.x + 14, py, box.width - 28, 26 }, tr(S_LK_RESET))) {
        vf_look_defaults(lk);
        snprintf(st->theme, sizeof st->theme, "graphite");
        retheme(); reload_fonts(); apply_window_look();
    }

    /* --- 3: window --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_LK_WINDOW), -1);
    py = box.y + 34;
    float op = (float)lk->opacity;
    if (slider_row(box, &py, tr(S_LK_OPACITY), &op, 40.0f, 100.0f, "%.0f %%", 1)) { lk->opacity = (int)(op + 0.5f); apply_window_look(); }
    int tm = lk->topmost;
    if (check_row(box, &py, tr(S_LK_TOPMOST), &tm, 1)) { lk->topmost = tm; apply_window_look(); }
    py += 6;
    nth_token(tr(S_TABS_ALL), lk->start_tab, t, sizeof t);
    const int d = stepper(box, &py, tr(S_LK_STARTTAB), t);
    if (d) lk->start_tab = (lk->start_tab + d + VF_LOOK_TABS) % VF_LOOK_TABS;

    /* --- 4: meters --- */
    box = (Rectangle){ area.x, area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_LK_METERS), -1);
    py = box.y + 34;
    slider_row(box, &py, tr(S_LK_WARN), &lk->meter_warn_db, -40.0f, -6.0f, "%.0f dB", 1);
    slider_row(box, &py, tr(S_LK_CLIP), &lk->meter_clip_db, -20.0f, 0.0f, "%.0f dB", 1);
    if (lk->meter_clip_db <= lk->meter_warn_db) lk->meter_clip_db = fminf(0.0f, lk->meter_warn_db + 3.0f);
    slider_row(box, &py, tr(S_LK_HOLD), &lk->meter_hold_s, 0.0f, 5.0f, "%.1f s", 1);
    slider_row(box, &py, tr(S_LK_FALL), &lk->meter_fall, 5.0f, 90.0f, "%.0f dB/s", 1);
    {   /* live preview: a level sweeping up and down with its peak-hold segment */
        const float tt = (float)GetTime() * 0.9f;
        led_meter((Rectangle){ box.x + 14, py + 4, box.width - 28, 18 }, -60.0f + 60.0f * (0.5f + 0.5f * sinf(tt)),
                  -60.0f + 60.0f * (0.5f + 0.5f * sinf(tt - 0.5f)));
    }

    /* --- 5: fonts and decoration --- */
    box = (Rectangle){ area.x + (pw + 6), area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_LK_FONTS), -1);
    py = box.y + 34;
    {
        const FontFam *fu = font_find(k_fonts_ui, NFONTS(k_fonts_ui), lk->font_ui);
        const int d1 = stepper(box, &py, tr(S_LK_FONT_UI), fu ? fu->name : tr(S_LK_FONT_DEFAULT));
        if (d1) { font_cycle(lk->font_ui, sizeof lk->font_ui, k_fonts_ui, NFONTS(k_fonts_ui), d1); reload_fonts(); }
        const FontFam *fm = font_find(k_fonts_mono, NFONTS(k_fonts_mono), lk->font_lcd);
        const int d2 = stepper(box, &py, tr(S_LK_FONT_LCD), fm ? fm->name : tr(S_LK_FONT_DEFAULT));
        if (d2) { font_cycle(lk->font_lcd, sizeof lk->font_lcd, k_fonts_mono, NFONTS(k_fonts_mono), d2); reload_fonts(); }
    }
    py += 2;
    check_row(box, &py, tr(S_LK_GRID), &lk->grid, 1);
    check_row(box, &py, tr(S_LK_SCAN), &lk->scanlines, 1);
    check_row(box, &py, tr(S_LK_GLOW), &lk->glow, 1);

    /* --- 6: which effect panels are shown (hidden ones keep running) --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_LK_PANELS), -1);
    py = box.y + 36;
    for (int i = 0; i < s->pipe.count; i++) {
        const Fx *fx = s->pipe.fx[i];
        const float cx = box.x + 14 + (float)(i % 2) * 138, cy = py + (float)(i / 2) * 25;
        bool vis = !vf_look_hidden(lk, fx->name);
        const bool was = vis;
        GuiCheckBox((Rectangle){ cx, cy, 16, 16 }, "", &vis);
        text_fit_head(F.ui, tr_fx(fx->name), cx + 22, cy, 14, 112, vis ? C_TEXT : C_DISABLED);
        if (vis != was) vf_look_set_hidden(lk, fx->name, !vis);
    }
    text(F.ui, tr(S_LK_PANELS_NOTE), box.x + 14, box.y + ph - 28, C_DISABLED);
}

