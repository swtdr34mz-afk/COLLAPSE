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
 *   K1..K4          the page's four knobs; turned slowly each rests a few detents on its sweetspots. The pages:
 *                   HOME the instrument's SOUND, EDIT its ENGINE (FM, GRAIN, FILTER, CHAOS: its bounces), FX the
 *                   master's (DRIVE, SPACE, PITCH, GLITCH), GLO the GENERAL page (BPM, SWING, DRIFT, the track's
 *                   LENGTH). The same button again: back to SOUND
 *   SELECT          MASS: FEATHER .. BLACK HOLE (light and nervous .. heavy, slow and dirty)
 *   ALGORITHM       CHAOS: the web's ties (CHAIN, RING, STAR, MESH) and how hard every hit pushes
 *   PRESET          the eight patterns: turn, and it takes over on the next beat, from its top (stopped: at once)
 *   HOME held 1 s   the pattern back to its factory state
 *   SEQ             the picked track's length: 16, 32, 48, 64 steps (a new bar starts as a copy of the first); each
 *                   track has its own length (LENGTH on GENERAL: 1 .. 64), so tracks run against each other
 *   OCT- / OCT+     the page of steps shown (1-16 .. 49-64)
 *   PLAY            start from step 1 / stop.   REC held 1 s: the picked instrument's steps cleared
 *   SAVE            saved now (it saves itself a few seconds after a change anyway); SAVE held + PRESET: a slot, and
 *                   on letting go the pattern is copied there
 *   ARP             PERF mode: the white keys are 16 pads, the instruments' keys mutes, black keys 7-11 live FX
 *   MIDI in (USB, TRS): clock, START, CONTINUE, STOP: COLLAPSE follows; notes 36-41 play the six instruments.
 *   Settings save themselves a few seconds after the last change. */
#include "fastmath.h"
#include "engine.h"

