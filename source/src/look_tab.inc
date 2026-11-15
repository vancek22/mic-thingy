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


static const char *panel_title(const char *id);

static Color col_of(unsigned rgb) { return (Color){ (unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb, 255 }; }

/* a clickable colour chip; returns 1 when clicked */
static int chip(Rectangle c, unsigned rgb, int selected)
{
    DrawRectangleRec(c, col_of(rgb));
    DrawRectangleLinesEx(c, selected ? 2 : 1, selected ? C_TITLE : C_EDGE);
    if (selected) DrawRectangleLinesEx((Rectangle){ c.x - 3, c.y - 3, c.width + 6, c.height + 6 }, 1, C_NEON);
    return !GuiIsLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), c);
}

static void after_colour_edit(VfSettings *st)
{
    st->look.preset[0] = '\0';                                  /* no longer exactly a preset */
    retheme();
}

static void draw_look(Rectangle area, VfSession *s, VfSettings *st)
{
    static const unsigned k_accents[8] = { 0x39ff14, 0x00e5ff, 0x508cff, 0xb266ff, 0xff40c8, 0xffb7d5, 0x5cffb0, 0xdcf0ff };
    static const unsigned k_mains[8] = { 0x395cbf, 0x17338f, 0x1d5f73, 0x2f6b3a, 0x4a2a80, 0x6b1f2e, 0x6b4a2a, 0x808080 };
    const float pw = 292, ph = 276;
    VfLook *lk = &st->look;
    Rectangle box;
    float py;
    char t[200];

    /* --- 1: ready-made styles, or (advanced) every single colour --- */
    box = (Rectangle){ area.x, area.y, pw, ph };
    if (!U.look_adv) {
        panel(box, tr(S_PN_LK_STYLES), -1);
        const int lang_pl = vf_i18n_get() == VF_LANG_PL;
        for (int i = 0; i < vf_look_preset_count() && i < 12; i++) {
            const VfLookPreset *p = vf_look_preset(i);
            Rectangle b = { box.x + 14 + (float)(i % 2) * 134, box.y + 36 + (float)(i / 2) * 32, 128, 28 };
            const int cur = !strcmp(lk->preset, p->id);
            const int hot = !GuiIsLocked() && CheckCollisionPointRec(GetMousePosition(), b);
            DrawRectangleRec(b, hot ? mixc(C_PANEL, C_BEVEL_HI, 0.3f) : mixc(C_PANEL, C_BEVEL_HI, 0.12f));
            DrawRectangleLinesEx(b, cur ? 2 : 1, cur ? C_NEON : mixc(C_PANEL, C_BEVEL_HI, 0.6f));
            /* preview: main colour block with the accent as a bar */
            unsigned pv;                                            /* the title-strip colour this style gives */
            if (p->theme) {
                const VfRgba sa = vf_theme_get(vf_theme_find(p->theme))->c[TC_SLATE_A];
                pv = (unsigned)sa.r << 16 | (unsigned)sa.g << 8 | sa.b;
            } else {
                float h, sat, v;
                vf_rgb_hsv(p->main, &h, &sat, &v);
                pv = vf_hsv_rgb(h, sat * 0.69f, fminf(0.75f, 0.361f * p->bright));
            }
            DrawRectangle((int)b.x + 4, (int)b.y + 4, 20, 20, col_of(pv));
            DrawRectangle((int)b.x + 4, (int)b.y + 18, 20, 6, col_of(p->accent));
            text_fit_head(F.ui, lang_pl ? p->name_pl : p->name_en, b.x + 30, b.y + 6, 15, b.width - 34, cur ? C_NEON : C_TEXT);
            if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                vf_look_use_preset(lk, st->theme, sizeof st->theme, i);
                retheme();
            }
        }
        if (GuiButton((Rectangle){ box.x + 14, box.y + ph - 40, box.width - 28, 26 }, tr(S_LK_ADVANCED))) U.look_adv = 1;
    } else {
        panel(box, tr(S_LK_COLORS), -1);
        py = box.y + 34;
        for (int i = 0; i < TC_COUNT; i++) {
            Rectangle c = { box.x + 14 + (float)(i % 5) * 52, py + (float)(i / 5) * 24, 50, 22 };
            DrawRectangleRec(c, g_c[i]);
            DrawRectangleLinesEx(c, i == U.look_sel ? 2 : 1, i == U.look_sel ? C_TITLE : C_EDGE);
            if (!GuiIsLocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), c)) U.look_sel = i;
        }
        py += 100;
        VfRgba base[TC_COUNT];
        vf_look_colors(lk, st->theme, base, 0);
        const int sel = U.look_sel;
        char hex[8];
        vf_rgb_format((unsigned)base[sel].r << 16 | (unsigned)base[sel].g << 8 | base[sel].b, hex);
        text(F.ui, tr((VfStr)(S_TCOL_0 + sel)), box.x + 14, py - 2, C_TEXT);
        text(F.lcd, hex, box.x + box.width - 14 - MeasureTextEx(F.lcd, hex, 14, 0).x, py, C_NEON);
        py += 24;
        float rgb[3] = { (float)base[sel].r, (float)base[sel].g, (float)base[sel].b };
        int ch = 0;
        ch |= rgb_row(box, py, "R", &rgb[0]);
        ch |= rgb_row(box, py + 24, "G", &rgb[1]);
        ch |= rgb_row(box, py + 48, "B", &rgb[2]);
        if (ch) {
            ensure_custom(st);
            lk->custom[sel] = (unsigned)(rgb[0] + 0.5f) << 16 | (unsigned)(rgb[1] + 0.5f) << 8 | (unsigned)(rgb[2] + 0.5f);
            after_colour_edit(st);
        }
        if (GuiButton((Rectangle){ box.x + 14, box.y + ph - 36, box.width - 28, 24 }, tr(S_LK_BACK))) U.look_adv = 0;
    }

    /* --- 2: main colour and accent --- */
    box = (Rectangle){ area.x + (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_LK_MAIN), -1);
    py = box.y + 34;
    text(F.ui, tr(S_LK_MAIN), box.x + 14, py, C_TEXT);
    py += 22;
    for (int i = 0; i < 8; i++)
        if (chip((Rectangle){ box.x + 14 + (float)i * 33, py, 30, 22 }, k_mains[i], lk->main_rgb == k_mains[i] && !strcmp(st->theme, "custom"))) {
            ensure_custom(st);
            vf_look_set_main(lk, k_mains[i], lk->main_bright);
            after_colour_edit(st);
        }
    py += 32;
    float mh, ms, mv;
    vf_rgb_hsv(lk->main_rgb, &mh, &ms, &mv);
    float hue = mh, sat = ms * 100.0f, br = lk->main_bright * 100.0f;
    int mch = 0;
    mch |= slider_row(box, &py, tr(S_LK_MAIN_HUE), &hue, 0.0f, 359.0f, "%.0f", 1);
    mch |= slider_row(box, &py, tr(S_LK_MAIN_SAT), &sat, 0.0f, 100.0f, "%.0f %%", 1);
    mch |= slider_row(box, &py, tr(S_LK_MAIN_BRIGHT), &br, 60.0f, 180.0f, "%.0f %%", 1);
    if (mch) {
        ensure_custom(st);
        vf_look_set_main(lk, vf_hsv_rgb(hue, sat / 100.0f, 0.75f), br / 100.0f);
        after_colour_edit(st);
    }
    text(F.ui, tr(S_LK_ACCENT), box.x + 14, py - 2, C_TEXT);
    py += 20;
    for (int i = 0; i < 8; i++) {
        VfRgba cur[TC_COUNT];
        vf_look_colors(lk, st->theme, cur, 0);
        const unsigned ca = (unsigned)cur[TC_ACCENT].r << 16 | (unsigned)cur[TC_ACCENT].g << 8 | cur[TC_ACCENT].b;
        if (chip((Rectangle){ box.x + 14 + (float)i * 33, py, 30, 22 }, k_accents[i], ca == k_accents[i])) {
            ensure_custom(st);
            vf_look_set_accent(lk, k_accents[i]);
            after_colour_edit(st);
        }
    }

    /* --- 3: window and sharing --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y, pw, ph };
    panel(box, tr(S_PN_LK_WINDOW), -1);
    py = box.y + 34;
    float op = (float)lk->opacity;
    if (slider_row(box, &py, tr(S_LK_OPACITY), &op, 40.0f, 100.0f, "%.0f %%", 1)) { lk->opacity = (int)(op + 0.5f); apply_window_look(); }
    int tm = lk->topmost;
    if (check_row(box, &py, tr(S_LK_TOPMOST), &tm, 1)) { lk->topmost = tm; apply_window_look(); }
    nth_token(tr(S_TABS_ALL), lk->start_tab, t, sizeof t);
    const int d = stepper(box, &py, tr(S_LK_STARTTAB), t);
    if (d) lk->start_tab = (lk->start_tab + d + VF_LOOK_TABS) % VF_LOOK_TABS;
    if (GuiButton((Rectangle){ box.x + 14, box.y + ph - 106, box.width - 28, 26 }, tr(S_LK_LAYOUT_RESET))) lk->layout[0] = '\0';
    py = box.y + ph - 72;
    if (GuiButton((Rectangle){ box.x + 14, py, 128, 26 }, tr(S_LK_COPY))) {
        char buf[4096];
        if (vf_look_to_text(lk, st->theme, buf, sizeof buf) > 0) { SetClipboardText(buf); note_set("%s", tr(S_LK_COPIED)); }
    }
    if (GuiButton((Rectangle){ box.x + 150, py, 128, 26 }, tr(S_LK_PASTE))) {
        const char *clip = GetClipboardText();
        const int n = clip ? vf_look_from_text(lk, st->theme, sizeof st->theme, clip) : 0;
        if (n > 0) { retheme(); reload_fonts(); apply_window_look(); note_set(tr(S_LK_PASTED_FMT), n); }
        else note_set("%s", tr(S_LK_PASTE_FAIL));
    }
    if (GuiButton((Rectangle){ box.x + 14, py + 32, box.width - 28, 26 }, tr(S_LK_RESET))) {
        vf_look_defaults(lk);
        snprintf(st->theme, sizeof st->theme, "graphite");
        retheme(); reload_fonts(); apply_window_look();
    }

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

    /* --- 6: which effect panels are shown (hidden ones keep running), their order --- */
    box = (Rectangle){ area.x + 2 * (pw + 6), area.y + (ph + 6), pw, ph };
    panel(box, tr(S_PN_LK_PANELS), -1);
    py = box.y + 32;
    {   /* effects (old Basic, Extra, Effects order), then the tools */
        const char *ids[VF_MAX_FX + VF_TOOL_COUNT];
        int nid = 0;
        for (int pg_ = 0; pg_ < VF_LOOK_PAGES; pg_++)
            for (int q = 0; q < s->pipe.count; q++)
                if (s->pipe.fx[q]->page == pg_ || (pg_ == VF_LOOK_PAGES - 1 && s->pipe.fx[q]->page >= VF_LOOK_PAGES)) ids[nid++] = s->pipe.fx[q]->name;
        for (int k = 0; k < VF_TOOL_COUNT; k++) ids[nid++] = k_tool_ids[k];
        for (int i = 0; i < nid; i++) {
            const float cx = box.x + 14 + (float)(i % 2) * 138, cy = py + (float)(i / 2) * 19;
            bool vis = !vf_look_hidden(lk, ids[i]);
            const bool was = vis;
            GuiCheckBox((Rectangle){ cx, cy, 14, 14 }, "", &vis);
            text_fit_head(F.ui, panel_title(ids[i]), cx + 20, cy - 1, 13, 114, vis ? C_TEXT : C_DISABLED);
            if (vis != was) vf_look_set_hidden(lk, ids[i], !vis);
        }
    }
    text_fit_head(F.ui, tr(S_LK_DRAG_HINT), box.x + 14, box.y + ph - 36, 14, box.width - 28, C_DISABLED);
}

