/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE: input, the knobs and their sweetspots, the eight patterns, MIDI in, the screen (a 90s media player's skin),
 * settings. Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in
 * scope. The sound is dsp/engine.c; the factory patterns dsp/patterns.c.
 *
 * Controls
 *   black keys 1-6  the six instruments (KICK SNARE HATS GLITCH PERC BASS): a key picks it (and plays it while the
 *                   sequencer is stopped); SEQ held + a key: muted / back
 *   white keys      the picked instrument's 16 steps on the page shown: a tap turns a step on, again ACCENT, again off
 *   K1..K4          the picked instrument's four knobs (FX: BPM, SWING, DRIVE, SPACE); turned slowly each rests a few
 *                   detents on its sweetspots
 *   SELECT          MASS: FEATHER .. BLACK HOLE (light and nervous .. heavy, slow and dirty)
 *   ALGORITHM       CHAOS: the web's ties (CHAIN, RING, STAR, MESH) and how hard every hit pushes
 *   PRESET          the eight patterns: turn, and it changes at the top of the next loop (stopped: at once)
 *   FX              the master page (again: back).   HOME: the instrument's page
 *   HOME held 1 s   the pattern back to its factory state
 *   SEQ             the length: 16, 32, 48, 64 steps (a new bar starts as a copy of the first)
 *   OCT- / OCT+     the page of steps shown (1-16 .. 49-64)
 *   PLAY            start from step 1 / stop.   REC held 1 s: the picked instrument's steps cleared
 *   MIDI in (USB, TRS): clock, START, CONTINUE, STOP: COLLAPSE follows; notes 36-41 play the six instruments.
 *   Settings save themselves a few seconds after the last change. */
#include "fastmath.h"
#include "engine.h"

#define SET_MAGIC 0x31434F43u            /* "COC1": the settings record; another magic loads the factory patterns */
typedef struct {
    uint32_t magic;
    pattern_t pat[NPATTERNS];
    uint8_t cur;                         /* the pattern playing (or to be) */
    uint8_t sel;                         /* the instrument picked */
    uint8_t rsv[2];
} settings_t;
_Static_assert(sizeof(settings_t) <= PLAT_STORE_MAX, "the settings outgrew a store object");
static settings_t set;
static uint8_t set_dirty;
static uint32_t dirty_at;

static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
#define PT (set.pat[set.cur])            /* the pattern being edited */
#define TR (PT.tr[set.sel])              /* the picked instrument's track */
#define IN (INST[set.sel])

/* --------------------------------------------------------------- patterns --- */
static void set_step(track_t *t, int s, int on, int acc)
{
    uint32_t b = 1u << (s & 31);
    if (on)
        t->steps[s >> 5] |= b;
    else
        t->steps[s >> 5] &= ~b;
    if (acc)
        t->acc[s >> 5] |= b;
    else
        t->acc[s >> 5] &= ~b;
}

static void factory_load(uint32_t i)     /* pattern slot i from its factory state */
{
    const factory_t *f = &FACTORY[i];
    pattern_t *p = &set.pat[i];
    uint32_t t, k, s;
    memset(p, 0, sizeof *p);
    p->bpm = f->bpm;
    p->swing = f->swing;
    p->mass = f->mass;
    p->chaos = f->chaos;
    p->length = f->length;
    p->drive = f->drive;
    p->space = f->space;
    for (t = 0; t < NINST; t++) {
        track_t *tr = &p->tr[t];
        for (k = 0; k < 4u; k++)
            tr->p[k] = f->p[t][k];
        tr->level = f->level[t];
        for (s = 0; s < NSTEPS && f->steps[t][s]; s++)
            set_step(tr, (int)s, f->steps[t][s] != '.', f->steps[t][s] == 'X');
    }
}

static void settings_defaults(void)
{
    uint32_t i;
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    for (i = 0; i < NPATTERNS; i++)
        factory_load(i);
    set.cur = 0;
    set.sel = I_KICK;
}

static int settings_ok(void)
{
    uint32_t i, t, k;
    if (set.magic != SET_MAGIC || set.cur >= NPATTERNS || set.sel >= NINST)
        return 0;
    for (i = 0; i < NPATTERNS; i++) {
        const pattern_t *p = &set.pat[i];
        if (p->mass > 127u || p->chaos > 127u || p->bpm < 60u || p->bpm > 187u || p->swing > 127u
            || p->drive > 127u || p->space > 127u
            || (p->length != 16u && p->length != 32u && p->length != 48u && p->length != 64u))
            return 0;
        for (t = 0; t < NINST; t++) {
            for (k = 0; k < 4u; k++)
                if (p->tr[t].p[k] > 127u)
                    return 0;
            if (p->tr[t].level > 127u)
                return 0;
        }
    }
    return 1;
}

static void settings_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
}

static void dirty(void)
{
    set_dirty = 1;
    dirty_at = plat_ms();
}

static void settings_save(void)
{
    if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
        set_dirty = 0;
}

/* ------------------------------------------------------- knobs: sweetspots --- */
/* A knob's position runs past each sweetspot REST detents more than its value: turned slowly, it rests on the zone for
 * those detents (a soft notch); turned fast, it runs past. */
#define REST 5
enum { PG_SOUND, PG_MASTER };

static int32_t pos_max(const macro_t *mc) { return 127 + mc->nzones * REST; }