#define SET_MAGIC 0x32434F43u            /* "COC2": the settings record; another magic loads the factory patterns */
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
static const char *const PAD_SHORT[NPADS] = {"CLPS", "0-G", "HOLE", "FRZ", "RWND", "1/8", "1/16", "1/32", "TAPE",
                                             "SWP", "DRFT", "AVAL", "UP", "DOWN", "BRK", "DROP", "GATE", "HALF",
                                             "LOFI", "THRW", "STUT"};
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
    p->drift = f->drift;
    p->drive = f->drive;
    p->space = f->space;
    p->pitch = f->pitch;
    p->glitch = f->glitch;
    for (t = 0; t < NINST; t++) {
        track_t *tr = &p->tr[t];
        for (k = 0; k < 4u; k++) {
            tr->p[k] = f->p[t][k];
            tr->fx[k] = f->fx[t][k];
        }
        tr->level = f->level[t];
        for (s = 0; s < NSTEPS && f->steps[t][s]; s++)
            set_step(tr, (int)s, f->steps[t][s] != '.', f->steps[t][s] == 'X');
        tr->len = (uint8_t)(s ? s : 16u);   /* the string's length is the track's */
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
            || p->drive > 127u || p->space > 127u || p->drift > 127u || p->pitch > 127u || p->glitch > 127u)
            return 0;
        for (t = 0; t < NINST; t++) {
            for (k = 0; k < 4u; k++)
                if (p->tr[t].p[k] > 127u || p->tr[t].fx[k] > 127u)
                    return 0;
            if (p->tr[t].level > 127u || p->tr[t].len < 1u || p->tr[t].len > 64u)
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
enum { PG_SOUND, PG_ENGINE, PG_FX, PG_GENERAL, NPAGES };
static const char *const PAGE_NAME[NPAGES] = {"SOUND", "ENGINE", "FX", "GENERAL"};

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
    uint8_t save_down;                   /* SAVE held */
    int8_t copy_to;                      /* SAVE held + PRESET: the slot the pattern is copied to on release (-1 none) */
    uint8_t seq_used;
    uint32_t enc_at[NE];
    uint8_t page;
    uint8_t view;                        /* the page of steps shown: 0 = 1-16 .. 3 = 49-64 */
    uint8_t muted;
    uint8_t perf;                        /* PERF mode (ARP): the white keys are the pads */
    uint32_t pads;
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

/* the page's knobs: the instrument's SOUND and ENGINE, the master's FX, the GENERAL page (BPM: the knob's value + 60;
 * LENGTH: the picked track's steps - 1) */
static const macro_t *page_knob(int k)
{
    switch (ui.page) {
    case PG_ENGINE: return &ENGINE_KNOB[k];
    case PG_FX: return &MASTER_KNOB[k];
    case PG_GENERAL: return &GENERAL_KNOB[k];
    default: return &IN.knob[k];
    }
}
static uint8_t *page_byte(int k)
{
    switch (ui.page) {
    case PG_ENGINE: return &TR.fx[k];
    case PG_FX: return k == 0 ? &PT.drive : k == 1 ? &PT.space : k == 2 ? &PT.pitch : &PT.glitch;
    case PG_GENERAL: return k == 0 ? &PT.bpm : k == 1 ? &PT.swing : k == 2 ? &PT.drift : &TR.len;
    default: return &TR.p[k];
    }
}
static int32_t knob_val(int k)
{
    int32_t v = *page_byte(k);
    if (ui.page == PG_GENERAL && k == 0)
        return v - 60;
    if (ui.page == PG_GENERAL && k == 3)
        return v - 1;
    return v;
}
static void knob_store(int k, int32_t v)
{
    if (ui.page == PG_GENERAL && k == 0)
        v += 60;
    if (ui.page == PG_GENERAL && k == 3) {
        v += 1;
        if (v > 64)
            v = 64;
    }
    *page_byte(k) = (uint8_t)v;
    if (ui.page == PG_GENERAL && k == 3 && ui.view * 16 >= v)
        ui.view = 0;
}
static int knob_shows_number(int k)      /* BPM and LENGTH show their number, not a zone name, between zones */
{
    return ui.page == PG_GENERAL && (k == 0 || k == 3);
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
        big_show(mc->zone[za], mc->name, ui.page <= PG_ENGINE ? IN.rgb : RGB(93, 232, 224));
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
    static const char *const WHAT[NPAGES] = {"", "FM GRAIN FILTER CHAOS", "DRIVE SPACE PITCH GLITCH",
                                             "BPM SWING DRIFT LENGTH"};
    ui.page = (uint8_t)pg;
    ui.big_until = 0;
    knobs_sync();
    app_say(pg <= PG_ENGINE ? IN.name : PAGE_NAME[pg], pg == PG_SOUND ? "SOUND" : WHAT[pg]);
}

static int32_t len_steps(void) { return TR.len; }

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

static void length_next(void)            /* the picked track: 16 -> 32 -> 48 -> 64 -> 16 (or up to the next sixteen);
                                          * a new bar starts as a copy of the first */
{
    int32_t l = TR.len, n = l >= 64 ? 16 : (l / 16 + 1) * 16, s;
    if (n > l && l >= 16) {
        int empty = 1;
        for (s = l; s < n; s++)
            if (step_on(&TR, s))
                empty = 0;
        if (empty)
            for (s = l; s < n; s++)
                set_step(&TR, s, step_on(&TR, s % 16), step_acc(&TR, s % 16));
    }
    TR.len = (uint8_t)n;
    if (ui.view * 16 >= n)
        ui.view = 0;
    knobs_sync();
    dirty();
    {
        char t[12];
        int_text(n, t);
        text_cat(t, " STEPS");
        app_say(IN.name, t);
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
    case B_EDIT: page_set(ui.page == PG_ENGINE ? PG_SOUND : PG_ENGINE); break;
    case B_FX: page_set(ui.page == PG_FX ? PG_SOUND : PG_FX); break;
    case B_GLO: page_set(ui.page == PG_GENERAL ? PG_SOUND : PG_GENERAL); break;
    case B_SEQ:
        ui.seq_t = plat_ms();
        ui.seq_used = 0;
        break;
    case B_SAVE:
        ui.save_down = 1;
        ui.copy_to = -1;
        break;
    case B_ARP:
        ui.perf = (uint8_t)!ui.perf;
        ui.pads = 0;
        eng_pads(0);
        app_say("PERFORM", ui.perf ? "PADS ON" : "PADS OFF");
        break;
    case B_OCTDN:
    case B_OCTUP:
        if (len_steps() > 16) {
            int32_t v = (int32_t)ui.view + (i == B_OCTUP ? 1 : -1), pages = (len_steps() + 15) / 16;
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
    if (ui.save_down && !(btn >> B_SAVE & 1u)) {   /* SAVE let go: copied where PRESET chose, or saved now */
        ui.save_down = 0;
        if (ui.copy_to >= 0 && ui.copy_to != (int8_t)set.cur) {
            char t[24];
            set.pat[ui.copy_to] = PT;
            two_digits(ui.copy_to + 1, t);
            settings_save();
            app_say("COPIED TO", t);
        } else {
            settings_save();
            app_say("SAVED", FACTORY[set.cur].name);
        }
        ui.copy_to = -1;
    }
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
    if (ui.perf) {                       /* the pads: the white keys and the black keys 7-11, while held */
        uint32_t pads = 0, on;
        for (i = 0; i < 16u; i++)
            if (keys >> WK[i] & 1u)
                pads |= 1u << i;
        for (i = 0; i < 5u; i++)
            if (keys >> BK[NINST + i] & 1u)
                pads |= 1u << (P_GATE + i);
        on = pads & ~ui.pads;
        for (i = 0; i < NPADS; i++)
            if (on >> i & 1u)
                app_say("PAD", PAD_NAME[i]);
        if (pads != ui.pads) {
            ui.pads = pads;
            eng_pads(pads);
            ui.redraw = 1;
        }
    } else {
        for (i = 0; i < 16u; i++) {
            uint32_t bit = 1u << WK[i];
            if ((keys & bit) && !(ui.keys & bit))
                step_key(i);
        }
    }
    for (i = 0; i < NINST; i++) {        /* the instruments (PERF mode: their mutes, as the LXR-02's) */
        uint32_t bit = 1u << BK[i];
        if ((keys & bit) && !(ui.keys & bit))
            inst_key(i, ui.perf ? (1u << B_SEQ) : btn);
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
    if ((e = plat_enc(EN_PRESET)) != 0) {
        if (ui.save_down) {              /* SAVE held: where to copy the pattern */
            char t[24];
            int32_t c = ui.copy_to >= 0 ? ui.copy_to : set.cur;
            c = (c + (e > 0 ? 1 : NPATTERNS - 1)) % NPATTERNS;
            ui.copy_to = (int8_t)c;
            two_digits(c + 1, t);
            text_cat(t, " ");
            text_cat(t, FACTORY[c].name);
            app_say("COPY TO", t);
        } else {
            pattern_browse(e);
        }
    }
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
    int32_t s = eng_track_step(set.sel);
    for (i = 0; i < 16u; i++) {          /* the steps shown: accents lit, hits half (PERF: the pads, held lit) */
        int32_t st = (int32_t)i + 16 * ui.view;
        uint32_t bit = 1u << WK[i];
        if (ui.perf) {
            if (ui.pads >> i & 1u)
                lit |= bit;
            else
                dim |= bit;
            continue;
        }
        if (st >= len_steps())
            continue;
        if (step_on(&TR, st)) {
            if (step_acc(&TR, st))
                lit |= bit;
            else
                mid |= bit;
        }
    }
    if (!ui.perf && s >= 0 && (s >> 4) == ui.view)
        lit ^= 1u << WK[s & 15];
    if (ui.perf)                         /* the live FX keys */
        for (i = 0; i < 5u; i++) {
            if (ui.pads >> (P_GATE + i) & 1u)
                lit |= 1u << BK[NINST + i];
            else
                dim |= 1u << BK[NINST + i];
        }
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
    if (ui.perf)
        btn |= 1u << B_ARP;
    {
        static const uint8_t PB[NPAGES] = {B_HOME, B_EDIT, B_FX, B_GLO};
        btn |= 1u << PB[ui.page];
    }
    dimb = (1u << B_PLAY) | (1u << B_REC) | (1u << B_SEQ) | (1u << B_FX) | (1u << B_HOME) | (1u << B_EDIT)
           | (1u << B_GLO) | (1u << B_ARP);
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
/* The look: 90s cyberpunk, not clean. Gunmetal with grain and scratches under everything; displays with scanlines;
 * a liquid plasma behind the web that flows faster and brighter the harder the web moves; grooves where light runs
 * like liquid; the big digits split into magenta and cyan; cut corners and neon edges; and on a hard hit a few rows of
 * the screen tear sideways for a frame. A 90s media player's layout underneath it all. */
#define BG RGB(12, 10, 22)
#define PANEL RGB(20, 18, 34)
#define EDGE RGB(60, 56, 92)
#define NEON RGB(40, 240, 255)
#define MAGENTA RGB(255, 50, 190)
#define ACID RGB(190, 255, 40)
#define PURPLE RGB(120, 70, 255)
#define LCD_BG RGB(5, 7, 15)
#define LCD_SCAN RGB(11, 14, 28)
#define LCD_HI RGB(93, 232, 224)
#define LCD_MID RGB(47, 170, 175)
#define LCD_LO RGB(30, 70, 90)
#define LCD_GRID RGB(18, 26, 44)
#define TXT RGB(150, 146, 190)

static uint32_t frame_n;                 /* frames drawn (the animations' clock) */
static uint8_t tear;                     /* the tear to do in this frame's bands (0: none) */

static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static uint16_t mix565(uint16_t a, uint16_t b, int32_t t)   /* a .. b, t 0..256 */
{
    int32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + (br - ar) * t / 256) << 11) | ((ag + (bg - ag) * t / 256) << 5) | (ab + (bb - ab) * t / 256));
}

static uint16_t dim_of(uint16_t c) { return mix565(BG, c, 96); }

/* the grain: a 64 x 32 tile of gunmetal, worked out once (noise, a few scratches), laid under a rectangle */
static uint16_t grain[32][64];
static void grain_init(void)
{
    uint32_t h = 0x9E3779B9u;
    int32_t x, y, k;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 64; x++) {
            int32_t n;
            h ^= h << 13;
            h ^= h >> 17;
            h ^= h << 5;
            n = (int32_t)(h & 15u);
            grain[y][x] = mix565(RGB(26, 24, 40), RGB(48, 46, 70), n * 8 + ((x + y) & 7) * 4);
        }
    for (k = 0; k < 5; k++) {            /* scratches: thin diagonals */
        int32_t sx = (int32_t)((h >> (k * 5)) & 63u), sy = k * 6;
        for (x = 0; x < 14; x++)
            grain[(sy + x / 3) & 31][(sx + x) & 63] = RGB(80, 78, 110);
    }
}
static void grunge(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            cv_pset(x + i, y + j, grain[(y + j) & 31][(x + i) & 63]);
}

/* a panel with cut corners and a neon edge */
static void panel(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t fill, uint16_t edge)
{
    int32_t c = 4, k;
    cv_rect(x + c, y, w - 2 * c, h, fill);
    cv_rect(x, y + c, w, h - 2 * c, fill);
    for (k = 0; k < c; k++) {
        cv_rect(x + c - k, y + k, w - 2 * (c - k), 1, fill);
        cv_rect(x + c - k, y + h - 1 - k, w - 2 * (c - k), 1, fill);
    }
    cv_rect(x + c, y, w - 2 * c, 1, edge);
    cv_rect(x + c, y + h - 1, w - 2 * c, 1, edge);
    cv_rect(x, y + c, 1, h - 2 * c, edge);
    cv_rect(x + w - 1, y + c, 1, h - 2 * c, edge);
    cv_line(x, y + c, x + c, y, edge);
    cv_line(x + w - 1 - c, y, x + w - 1, y + c, edge);
    cv_line(x, y + h - 1 - c, x + c, y + h - 1, edge);
    cv_line(x + w - 1 - c, y + h - 1, x + w - 1, y + h - 1 - c, edge);
}

/* a display: dark, with scanlines */
static void lcd_box(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t j;
    panel(x, y, w, h, LCD_BG, EDGE);
    for (j = y + 2; j < y + h - 2; j += 2)
        cv_rect(x + 2, j, w - 4, 1, LCD_SCAN);
}

/* text split into magenta and cyan (a 90s video glitch), wider when the beat hits */
static void chroma(int32_t x, int32_t y, const felucca_font_t *f, const char *s, int32_t d, uint16_t c)
{
    cv_text(x - d, y, f, s, MAGENTA);
    cv_text(x + d, y, f, s, NEON);
    cv_text(x, y, f, s, c);
}

/* the tear: a few rows of the band just drawn, moved sideways (a hard hit) */
static void tear_band(void)
{
    uint32_t h = 0x2545F491u ^ (frame_n * 2654435761u), r, k;
    if (!tear)
        return;
    for (k = 0; k < tear; k++) {
        uint32_t y0, rows, row;
        int32_t sh;
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        y0 = h % cv_h;
        rows = 1u + (h >> 8) % 5u;
        sh = (int32_t)((h >> 16) % 13u) - 6;
        for (row = y0; row < y0 + rows && row < cv_h; row++) {
            uint16_t *p = cv_px + row * cv_w;
            if (sh > 0)
                for (r = cv_w - 1; r >= (uint32_t)sh; r--)
                    p[r] = p[r - (uint32_t)sh];
            else if (sh < 0)
                for (r = 0; r + (uint32_t)(-sh) < cv_w; r++)
                    p[r] = p[r + (uint32_t)(-sh)];
        }
    }
}

/* the title bar: the name split in colour, a ticker running through, the three little buttons (or a message) */
static void draw_title(uint32_t now)
{
    int32_t x, i;
    char t[64];
    cv_rect(0, 0, 240, 15, RGB(16, 12, 30));
    cv_rect(0, 14, 240, 1, MAGENTA);
    for (i = 0; i < 3; i++)
        cv_rect(214 + i * 8, 4, 6, 6, i == 2 ? MAGENTA : EDGE);
    if (now < ui.msg_until) {
        x = cv_text(6, 1, &FONT_XS, ui.msg[0], C_WHITE);
        cv_text(x + 6, 1, &FONT_XS, ui.msg[1], NEON);
        return;
    }
    chroma(6, 1, &FONT_XS, "COLLAPSE", (frame_n & 15u) == 0 ? 2 : 1, C_WHITE);
    t[0] = 0;                            /* the ticker */
    text_cat(t, "/// ");
    text_cat(t, FACTORY[set.cur].name);
    text_cat(t, " /// ");
    text_cat(t, WEB_NAME[eng_topology(PT.chaos)]);
    text_cat(t, " /// ");
    text_cat(t, MASS_KNOB.zone[zone_near(&MASS_KNOB, PT.mass)]);
    text_cat(t, " ");
    {
        int32_t w = text_w(&FONT_XS, t), off = (int32_t)(frame_n % (uint32_t)w), x0 = 70 - off;
        cv_text(x0, 1, &FONT_XS, t, LCD_LO);
        cv_text(x0 + w, 1, &FONT_XS, t, LCD_LO);
        cv_rect(0, 0, 68, 14, RGB(16, 12, 30));   /* (the ticker runs behind the name) */
        cv_rect(206, 0, 34, 14, RGB(16, 12, 30));
        chroma(6, 1, &FONT_XS, "COLLAPSE", (frame_n & 15u) == 0 ? 2 : 1, C_WHITE);
        for (i = 0; i < 3; i++)
            cv_rect(214 + i * 8, 4, 6, 6, i == 2 ? MAGENTA : EDGE);
    }
}

/* the left display: where the beat is, big, split in colour; the tempo and the clock it follows; or a sweetspot just
 * reached; six little level bars */
static void draw_time(int32_t y, uint32_t now)
{
    int32_t s = eng_step(), i;
    char t[16];
    lcd_box(4, y, 118, 56);
    if (now < ui.big_until && ui.big) {
        cv_text(10, y + 5, &FONT_XS, ui.big_of, LCD_MID);
        chroma(10, y + 20, text_w(&FONT_B, ui.big) > 104 ? &FONT_XS : &FONT_B, ui.big, 1, ui.big_col);
    } else {
        int32_t bar = s >= 0 ? s / 16 % 100 + 1 : 1, beat = s >= 0 ? (s % 16) / 4 + 1 : 1;
        two_digits(bar, t);
        t[2] = '.';
        t[3] = (char)('0' + beat);
        t[4] = 0;
        if (s >= 0)
            chroma(10, y + 4, &APP_BIG_FONT, t, eng_env(I_KICK) > 0.4f ? 3 : 1, C_WHITE);
        else
            cv_text(10, y + 4, &APP_BIG_FONT, t, LCD_LO);
    }
    if (s >= 0) {
        for (i = 0; i < 5; i++)
            cv_rect(104 + i, y + 6 + i, 1, 10 - 2 * i, NEON);
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
    cv_text(10, y + 40, &FONT_XS, t, eng_ext() ? ACID : LCD_MID);
    for (i = 0; i < NINST; i++) {        /* the levels: magenta at the top */
        int32_t h = (int32_t)(fm_clampf(eng_env((int)i) * 1.6f, 0.0f, 1.0f) * 14.0f), j;
        cv_rect(76 + i * 7, y + 38, 5, 14, LCD_GRID);
        if (!(ui.muted >> i & 1u))
            for (j = 0; j < h; j++)
                cv_rect(76 + i * 7, y + 51 - j, 5, 1, mix565(NEON, MAGENTA, j * 18));
    }
}

/* the web on a liquid plasma: the plasma flows faster and brighter the harder the web moves; the masses glow */
static const int8_t HEX_X[NINST] = {0, 30, 30, 0, -30, -30}, HEX_Y[NINST] = {-20, -10, 10, 20, 10, -10};
static float plasma_t;
static void draw_web(int32_t y)
{
    int32_t cx = 180, cy = y + 26, nx[NINST], ny[NINST], i, j, topo = eng_topology(PT.chaos);
    float e = 0.0f;
    for (i = 0; i < NINST; i++)
        e += eng_energy((int)i);
    e = fm_clampf(e * 0.4f, 0.0f, 1.0f);
    plasma_t += 0.04f + 0.25f * e;
    lcd_box(124, y, 112, 56);
    for (j = 0; j < 13; j++)             /* the plasma: 4 x 4 cells, three waves */
        for (i = 0; i < 27; i++) {
            float v = fm_sinf((float)i * 0.32f + plasma_t) + fm_sinf((float)j * 0.45f - plasma_t * 1.3f)
                      + fm_sinf((float)(i + j) * 0.21f + plasma_t * 0.7f);
            int32_t k = (int32_t)((v + 3.0f) * 42.0f);
            uint16_t c = mix565(LCD_BG, k < 128 ? PURPLE : NEON, (k < 128 ? k : 255 - k) * (int32_t)(40.0f + 110.0f * e) / 128);
            cv_rect(126 + i * 4, y + 2 + j * 4, 4, 4, c);
        }
    for (j = y + 2; j < y + 54; j += 2)  /* the scanlines over it */
        cv_rect(126, j, 108, 1, LCD_BG);
    for (i = 0; i < NINST; i++) {
        float d[4];
        eng_disp((int)i, d);
        nx[i] = clamp(cx + HEX_X[i] + (int32_t)(d[0] * 0.22f + d[2] * 0.12f), 130, 229);
        ny[i] = clamp(cy + HEX_Y[i] + (int32_t)(d[1] * 0.18f + d[3] * 0.1f), y + 5, y + 50);
    }
    for (i = 0; i < NINST; i++)
        for (j = i + 1; j < NINST; j++) {
            int tie = topo == 3 || (topo == 2 && i == 0) || ((topo <= 1) && j == i + 1)
                      || (topo == 1 && i == 0 && j == NINST - 1);
            if (tie)
                cv_line(nx[i], ny[i], nx[j], ny[j], topo == 3 ? LCD_LO : LCD_MID);
        }
    for (i = 0; i < NINST; i++) {        /* the masses: a glow, a core */
        int32_t r = clamp(1 + (int32_t)(eng_env((int)i) * 4.0f), 1, 4);
        uint16_t c = (ui.muted >> i & 1u) ? LCD_LO : INST[i].rgb;
        cv_rect(nx[i] - r - 1, ny[i] - r + 1, 2 * r + 3, 2 * r - 1, dim_of(c));
        cv_rect(nx[i] - r + 1, ny[i] - r - 1, 2 * r - 1, 2 * r + 3, dim_of(c));
        cv_rect(nx[i] - r, ny[i] - r, 2 * r + 1, 2 * r + 1, c);
        if ((uint32_t)i == set.sel) {    /* the picked one: magenta brackets */
            int32_t q = r + 4;
            cv_rect(nx[i] - q, ny[i] - q, 3, 1, MAGENTA);
            cv_rect(nx[i] - q, ny[i] - q, 1, 3, MAGENTA);
            cv_rect(nx[i] + q - 2, ny[i] + q, 3, 1, MAGENTA);
            cv_rect(nx[i] + q, ny[i] + q - 2, 1, 3, MAGENTA);
        }
    }
    cv_text(130, y + 41, &FONT_XS, WEB_NAME[topo], ACID);
}

/* the six sliders: liquid light running up each groove (faster the harder it moves), a neon thumb where the web has
 * it, the level lit under it */
static void draw_sliders(int32_t y)
{
    int32_t i;
    grunge(0, y, 240, 50);
    cv_rect(0, y, 240, 1, EDGE);
    for (i = 0; i < NINST; i++) {
        int32_t x = 9 + i * 38, gx = x + 13, top = y + 4, h = 30, j;
        float en = eng_energy((int)i);
        int32_t th = top + h - 6 - (int32_t)(en * (float)(h - 6));
        int32_t lv = (int32_t)(fm_clampf(eng_env((int)i) * 1.5f, 0.0f, 1.0f) * (float)(h - 2));
        int sel = (uint32_t)i == set.sel, mute = ui.muted >> i & 1u;
        uint16_t c = INST[i].rgb;
        cv_rect(gx - 1, top - 1, 8, h + 2, BG);
        for (j = 0; j < h; j++) {        /* the liquid: light running up */
            float w = 0.5f + 0.5f * fm_sinf((float)j * 0.45f + (float)frame_n * (0.25f + en) + (float)i);
            int32_t k = mute ? 10 : (int32_t)(w * (j >= h - lv ? 220.0f : 70.0f));
            cv_rect(gx, top + j, 6, 1, mix565(BG, c, k));
        }
        panel(x + 4, th - 1, 24, 8, sel ? c : RGB(70, 66, 100), sel ? C_WHITE : EDGE);
        cv_rect(x + 8, th + 2, 16, 1, sel ? C_WHITE : BG);
        ctext(x + 16, y + 36, &FONT_XS, INST[i].tag, mute ? EDGE : sel ? C_WHITE : TXT);
        if (mute)
            cv_line(x + 6, y + 47, x + 26, y + 37, MAGENTA);
    }
}

/* the four knobs: neon bars on dark cut-corner panels, the sweetspots as ticks, the zone they are on or near */
static void draw_knobs(int32_t y, uint32_t now)
{
    int32_t k;
    uint16_t col = ui.page <= PG_ENGINE ? IN.rgb : NEON;
    grunge(0, y, 240, 44);
    for (k = 0; k < 4; k++) {
        const macro_t *mc = page_knob(k);
        int32_t x = 4 + k * 59, w = 55, v = knob_val(k), z = zone_at(mc, v), zn = zone_near(mc, v);
        int32_t fill = (v * (w - 8)) / 127, j;
        int hot = ui.knob_at[k] && now - ui.knob_at[k] < 600u;
        uint32_t i;
        char t[8];
        panel(x, y + 2, w, 40, PANEL, hot ? col : EDGE);
        cv_text(x + 5, y + 3, &FONT_XS, mc->name, hot ? C_WHITE : TXT);
        cv_rect(x + 4, y + 18, w - 8, 5, BG);
        for (j = 0; j < fill; j++)       /* the bar: a gradient, a ripple running through it */
            cv_rect(x + 4 + j, y + 18, 1, 5,
                    mix565(z >= 0 ? col : mix565(col, BG, 120), MAGENTA,
                           (j * 160) / (w - 8) + (int32_t)(30.0f * fm_sinf((float)j * 0.5f - (float)frame_n * 0.3f))));
        for (i = 0; i < mc->nzones; i++)
            cv_rect(x + 4 + (mc->centre[i] * (w - 8)) / 127, y + 15, 1, 2, (int)i == z ? C_WHITE : EDGE);
        cv_rect(x + 3 + fill, y + 16, 2, 9, C_WHITE);
        if (knob_shows_number(k)) {
            int_text(k == 0 ? v + 60 : v + 1, t);
            ctext(x + w / 2, y + 26, &FONT_XS, t, z >= 0 || hot ? C_WHITE : TXT);
        } else if (hot && z < 0) {
            int_text(v, t);
            ctext(x + w / 2, y + 26, &FONT_XS, t, C_WHITE);
        } else {
            ctext(x + w / 2, y + 26, &FONT_XS, mc->zone[z >= 0 ? z : zn], z >= 0 ? C_WHITE : TXT);
        }
    }
}

/* MASS and CHAOS: two segment meters, acid to magenta */
static void draw_macros(int32_t y)
{
    int32_t m, i;
    grunge(0, y, 240, 18);
    for (m = 0; m < 2; m++) {
        const macro_t *mc = m ? &CHAOS_KNOB : &MASS_KNOB;
        int32_t x = 4 + m * 118, v = m ? PT.chaos : PT.mass, segs = (v * 16 + 63) / 127;
        lcd_box(x, y + 1, 114, 16);
        cv_text(x + 5, y + 2, &FONT_XS, mc->name, m ? MAGENTA : ACID);
        for (i = 0; i < 16; i++)
            cv_rect(x + 46 + i * 4, y + 5, 3, 8, i < segs ? mix565(m ? NEON : ACID, MAGENTA, i * 16) : LCD_GRID);
    }
}

/* the playlist: the pattern (and the one queued), then the picked track's steps on the page shown (PERF: the pads) */
static void draw_playlist(int32_t y, int32_t s)
{
    int32_t i, j;
    char t[40];
    lcd_box(2, y, 236, 57);
    cv_rect(4, y + 2, 232, 14, RGB(40, 10, 50));
    cv_rect(4, y + 15, 232, 1, MAGENTA);
    two_digits(set.cur + 1, t);
    text_cat(t, ". ");
    text_cat(t, FACTORY[set.cur].name);
    text_cat(t, " - ");
    text_cat(t, FACTORY[set.cur].genre);
    cv_text(8, y + 3, &FONT_XS, t, C_WHITE);
    int_text(PT.bpm, t);
    cv_text(232 - text_w(&FONT_XS, t), y + 3, &FONT_XS, t, ACID);
    if (ui.browse >= 0 || eng_next_pending()) {   /* the next one */
        int32_t b = ui.browse >= 0 ? ui.browse : set.cur;
        two_digits(b + 1, t);
        text_cat(t, ". ");
        text_cat(t, FACTORY[b].name);
        cv_text(8, y + 17, &FONT_XS, t, LCD_MID);
        cv_text(232 - text_w(&FONT_XS, "NEXT"), y + 17, &FONT_XS, "NEXT", (frame_n & 8u) ? MAGENTA : NEON);
    } else {
        int32_t x;
        if (ui.perf) {                   /* the live FX keys, lit while held */
            int32_t fx = 70;
            cv_text(8, y + 17, &FONT_XS, "PERFORM", MAGENTA);
            for (i = P_GATE; i < NPADS; i++) {
                int on = ui.pads >> i & 1u;
                int32_t w = text_w(&FONT_XS, PAD_SHORT[i]) + 4;
                cv_rect(fx, y + 17, w, 12, on ? NEON : LCD_GRID);
                cv_text(fx + 2, y + 17, &FONT_XS, PAD_SHORT[i], on ? LCD_BG : LCD_MID);
                fx += w + 3;
            }
            goto steps;
        }
        x = cv_text(8, y + 17, &FONT_XS, ui.page <= PG_ENGINE ? IN.name : PAGE_NAME[ui.page],
                    ui.page <= PG_ENGINE ? IN.rgb : NEON);
        if (ui.page == PG_ENGINE)
            x = cv_text(x + 6, y + 17, &FONT_XS, "ENGINE", MAGENTA);
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
        cv_text(232 - text_w(&FONT_XS, t), y + 17, &FONT_XS, t, plat_cpu_pct() > 80u ? MAGENTA : LCD_LO);
    }
steps:
    if (ui.perf) {                       /* PERF: the sixteen pads, held ones lit */
        for (i = 0; i < 16; i++) {
            int32_t x = 6 + (i % 8) * 29, yy = y + 30 + (i / 8) * 13;
            int on = ui.pads >> i & 1u;
            cv_rect(x, yy, 27, 12, on ? (i == P_COLLAPSE ? MAGENTA : NEON) : LCD_GRID);
            ctext(x + 14, yy, &FONT_XS, PAD_SHORT[i], on ? LCD_BG : i == P_COLLAPSE ? MAGENTA : LCD_MID);
        }
        return;
    }
    for (i = 0; i < 16; i++) {           /* the steps: accents full, hits half, the others' hits as dots */
        int32_t st = i + 16 * ui.view, x = 6 + i * 14 + (i / 4) * 1, yy = y + 32, mx = x + 1;
        int on, acc;
        if (st >= len_steps()) {
            cv_rect(x, yy, 12, 13, LCD_GRID);
            cv_line(x, yy + 12, x + 11, yy, LCD_SCAN);
            continue;
        }
        on = step_on(&TR, st);
        acc = on && step_acc(&TR, st);
        cv_rect(x, yy, 12, 13, acc ? IN.rgb : on ? dim_of(IN.rgb) : LCD_GRID);
        if (acc)
            cv_rect(x + 2, yy + 2, 8, 2, C_WHITE);
        for (j = 0; j < NINST; j++)
            if ((uint32_t)j != set.sel && step_on(&PT.tr[j], st % PT.tr[j].len) && mx < x + 11) {
                cv_rect(mx, yy + 15, 2, 2, (ui.muted >> j & 1u) ? LCD_LO : INST[j].rgb);
                mx += 2;
            }
        if (st == s) {
            cv_rect(x - 1, yy - 1, 14, 1, MAGENTA);
            cv_rect(x, yy + 19, 12, 3, MAGENTA);
        } else if (i % 4 == 0) {
            cv_rect(x, yy + 19, 2, 2, LCD_LO);
        }
    }
}

static void draw(uint32_t now)
{
    int32_t s = eng_track_step(set.sel);
    frame_n++;
    tear = eng_env(I_KICK) > 0.55f ? 2 : 0;   /* a hard kick: the screen tears */
    if (ui.pads >> P_COLLAPSE & 1u)
        tear = 6;
    cv_begin(240, 120, BG);
    draw_title(now);
    grunge(0, 15, 240, 2);
    grunge(0, 15, 4, 56);
    grunge(122, 15, 2, 56);
    grunge(236, 15, 4, 56);
    grunge(0, 68, 240, 2);
    draw_time(17, now);
    draw_web(17);
    draw_sliders(70);
    tear_band();
    cv_blit(0, 0);
    cv_begin(240, 120, BG);
    draw_knobs(0, now);
    draw_macros(44);
    grunge(0, 62, 240, 1);
    draw_playlist(63, s);
    tear_band();
    cv_blit(0, 120);
    ui.drawn_at = now;
}

/* ---------------------------------------------------------- splash, intro --- */
/* chrome type, split in colour (a 90s electronics ad on a VHS tape) */
static void chrome(int32_t cx, int32_t y, const felucca_font_t *f, const char *s)
{
    int32_t x = cx - text_w(f, s) / 2;
    cv_text(x - 2, y, f, s, MAGENTA);
    cv_text(x + 2, y + 1, f, s, NEON);
    cv_text(x + 1, y + 2, f, s, RGB(40, 30, 70));
    cv_text(x, y, f, s, RGB(235, 240, 255));
}

static void draw_logo(int lit)          /* the name over a neon horizon; lit: how many of the six masses have fallen */
{
    int32_t i;
    cv_begin(240, 120, BG);
    for (i = 0; i < 120; i += 2)
        cv_rect(0, i, 240, 1, RGB(16, 12, 30));
    for (i = 0; i < 6; i++)              /* the bands behind the name */
        cv_rect(0, 40 + i * 6, 240, 2, mix565(RGB(40, 10, 60), RGB(10, 60, 80), i * 50));
    chrome(120, 44, &APP_BIG_FONT, "COLLAPSE");
    cv_rect(30, 86, 180, 1, MAGENTA);
    ctext(120, 92, &FONT_XS, "PHYSICS DRUM MACHINE", NEON);
    cv_blit(0, 0);
    cv_begin(240, 120, BG);
    for (i = 0; i < 7; i++)              /* the horizon grid */
        cv_rect(0, 50 + i * i * 2, 240, 1, mix565(PURPLE, MAGENTA, i * 40));
    for (i = -6; i <= 6; i++)
        cv_line(120 + i * 8, 50, 120 + i * 40, 119, RGB(60, 20, 90));
    for (i = 0; i < NINST; i++) {
        int on = i < lit;
        int32_t x = 120 + (on ? HEX_X[i] / 3 : HEX_X[i] * 2), yy = on ? 30 + HEX_Y[i] / 3 : 30 + HEX_Y[i];
        cv_rect(x - 3, yy - 3, 7, 7, on ? INST[i].rgb : RGB(40, 40, 70));
    }
    ctext(120, 100, &FONT_XS, "by swtdr34mz", RGB(170, 160, 210));
    cv_blit(0, 120);
}

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, BG);
    grain_init();
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
    ui.copy_to = -1;
    knobs_sync();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    eng_render(lr, n, (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f);
}

void app_overload(void) { eng_overload(); }

void app_panic(void)
{
    eng_play(0);
    eng_panic();
}

void app_ui_init(void)
{
    grain_init();
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
    else if (!strcmp(what, "length"))    /* the picked track's */
        *got = TR.len;
    else if (!strcmp(what, "perf"))
        *got = ui.perf;
    else if (!strcmp(what, "pads"))
        *got = (int)ui.pads;
    else if (!strcmp(what, "tstep"))
        *got = eng_track_step(set.sel);
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