/* title of a panel id: an effect name or a tool id (t_*) */
static const char *panel_title(const char *id)
{
    static const VfStr tool_titles[VF_TOOL_COUNT] = { S_PN_MIX, S_PN_TEST, S_PN_CAL, S_PN_BOARD, S_PN_QUALITY, S_PN_SPECTRUM };
    for (int k = 0; k < VF_TOOL_COUNT; k++)
        if (!strcmp(id, k_tool_ids[k])) return tr(tool_titles[k]);
    return tr_fx(id);
}

/* A panel carried by its title strip (main_gui.c starts the drag): drop it on another place of the grid. The view
 * scrolls when the panel is carried near its top or bottom edge. Only the drawing order changes, never the audio chain. */
static void drag_panel(VfSettings *st, Vector2 mouse, Rectangle view, const char *const *names, const int *defp, int n,
                       const int *vis_all, int nvis_all)
{
    const float pw = 292, ph = 276;
    const int id = U.drag_fx;
    if (fabsf(mouse.x - U.drag_sx) + fabsf(mouse.y - U.drag_sy) > 6) U.drag_moved = 1;
    int vis[64], nv = 0;                                   /* visible panels without the carried one */
    for (int k = 0; k < nvis_all && nv < 64; k++)
        if (vis_all[k] != id) vis[nv++] = vis_all[k];
    int target = -1;
    if (CheckCollisionPointRec(mouse, view)) {
        const int col = (int)((mouse.x - view.x) / (pw + 6)), row = (int)((mouse.y - view.y + U.scroll) / (ph + 6));
        if (col >= 0 && col < 3 && row >= 0) target = row * 3 + col;
        if (target > nv) target = nv;
    }
    if (U.drag_moved) {
        if (target >= 0) {
            BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
            DrawRectangleLinesEx((Rectangle){ view.x + (float)(target % 3) * (pw + 6), view.y + (float)(target / 3) * (ph + 6) - U.scroll, pw, ph }, 3, C_NEON);
            EndScissorMode();
        }
        Rectangle g = { mouse.x - U.drag_dx, mouse.y - U.drag_dy, pw, 28 };    /* the carried title strip */
        DrawRectangleGradientH((int)g.x, (int)g.y, (int)g.width, (int)g.height, C_SLATE_A, C_SLATE_B);
        DrawRectangleLinesEx(g, 2, C_NEON);
        text(F.bold, panel_title(names[id]), g.x + 10, g.y + 7, C_TITLE);
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {                                 /* dropped */
        if (U.drag_moved && target >= 0) vf_look_move(&st->look, names, defp, n, id, 0, target < nv ? vis[target] : -1);
        U.drag_fx = -1;
    }
}