static int32_t val_of_pos(const macro_t *mc, int32_t p)
{
    uint32_t z;
    for (z = 0; z < mc->nzones; z++) {
        int32_t c = mc->centre[z];
        if (p <= c)
            return p;
        if (p <= c + REST)
            return c;
        p -= REST;
    }
    return p > 127 ? 127 : p;
}

static int32_t pos_of_val(const macro_t *mc, int32_t v)
{
    int32_t p = v;
    uint32_t z;
    for (z = 0; z < mc->nzones; z++) {
        if (mc->centre[z] < v)
            p += REST;
        else if (mc->centre[z] == v)
            p += REST / 2;
    }
    return p;
}

static int zone_at(const macro_t *mc, int32_t v)
{
    uint32_t z;
    for (z = 0; z < mc->nzones; z++)
        if (mc->centre[z] == v)
            return (int)z;
    return -1;
}

static int zone_near(const macro_t *mc, int32_t v)
{
    uint32_t z;
    int best = 0;
    int32_t d = 999;
    for (z = 0; z < mc->nzones; z++) {
        int32_t e = v - mc->centre[z];
        if (e < 0)
            e = -e;
        if (e < d) {
            d = e;
            best = (int)z;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------- UI --- */
static struct {
    char msg[2][26];
    uint32_t msg_until;
    const char *big;                     /* a sweetspot just reached: its name, big in the display (and the knob's) */
    const char *big_of;
    uint16_t big_col;
    uint32_t big_until;
    uint32_t knob_at[4];
    uint32_t keys, btn;
    uint32_t rec_t, home_t, seq_t;       /* when REC, HOME, SEQ went down (0: not, or used) */
    uint8_t seq_used;
    uint32_t enc_at[NE];
    uint8_t page;
    uint8_t view;                        /* the page of steps shown: 0 = 1-16 .. 3 = 49-64 */
    uint8_t muted;
    int8_t browse;                       /* PRESET turned: the pattern chosen (-1 none), when */
    uint32_t browse_at;
    int8_t mass_pos, chaos_pos;          /* (unused: the macros rest on their zones like the knobs) */
    int32_t knob_pos[4], mpos[2];
    uint8_t redraw;
    uint32_t drawn_at;
    int32_t step_drawn;
    uint32_t intro_at;
    int8_t intro;                        /* the intro's stage (-1: over) */
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static uint32_t text_len(const char *s)  /* (no libc strlen on the device) */
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void two_digits(int32_t v, char *b)
{
    b[0] = (char)('0' + (v / 10) % 10);
    b[1] = (char)('0' + v % 10);
    b[2] = 0;
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[56];
    for (i = 0; i < 25u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 25u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1400u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

/* the page's knobs: the instrument's, or the master's (BPM is the knob's value + 60) */
static const macro_t *page_knob(int k) { return ui.page == PG_SOUND ? &IN.knob[k] : &MASTER_KNOB[k]; }
static int32_t knob_val(int k)
{
    if (ui.page == PG_SOUND)
        return TR.p[k];
    switch (k) {
    case 0: return PT.bpm - 60;
    case 1: return PT.swing;
    case 2: return PT.drive;
    default: return PT.space;
    }
}
static void knob_store(int k, int32_t v)
{
    if (ui.page == PG_SOUND) {
        TR.p[k] = (uint8_t)v;
        return;
    }
    switch (k) {
    case 0: PT.bpm = (uint8_t)(v + 60); break;
    case 1: PT.swing = (uint8_t)v; break;
    case 2: PT.drive = (uint8_t)v; break;
    default: PT.space = (uint8_t)v; break;
    }
}

static void knobs_sync(void)
{
    uint32_t k;
    for (k = 0; k < 4u; k++)
        ui.knob_pos[k] = pos_of_val(page_knob((int)k), knob_val((int)k));
    ui.mpos[0] = pos_of_val(&MASS_KNOB, PT.mass);
    ui.mpos[1] = pos_of_val(&CHAOS_KNOB, PT.chaos);
}

static int32_t accel(int k, int32_t e, int32_t mid, int32_t fast)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_at[k];
    ui.enc_at[k] = now;
    if (dt < 35u)
        return e * fast;
    if (dt < 80u)
        return e * mid;
    return e;
}

static void big_show(const char *what, const char *of, uint16_t col)
{
    ui.big = what;
    ui.big_of = of;
    ui.big_col = col;
    ui.big_until = plat_ms() + 800u;
    ui.redraw = 1;
    if (ann_on())
        ann_say(what, 0, ANN_NOW);
}

static void knob(int k, int32_t e)
{
    const macro_t *mc = page_knob(k);
    int32_t p = clamp(ui.knob_pos[k] + accel(EN_K1 + k, e, 3, 6), 0, pos_max(mc)), v = val_of_pos(mc, p);
    int32_t was = knob_val(k);
    int za = zone_at(mc, v);
    ui.knob_pos[k] = p;
    ui.knob_at[k] = plat_ms();
    ui.redraw = 1;
    if (v == was)
        return;
    knob_store(k, v);
    dirty();
    if (za >= 0 && za != zone_at(mc, was))
        big_show(mc->zone[za], mc->name, ui.page == PG_SOUND ? IN.rgb : RGB(93, 232, 224));
}

/* SELECT and ALGORITHM: the web's two macros, with sweetspots too */
static void macro_turn(int which, int32_t e)
{
    const macro_t *mc = which ? &CHAOS_KNOB : &MASS_KNOB;
    uint8_t *val = which ? &PT.chaos : &PT.mass;
    int32_t p = clamp(ui.mpos[which] + accel(which ? EN_ALGO : EN_SELECT, e, 3, 6), 0, pos_max(mc));
    int32_t v = val_of_pos(mc, p), was = *val;
    int za = zone_at(mc, v);
    ui.mpos[which] = p;
    ui.redraw = 1;
    if (v == was)
        return;
    *val = (uint8_t)v;
    dirty();
    if (za >= 0 && za != zone_at(mc, was))
        big_show(mc->zone[za], mc->name, which ? RGB(255, 110, 200) : RGB(255, 190, 90));
    else if (which && eng_topology(v) != eng_topology(was))
        big_show(WEB_NAME[eng_topology(v)], "WEB", RGB(255, 110, 200));
}

static void page_set(int pg)
{
    ui.page = (uint8_t)pg;
    ui.big_until = 0;
    knobs_sync();
    app_say(pg == PG_SOUND ? IN.name : "MASTER", pg == PG_SOUND ? "SOUND" : "BPM SWING DRIVE SPACE");
}

static int32_t len_steps(void) { return PT.length; }

static void step_key(uint32_t i)         /* off -> on -> accent -> off */
{
    int32_t s = (int32_t)i + 16 * ui.view;
    if (s >= len_steps())
        return;
    if (!step_on(&TR, s)) {
        set_step(&TR, s, 1, 0);
        if (!eng_playing())
            eng_hit(set.sel, 0.68f);
    } else if (!step_acc(&TR, s)) {
        set_step(&TR, s, 1, 1);
        if (!eng_playing())
            eng_hit(set.sel, 1.0f);
    } else {
        set_step(&TR, s, 0, 0);
    }
    dirty();
    ui.redraw = 1;
}

static void length_next(void)            /* 16 -> 32 -> 48 -> 64 -> 16; a new bar starts as a copy of the first */
{
    int32_t l = PT.length, n = l >= 64 ? 16 : l + 16, t, s;
    if (n > l)
        for (t = 0; t < NINST; t++) {
            track_t *tr = &PT.tr[t];
            int empty = 1;
            for (s = l; s < n; s++)
                if (step_on(tr, s))
                    empty = 0;
            if (empty)
                for (s = 0; s < 16; s++)
                    set_step(tr, l + s, step_on(tr, s), step_acc(tr, s));
        }
    PT.length = (uint8_t)n;
    if (ui.view * 16 >= n)
        ui.view = 0;
    dirty();
    {
        char t[12];
        int_text(n, t);
        text_cat(t, " STEPS");
        app_say("LENGTH", t);
    }
}

static void inst_key(uint32_t i, uint32_t btn)
{
    if (btn >> B_SEQ & 1u) {
        ui.muted ^= (uint8_t)(1u << i);
        ui.seq_used = 1;
        eng_mute((int)i, ui.muted >> i & 1u);
        app_say(INST[i].name, (ui.muted >> i & 1u) ? "MUTED" : "ON");
        return;
    }
    if (!eng_playing())
        eng_hit((int)i, 1.0f);
    if (set.sel != i) {
        set.sel = (uint8_t)i;
        if (ui.page == PG_SOUND)
            knobs_sync();
        dirty();
    }
    ui.redraw = 1;
}

static void pattern_browse(int32_t d)
{
    int32_t b = ui.browse >= 0 ? ui.browse : set.cur;
    char t[24];
    b = (b + (d > 0 ? 1 : NPATTERNS - 1)) % NPATTERNS;
    ui.browse = (int8_t)b;
    ui.browse_at = plat_ms();
    two_digits(b + 1, t);
    text_cat(t, " ");
    text_cat(t, FACTORY[b].name);
    app_say("PATTERN", t);
}

static void pattern_commit(void)
{
    set.cur = (uint8_t)ui.browse;
    ui.browse = -1;
    eng_bind_next(&PT);
    knobs_sync();
    dirty();
    ui.redraw = 1;
}

static void button(uint32_t i)
{
    switch (i) {
    case B_PLAY: eng_play(!eng_playing()); ui.redraw = 1; break;
    case B_REC: ui.rec_t = plat_ms(); break;
    case B_HOME:
        ui.home_t = plat_ms();
        if (ui.page != PG_SOUND)
            page_set(PG_SOUND);
        break;
    case B_FX: page_set(ui.page == PG_MASTER ? PG_SOUND : PG_MASTER); break;
    case B_SEQ:
        ui.seq_t = plat_ms();
        ui.seq_used = 0;
        break;
    case B_OCTDN:
    case B_OCTUP:
        if (len_steps() > 16) {
            int32_t v = (int32_t)ui.view + (i == B_OCTUP ? 1 : -1), pages = len_steps() / 16;
            char t[12];
            ui.view = (uint8_t)clamp(v, 0, pages - 1);
            int_text(ui.view * 16 + 1, t);
            text_cat(t, "-");
            int_text(ui.view * 16 + 16, t + text_len(t));
            app_say("STEPS", t);
        } else {
            app_say("LENGTH", "16 STEPS");
        }
        break;
    default: break;
    }
}

static void held_buttons(uint32_t btn)
{
    uint32_t now = plat_ms();
    if (ui.rec_t) {                      /* REC held a second: the instrument's steps cleared */
        if (!(btn >> B_REC & 1u))
            ui.rec_t = 0;
        else if (now - ui.rec_t >= 1000u) {
            ui.rec_t = 0;
            TR.steps[0] = TR.steps[1] = TR.acc[0] = TR.acc[1] = 0;
            dirty();
            app_say(IN.name, "CLEARED");
        }
    }
    if (ui.home_t) {                     /* HOME held a second: the pattern from its factory state */
        if (!(btn >> B_HOME & 1u))
            ui.home_t = 0;
        else if (now - ui.home_t >= 1000u) {
            ui.home_t = 0;
            factory_load(set.cur);
            knobs_sync();
            dirty();
            app_say("RESET", FACTORY[set.cur].name);
        }
    }
    if (ui.seq_t && !(btn >> B_SEQ & 1u)) {   /* SEQ let go: the length, unless it muted something */
        if (!ui.seq_used && now - ui.seq_t < 600u)
            length_next();
        ui.seq_t = 0;
    }
}

static void input(void)
{
    uint32_t keys = plat_keys(), btn = plat_buttons(), i;
    int32_t e;
    for (i = 0; i < 16u; i++) {
        uint32_t bit = 1u << WK[i];
        if ((keys & bit) && !(ui.keys & bit))
            step_key(i);
    }
    for (i = 0; i < NINST; i++) {
        uint32_t bit = 1u << BK[i];
        if ((keys & bit) && !(ui.keys & bit))
            inst_key(i, btn);
    }
    for (i = 0; i < NB; i++)
        if ((btn >> i & 1u) && !(ui.btn >> i & 1u))
            button(i);
    held_buttons(btn);
    for (i = 0; i < 4u; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0)
            knob((int)i, e);
    if ((e = plat_enc(EN_SELECT)) != 0)
        macro_turn(0, e);
    if ((e = plat_enc(EN_ALGO)) != 0)
        macro_turn(1, e);
    if ((e = plat_enc(EN_PRESET)) != 0)
        pattern_browse(e);
    if (ui.browse >= 0 && plat_ms() - ui.browse_at > 700u)
        pattern_commit();
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
}

static void lights(void)
{
    uint32_t lit = 0, mid = 0, dim = 0, i, btn = 0, dimb;
    int32_t s = eng_step();
    for (i = 0; i < 16u; i++) {          /* the steps shown: accents lit, hits half */
        int32_t st = (int32_t)i + 16 * ui.view;
        uint32_t bit = 1u << WK[i];
        if (st >= len_steps())
            continue;
        if (step_on(&TR, st)) {
            if (step_acc(&TR, st))
                lit |= bit;
            else
                mid |= bit;
        }
    }
    if (s >= 0 && (s >> 4) == ui.view)
        lit ^= 1u << WK[s & 15];
    for (i = 0; i < NINST; i++) {        /* the instruments: picked lit, a hit flashes, the others glow */
        uint32_t bit = 1u << BK[i];
        if (ui.muted >> i & 1u)
            continue;
        if (i == set.sel || eng_env((int)i) > 0.3f)
            lit |= bit;
        else
            dim |= bit;
    }
    if (eng_playing())
        btn |= LED_PLAY_GREEN;
    btn |= 1u << (ui.page == PG_MASTER ? B_FX : B_HOME);
    dimb = (1u << B_PLAY) | (1u << B_REC) | (1u << B_SEQ) | (1u << B_FX) | (1u << B_HOME);
    if (len_steps() > 16)
        dimb |= (1u << B_OCTDN) | (1u << B_OCTUP);
    plat_lights(btn | (ui.btn & ~(1u << B_PLAY)), lit, dimb, dim, 1);
    plat_lights_mid(0, mid & ~lit);
}

void app_input_only(void)
{
    if (ui.intro < 0)
        input();
}

/* ------------------------------------------------------------------- MIDI in --- */
/* At 1 kHz (the timer interrupt): the clock, START, CONTINUE, STOP to the engine; the tempo heard in the clock (the
 * gaps between clocks, smoothed); notes 36-41 (any channel) play the six instruments. */
static uint32_t clk_last;
static float clk_gap;                    /* ms between clocks, smoothed */
static volatile uint8_t note_seen;
void app_tick(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = pkt >> 8 & 0xFFu, d1 = pkt >> 16 & 0x7Fu, d2 = pkt >> 24 & 0x7Fu;
        if (st == 0xF8u) {
            uint32_t now = plat_ms(), gap = now - clk_last;
            clk_last = now;
            eng_ext_clock();
            if (gap > 4u && gap < 200u) {
                clk_gap = clk_gap > 0.0f ? clk_gap + ((float)gap - clk_gap) * 0.08f : (float)gap;
                eng_ext_bpm(60000.0f / (24.0f * clk_gap));
            }
        } else if (st == 0xFAu || st == 0xFBu) {
            eng_ext_start(st == 0xFBu);
        } else if (st == 0xFCu) {
            eng_ext_stop();
        } else if ((st & 0xF0u) == 0x90u && d2 && d1 >= 36u && d1 < 36u + NINST) {
            eng_hit((int)(d1 - 36u), 0.3f + 0.7f * (float)d2 / 127.0f);
            note_seen = 1;
        }
    }
}

/* ------------------------------------------------------------------ drawing --- */
/* The skin: brushed steel with bevels, teal LCDs, a title bar with grooves (a 90s media player); chrome type for the
 * splash (a 90s electronics ad). */
#define SK_STEEL RGB(90, 104, 112)
#define SK_LIGHT RGB(170, 190, 198)
#define SK_DARK RGB(30, 40, 46)
#define SK_TITLE RGB(36, 52, 60)
#define LCD_BG RGB(6, 18, 20)
#define LCD_HI RGB(93, 232, 224)
#define LCD_MID RGB(47, 181, 173)
#define LCD_LO RGB(22, 78, 76)
#define LCD_GRID RGB(12, 38, 40)

static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static void bevel(int32_t x, int32_t y, int32_t w, int32_t h, int raised)
{
    uint16_t a = raised ? SK_LIGHT : SK_DARK, b = raised ? SK_DARK : SK_LIGHT;
    cv_rect(x, y, w, 1, a);
    cv_rect(x, y, 1, h, a);
    cv_rect(x, y + h - 1, w, 1, b);
    cv_rect(x + w - 1, y, 1, h, b);
}

static void lcd_box(int32_t x, int32_t y, int32_t w, int32_t h)   /* a sunken LCD */
{
    bevel(x, y, w, h, 0);
    cv_rect(x + 1, y + 1, w - 2, h - 2, LCD_BG);
}

static uint16_t dim_of(uint16_t c)       /* a colour at about a third */
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)(((r * 3u / 8u) << 11) | ((g * 3u / 8u) << 5) | (b * 3u / 8u));
}

/* the title bar: grooves either side of the name (or a message), the three little buttons */
static void draw_title(uint32_t now)
{
    int32_t x, i;
    cv_rect(0, 0, 240, 15, SK_TITLE);
    bevel(0, 0, 240, 15, 1);
    for (i = 0; i < 3; i++) {
        cv_rect(212 + i * 9, 4, 7, 7, SK_STEEL);
        bevel(212 + i * 9, 4, 7, 7, 1);
    }
    if (now < ui.msg_until) {
        x = cv_text(6, 1, &FONT_XS, ui.msg[0], C_WHITE);
        cv_text(x + 6, 1, &FONT_XS, ui.msg[1], LCD_HI);
        return;
    }
    x = cv_text(6, 1, &FONT_XS, "COLLAPSE", LCD_HI);
    for (i = x + 6; i < 206; i += 3)     /* the grooves */
        cv_rect(i, 4, 1, 7, (i & 1) ? SK_LIGHT : SK_DARK);
}

/* the left LCD: where the beat is (bar.beat, big), the tempo, the clock it follows, the transport; or a sweetspot
 * just reached, big; and six little level bars */
static void draw_time(int32_t y, uint32_t now)
{
    int32_t s = eng_step(), i;
    char t[16];
    lcd_box(4, y, 118, 56);
    if (now < ui.big_until && ui.big) {
        cv_text(10, y + 5, &FONT_XS, ui.big_of, LCD_MID);
        cv_text(10, y + 20, text_w(&FONT_B, ui.big) > 104 ? &FONT_XS : &FONT_B, ui.big, ui.big_col);
    } else {
        int32_t bar = s >= 0 ? s / 16 + 1 : 1, beat = s >= 0 ? (s % 16) / 4 + 1 : 1;
        two_digits(bar, t);
        t[2] = '.';
        t[3] = (char)('0' + beat);
        t[4] = 0;
        cv_text(10, y + 4, &APP_BIG_FONT, t, s >= 0 ? LCD_HI : LCD_LO);
    }
    if (s >= 0) {                        /* play: a triangle; stopped: a square */
        for (i = 0; i < 5; i++)
            cv_rect(104 + i, y + 6 + i, 1, 10 - 2 * i, LCD_HI);
    } else {
        cv_rect(104, y + 7, 8, 8, LCD_LO);
    }
    t[0] = 0;
    if (eng_ext()) {
        text_cat(t, "EXT SYNC");
    } else {
        int_text(PT.bpm, t);
        text_cat(t, " BPM");
    }
    cv_text(10, y + 40, &FONT_XS, t, eng_ext() ? C_WHITE : LCD_MID);
    for (i = 0; i < NINST; i++) {        /* the levels, as a little analyser */
        int32_t h = (int32_t)(fm_clampf(eng_env((int)i) * 1.6f, 0.0f, 1.0f) * 14.0f);
        cv_rect(76 + i * 7, y + 52 - 14, 5, 14, LCD_GRID);
        if (h > 0)
            cv_rect(76 + i * 7, y + 52 - h, 5, h, (ui.muted >> i & 1u) ? LCD_LO : LCD_MID);
    }
}

/* the web: six masses on a hexagon, moved by how far they are pushed, their ties drawn; the picked one ringed */
static const int8_t HEX_X[NINST] = {0, 30, 30, 0, -30, -30}, HEX_Y[NINST] = {-20, -10, 10, 20, 10, -10};
static void draw_web(int32_t y)
{
    int32_t cx = 180, cy = y + 26, nx[NINST], ny[NINST], i, j, topo = eng_topology(PT.chaos);
    lcd_box(124, y, 112, 56);
    for (i = 0; i < NINST; i++) {
        float d[4];
        eng_disp((int)i, d);
        nx[i] = cx + HEX_X[i] + (int32_t)(d[0] * 0.22f + d[2] * 0.12f);
        ny[i] = cy + HEX_Y[i] + (int32_t)(d[1] * 0.18f + d[3] * 0.1f);
        nx[i] = clamp(nx[i], 128, 231);
        ny[i] = clamp(ny[i], y + 4, y + 51);
    }
    for (i = 0; i < NINST; i++)          /* the ties of the topology now */
        for (j = i + 1; j < NINST; j++) {
            int tie = topo == 3 || (topo == 2 && i == 0) || ((topo <= 1) && j == i + 1)
                      || (topo == 1 && i == 0 && j == NINST - 1);
            if (tie)
                cv_line(nx[i], ny[i], nx[j], ny[j], topo == 3 ? LCD_LO : LCD_MID);
        }
    for (i = 0; i < NINST; i++) {
        int32_t r = 1 + (int32_t)(eng_env((int)i) * 4.0f);
        uint16_t c = (ui.muted >> i & 1u) ? LCD_LO : INST[i].rgb;
        r = clamp(r, 1, 4);
        cv_rect(nx[i] - r, ny[i] - r, 2 * r + 1, 2 * r + 1, c);
        if (i == set.sel) {
            int32_t q = r + 3;
            cv_rect(nx[i] - q, ny[i] - q, 2 * q + 1, 1, C_WHITE);
            cv_rect(nx[i] - q, ny[i] + q, 2 * q + 1, 1, C_WHITE);
            cv_rect(nx[i] - q, ny[i] - q, 1, 2 * q + 1, C_WHITE);
            cv_rect(nx[i] + q, ny[i] - q, 1, 2 * q + 1, C_WHITE);
        }
    }
    cv_text(128, y + 42, &FONT_XS, WEB_NAME[topo], LCD_MID);
}

/* the six "equaliser" sliders: each instrument's groove, its thumb where the web has it (how hard it moves), its
 * level lit under; the picked one's name lit, a muted one's struck out */
static void draw_sliders(int32_t y)
{
    int32_t i;
    cv_rect(0, y, 240, 50, SK_STEEL);
    bevel(2, y, 236, 50, 1);
    for (i = 0; i < NINST; i++) {
        int32_t x = 9 + i * 38, gx = x + 13, top = y + 4, h = 30;
        int32_t th = top + h - 6 - (int32_t)(eng_energy((int)i) * (float)(h - 6));
        int32_t lv = (int32_t)(fm_clampf(eng_env((int)i) * 1.5f, 0.0f, 1.0f) * (float)(h - 2));
        int sel = (uint32_t)i == set.sel, mute = ui.muted >> i & 1u;
        cv_rect(gx, top, 6, h, SK_DARK);
        bevel(gx, top, 6, h, 0);
        if (lv > 0 && !mute)
            cv_rect(gx + 2, top + h - 1 - lv, 2, lv, INST[i].rgb);
        cv_rect(x + 5, th, 22, 6, sel ? INST[i].rgb : SK_LIGHT);   /* the thumb */
        bevel(x + 5, th, 22, 6, 1);
        cv_rect(x + 6, th + 2, 20, 1, sel ? C_WHITE : SK_DARK);
        ctext(x + 16, y + 36, &FONT_XS, INST[i].tag, mute ? SK_DARK : sel ? C_WHITE : SK_DARK);
        if (mute)
            cv_rect(x + 5, y + 42, 22, 1, RGB(255, 80, 80));
    }
}

/* the four knobs: balance sliders with their sweetspots as ticks, the zone they are on or near */
static void draw_knobs(int32_t y, uint32_t now)
{
    int32_t k;
    uint16_t col = ui.page == PG_SOUND ? IN.rgb : LCD_HI;
    cv_rect(0, y, 240, 44, SK_STEEL);
    bevel(2, y, 236, 44, 1);
    for (k = 0; k < 4; k++) {
        const macro_t *mc = page_knob(k);
        int32_t x = 7 + k * 58, w = 52, v = knob_val(k), z = zone_at(mc, v), zn = zone_near(mc, v);
        int32_t fx = x + 1 + (v * (w - 8)) / 127;
        int hot = ui.knob_at[k] && now - ui.knob_at[k] < 600u;
        uint32_t i;
        char t[8];
        cv_text(x, y + 2, &FONT_XS, mc->name, hot ? C_WHITE : SK_DARK);
        cv_rect(x, y + 17, w, 5, SK_DARK);
        bevel(x, y + 17, w, 5, 0);
        for (i = 0; i < mc->nzones; i++)
            cv_rect(x + 3 + (mc->centre[i] * (w - 8)) / 127, y + 14, 1, 2, (int)i == z ? C_WHITE : SK_DARK);
        cv_rect(fx, y + 15, 7, 9, z >= 0 ? col : SK_LIGHT);
        bevel(fx, y + 15, 7, 9, 1);
        if (hot && z < 0) {
            int_text(ui.page == PG_MASTER && k == 0 ? v + 60 : v, t);
            ctext(x + w / 2, y + 27, &FONT_XS, t, C_WHITE);
        } else if (ui.page == PG_MASTER && k == 0) {
            int_text(v + 60, t);
            ctext(x + w / 2, y + 27, &FONT_XS, t, z >= 0 ? C_WHITE : SK_DARK);
        } else {
            ctext(x + w / 2, y + 27, &FONT_XS, mc->zone[z >= 0 ? z : zn], z >= 0 ? C_WHITE : SK_DARK);
        }
    }
}

/* MASS and CHAOS: two segment meters, as a media player's volume and balance */
static void draw_macros(int32_t y)
{
    int32_t m, i;
    cv_rect(0, y, 240, 18, SK_STEEL);
    for (m = 0; m < 2; m++) {
        const macro_t *mc = m ? &CHAOS_KNOB : &MASS_KNOB;
        int32_t x = 4 + m * 118, v = m ? PT.chaos : PT.mass, segs = (v * 16 + 63) / 127;
        uint16_t c = m ? RGB(255, 110, 200) : RGB(255, 190, 90);
        lcd_box(x, y + 1, 114, 16);
        cv_text(x + 4, y + 2, &FONT_XS, mc->name, LCD_MID);
        for (i = 0; i < 16; i++)
            cv_rect(x + 46 + i * 4, y + 5, 3, 8, i < segs ? c : LCD_GRID);
    }
}

/* the playlist: the pattern (and the one queued), then the picked instrument's steps on the page shown */
static void draw_playlist(int32_t y, int32_t s)
{
    int32_t i, j;
    char t[40];
    lcd_box(2, y, 236, 57);
    cv_rect(4, y + 2, 232, 14, RGB(18, 58, 61));
    two_digits(set.cur + 1, t);
    text_cat(t, ". ");
    text_cat(t, FACTORY[set.cur].name);
    text_cat(t, " - ");
    text_cat(t, FACTORY[set.cur].genre);
    cv_text(8, y + 3, &FONT_XS, t, C_WHITE);
    int_text(PT.bpm, t);
    cv_text(232 - text_w(&FONT_XS, t), y + 3, &FONT_XS, t, C_WHITE);
    if (ui.browse >= 0 || eng_next_pending()) {   /* the next one */
        int32_t b = ui.browse >= 0 ? ui.browse : set.cur;
        two_digits(b + 1, t);
        text_cat(t, ". ");
        text_cat(t, FACTORY[b].name);
        cv_text(8, y + 17, &FONT_XS, t, LCD_MID);
        cv_text(232 - text_w(&FONT_XS, "NEXT"), y + 17, &FONT_XS, "NEXT", LCD_HI);
    } else {
        int32_t x = cv_text(8, y + 17, &FONT_XS, IN.name, IN.rgb);
        t[0] = 0;
        int_text(ui.view * 16 + 1, t);
        text_cat(t, "-");
        int_text(ui.view * 16 + 16, t + text_len(t));
        text_cat(t, " / ");
        int_text(len_steps(), t + text_len(t));
        cv_text(x + 8, y + 17, &FONT_XS, t, LCD_MID);
        t[0] = 0;
        text_cat(t, "CPU ");
        int_text((int32_t)plat_cpu_pct(), t + 4);
        text_cat(t, "%");
        cv_text(232 - text_w(&FONT_XS, t), y + 17, &FONT_XS, t, plat_cpu_pct() > 80u ? C_WHITE : LCD_LO);
    }
    for (i = 0; i < 16; i++) {           /* the steps: accents full, hits half, the others' hits as dots */
        int32_t st = i + 16 * ui.view, x = 6 + i * 14 + (i / 4) * 1, yy = y + 32, mx = x + 1;
        int on, acc;
        if (st >= len_steps()) {
            cv_rect(x, yy, 12, 13, LCD_GRID);
            continue;
        }
        on = step_on(&TR, st);
        acc = on && step_acc(&TR, st);
        cv_rect(x, yy, 12, 13, acc ? IN.rgb : on ? dim_of(IN.rgb) : LCD_GRID);
        if (acc)
            cv_rect(x + 2, yy + 2, 8, 2, C_WHITE);
        for (j = 0; j < NINST; j++)
            if ((uint32_t)j != set.sel && step_on(&PT.tr[j], st) && mx < x + 11) {
                cv_rect(mx, yy + 15, 2, 2, (ui.muted >> j & 1u) ? LCD_LO : INST[j].rgb);
                mx += 2;
            }
        if (st == s)
            cv_rect(x, yy + 19, 12, 3, C_WHITE);
        else if (i % 4 == 0)
            cv_rect(x, yy + 19, 2, 2, LCD_LO);
    }
}

static void draw(uint32_t now)
{
    int32_t s = eng_step();
    cv_begin(240, 120, SK_STEEL);
    draw_title(now);
    draw_time(17, now);
    draw_web(17);
    draw_sliders(70);
    cv_blit(0, 0);
    cv_begin(240, 120, SK_STEEL);
    draw_knobs(0, now);
    draw_macros(44);
    draw_playlist(63, s);
    cv_blit(0, 120);
    ui.drawn_at = now;
}

/* ---------------------------------------------------------- splash, intro --- */
/* chrome type: the word drawn dark, then mid, then light, a pixel apart each (a 90s electronics ad) */
static void chrome(int32_t cx, int32_t y, const felucca_font_t *f, const char *s)
{
    int32_t x = cx - text_w(f, s) / 2;
    cv_text(x + 2, y + 2, f, s, RGB(20, 40, 48));
    cv_text(x + 1, y + 1, f, s, RGB(80, 120, 130));
    cv_text(x, y, f, s, RGB(225, 245, 250));
}

static void draw_logo(int lit)          /* the name on a horizon grid; lit: how many of the six masses have fallen */
{
    int32_t i;
    cv_begin(240, 120, C_BLACK);
    for (i = 0; i < 6; i++)              /* the chrome bands behind the name */
        cv_rect(0, 40 + i * 6, 240, 2, i & 1 ? RGB(14, 40, 44) : RGB(22, 60, 64));
    chrome(120, 44, &APP_BIG_FONT, "COLLAPSE");
    cv_rect(30, 86, 180, 1, LCD_HI);
    ctext(120, 92, &FONT_XS, "PHYSICS DRUM MACHINE", LCD_MID);
    cv_blit(0, 0);
    cv_begin(240, 120, C_BLACK);
    for (i = 0; i < 7; i++)              /* the horizon grid */
        cv_rect(0, 50 + i * i * 2, 240, 1, LCD_LO);
    for (i = -6; i <= 6; i++)
        cv_line(120 + i * 8, 50, 120 + i * 40, 119, LCD_GRID);
    for (i = 0; i < NINST; i++) {
        int on = i < lit;
        int32_t x = 120 + (on ? HEX_X[i] / 3 : HEX_X[i] * 2), yy = on ? 30 + HEX_Y[i] / 3 : 30 + HEX_Y[i];
        cv_rect(x - 3, yy - 3, 7, 7, on ? INST[i].rgb : RGB(40, 60, 64));
    }
    ctext(120, 100, &FONT_XS, "by swtdr34mz", RGB(150, 160, 168));
    cv_blit(0, 120);
}

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_logo(0);
}

#define INTRO_STEP 150u
static int intro_frame(uint32_t now)     /* 1 while the intro runs: the six masses fall together, each with its hit */
{
    int k;
    if (ui.intro < 0)
        return 0;
    if (plat_keys() || plat_buttons()) {
        ui.intro = -1;
        ui.keys = plat_keys();
        ui.btn = plat_buttons();
        ui.redraw = 1;
        return 0;
    }
    k = (int)((now - ui.intro_at) / INTRO_STEP);
    if (k > NINST + 4) {
        ui.intro = -1;
        ui.redraw = 1;
        return 0;
    }
    if (k > ui.intro && k <= NINST) {
        ui.intro = (int8_t)k;
        eng_hit(k - 1, 0.6f);
        if (k == NINST)
            eng_kick_web();              /* the collapse: the whole web shoved at once */
        draw_logo(k);
    }
    return 1;
}

/* ------------------------------------------------------------- the contract --- */
void app_init(void)
{
    plat_cpu2_set(1);
    eng_init();
    settings_load();
    eng_bind(&PT);
    ui.browse = -1;
    knobs_sync();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    eng_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f);
}

void app_panic(void)
{
    eng_play(0);
    eng_panic();
}

void app_ui_init(void)
{
    ui.browse = -1;
    ui.step_drawn = -2;
    ui.redraw = 1;
    ui.intro = 0;
    ui.intro_at = plat_ms();
    draw_logo(0);
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    int32_t s;
    if (intro_frame(now)) {
        plat_lights(0, 0, 0, 0, 0);
        return;
    }
    input();
    lights();
    if (ann_on() && ann_where()) {
        char t[64] = "COLLAPSE. ";
        text_cat(t, FACTORY[set.cur].name);
        text_cat(t, ". ");
        text_cat(t, IN.name);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(now);
    s = eng_step();
    if (s != ui.step_drawn) {
        ui.step_drawn = s;
        ui.redraw = 1;
    }
    if ((ui.msg_until && now >= ui.msg_until) || (ui.big_until && now >= ui.big_until)) {
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        if (now >= ui.big_until)
            ui.big_until = 0;
        ui.redraw = 1;
    }
    if (now - ui.drawn_at >= 40u)        /* the web and the levels move: 25 frames a second */
        ui.redraw = 1;
    if (set_dirty && now - dirty_at > 4000u)
        settings_save();
    if (ui.redraw) {
        ui.redraw = 0;
        draw(now);
    }
}

/* ------------------------------------------------------------- backup ------ */
int app_backup_playing(void) { return eng_playing(); }
void app_backup_flush(void)
{
    if (set_dirty)
        settings_save();
}
void app_backup_hold(int on) { (void)on; }
void app_backup_restored(void)
{
    settings_load();
    eng_bind(&PT);
    knobs_sync();
    ui.redraw = 1;
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "steps"))          /* the picked instrument's, steps 1-32 */
        *got = (int)TR.steps[0];
    else if (!strcmp(what, "steps2"))    /* 33-64 */
        *got = (int)TR.steps[1];
    else if (!strcmp(what, "acc"))
        *got = (int)TR.acc[0];
    else if (!strcmp(what, "sel"))
        *got = set.sel;
    else if (!strcmp(what, "cur"))
        *got = set.cur;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "view"))
        *got = ui.view;
    else if (!strcmp(what, "muted"))
        *got = ui.muted;
    else if (!strcmp(what, "bpm"))
        *got = PT.bpm;
    else if (!strcmp(what, "length"))
        *got = PT.length;
    else if (!strcmp(what, "mass"))
        *got = PT.mass;
    else if (!strcmp(what, "chaos"))
        *got = PT.chaos;
    else if (!strcmp(what, "playing"))
        *got = eng_playing();
    else if (!strcmp(what, "ext"))
        *got = eng_ext();
    else if (!strcmp(what, "step"))
        *got = eng_step();
    else if (!strcmp(what, "hits"))
        *got = (int)eng_hits();
    else if (!strcmp(what, "queued"))
        *got = eng_next_pending();
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "energy"))    /* the web's motion, all six, x1000 */
    {
        int i;
        float e = 0.0f;
        for (i = 0; i < NINST; i++)
            e += eng_energy(i);
        *got = (int)(e * 1000.0f);
    } else if (what[0] == 'p' && what[1] >= '1' && what[1] <= '4' && !what[2])   /* p1..p4: the picked one's knobs */
        *got = TR.p[what[1] - '1'];
    else if (what[0] == 'k' && what[1] >= '1' && what[1] <= '4' && !what[2])   /* k1..k4: the page's knobs */
        *got = knob_val(what[1] - '1');
    else if (!strncmp(what, "zone", 4) && what[4] >= '1' && what[4] <= '4')
        *got = zone_at(page_knob(what[4] - '1'), knob_val(what[4] - '1'));
    else
        return 0;
    return 1;
}
#endif
