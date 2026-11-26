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
 *                   HOME the instrument's SOUND, EDIT its ENGINE (FM, GRAIN, FILTER, CHAOS: its bounces), ENV its
 *                   EXCITE (TYPE: mallet, pluck, bow, brush; HARDNESS, POSITION, BOUNCE), LFO its MOTION (RATE, DEPTH,
 *                   SHAPE, TARGET), SEL the WEB (TENSION, DAMPING, REACH; the instrument's LINK), FX the
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
 *   ARP             PERF mode: the white keys are 16 pads, the instruments' keys mutes, black keys 7-11 live FX, and
 *                   K1..K4 MUTATE the whole pattern live: ERODE, MORPH, ENTANGLE, RIPPLE (8 a detent; HOME: all clean)
 *   MIDI in (USB, TRS): clock, START, CONTINUE, STOP: COLLAPSE follows; notes 36-41 play the six instruments.
 *   Settings save themselves a few seconds after the last change. */
#include "fastmath.h"
#include "engine.h"

#define SET_MAGIC 0x33434F43u            /* "COC3": the settings record; another magic loads the factory patterns */
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
/* set.pat[] holds each slot's start state (factory, or what SAVE stored). What plays and gets
 * edited is a working copy: switching patterns throws the edits away and starts the new one
 * clean. Two copies, so the engine keeps reading the old one until the switch lands. */
static pattern_t live[2];
static uint8_t li;
#define PT (live[li])                    /* the pattern being edited */
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

/* what the factory patterns do with EXCITE and MOTION (255: as is) */
typedef struct {
    uint8_t pat, inst, exc[4], mot[4];
} touch_t;
static const touch_t FACTORY_TOUCH[] = {
    {0, I_PERC, {255, 255, 255, 255}, {96, 40, 10, 10}},     /* MARBLE RUN: the perc's pitch drifts over 4 bars */
    {1, I_PERC, {80, 70, 255, 255}, {255, 255, 255, 255}},   /* SPRING TANK: a bowed perc, singing */
    {1, I_KICK, {255, 30, 255, 40}, {255, 255, 255, 255}},   /* (a soft, flamming kick) */
    {3, I_BASS, {80, 64, 255, 255}, {127, 60, 10, 118}},     /* GRAVITY WELL: a bowed bass, its pitch over 16 bars */
    {4, I_HATS, {42, 110, 255, 255}, {255, 255, 255, 255}},  /* CLOCKWORK: plucked hats */
    {5, I_HATS, {116, 255, 255, 255}, {255, 255, 255, 255}}, /* RAIN ON TIN: brushed hats */
    {5, I_GLITCH, {255, 255, 255, 255}, {32, 70, 80, 36}},   /* (the ball's pitch, at random) */
    {6, I_SNARE, {255, 100, 255, 90}, {255, 255, 255, 255}}, /* FRACTURE: a hard, rattling snare */
    {7, I_KICK, {255, 255, 255, 60}, {64, 50, 116, 10}},     /* AVALANCHE: the kick bounces, its tune hops */
};

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
        tr->exc[EX_TYPE] = 10;           /* EXCITE, MOTION, LINK: as the body has it, still, tied */
        tr->exc[EX_HARD] = tr->exc[EX_POS] = 64;
        tr->mot[MO_RATE] = 64;
        tr->mot[MO_SHAPE] = 10;
        tr->mot[MO_TARGET] = 10;
        tr->link = 64;
    }
    p->web[WB_TENSION] = p->web[WB_DAMPING] = p->web[WB_REACH] = 64;
    for (k = 0; k < sizeof FACTORY_TOUCH / sizeof FACTORY_TOUCH[0]; k++) {   /* the patterns' own EXCITE, MOTION */
        const touch_t *x = &FACTORY_TOUCH[k];
        if (x->pat == i)
            for (s = 0; s < 4u; s++) {
                if (x->exc[s] != 255u)
                    p->tr[x->inst].exc[s] = x->exc[s];
                if (x->mot[s] != 255u)
                    p->tr[x->inst].mot[s] = x->mot[s];
            }
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
            || p->drive > 127u || p->space > 127u || p->drift > 127u || p->pitch > 127u || p->glitch > 127u
            || p->web[0] > 127u || p->web[1] > 127u || p->web[2] > 127u)
            return 0;
        for (t = 0; t < NINST; t++) {
            for (k = 0; k < 4u; k++)
                if (p->tr[t].p[k] > 127u || p->tr[t].fx[k] > 127u)
                    return 0;
            if (p->tr[t].level > 127u || p->tr[t].len < 1u || p->tr[t].len > 64u || p->tr[t].link > 127u)
                return 0;
            for (k = 0; k < 4u; k++)
                if (p->tr[t].exc[k] > 127u || p->tr[t].mot[k] > 127u)
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
enum { PG_SOUND, PG_ENGINE, PG_EXCITE, PG_MOTION, PG_FX, PG_GENERAL, PG_WEB, NPAGES };
static const char *const PAGE_NAME[NPAGES] = {"SOUND", "ENGINE", "EXCITE", "MOTION", "FX", "GENERAL", "WEB"};
#define INST_PAGE(p) ((p) <= PG_MOTION)   /* the picked instrument's pages (the others: the pattern's) */

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
    uint8_t perf;                        /* PERF mode (ARP): the white keys are the pads, the knobs MUTATE */
    uint8_t mut[4];                      /* MUTATE's knobs (live: not kept) */
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
    if (ui.perf)
        return &MUTATE_KNOB[k];
    switch (ui.page) {
    case PG_ENGINE: return &ENGINE_KNOB[k];
    case PG_EXCITE: return &EXCITE_KNOB[k];
    case PG_MOTION: return &MOTION_KNOB[k];
    case PG_FX: return &MASTER_KNOB[k];
    case PG_GENERAL: return &GENERAL_KNOB[k];
    case PG_WEB: return &WEB_KNOB[k];
    default: return &IN.knob[k];
    }
}
static uint8_t *page_byte(int k)
{
    switch (ui.page) {
    case PG_ENGINE: return &TR.fx[k];
    case PG_EXCITE: return &TR.exc[k];
    case PG_MOTION: return &TR.mot[k];
    case PG_WEB: return k == WB_LINK ? &TR.link : &PT.web[k];
    case PG_FX: return k == 0 ? &PT.drive : k == 1 ? &PT.space : k == 2 ? &PT.pitch : &PT.glitch;
    case PG_GENERAL: return k == 0 ? &PT.bpm : k == 1 ? &PT.swing : k == 2 ? &PT.drift : &TR.len;
    default: return &TR.p[k];
    }
}
static int32_t knob_val(int k)
{
    int32_t v;
    if (ui.perf)
        return ui.mut[k];
    v = *page_byte(k);
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
    return !ui.perf && ui.page == PG_GENERAL && (k == 0 || k == 3);
}

static void big_show(const char *what, const char *of, uint16_t col);

/* a zone's name; MOTION's TARGET: the picked instrument's own knob names */
static const char *zname(const macro_t *mc, int z)
{
    return mc->zone[z] ? mc->zone[z] : IN.knob[z].name;
}

/* MUTATE (PERF mode): 8 a detent, no rests: a click is heard, sixteen go end to end */
static void mutate(int k, int32_t e)
{
    const macro_t *mc = &MUTATE_KNOB[k];
    int32_t was = ui.mut[k], v = clamp(was + e * 8, 0, 127), zw = zone_near(mc, was), zv = zone_near(mc, v);
    static const uint16_t COL[4] = {RGB(190, 255, 40), RGB(255, 50, 190), RGB(40, 240, 255), RGB(120, 70, 255)};
    if (e > 0 && was == 0 && v < 8)
        v = 8;
    ui.mut[k] = (uint8_t)v;
    ui.knob_at[k] = plat_ms();
    eng_mutate(k, v);
    ui.redraw = 1;
    if (zv != zw || v == 0 || v == 127)
        big_show(mc->zone[zv], mc->name, COL[k]);
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

/* a knob that chooses (EXCITE's TYPE, MOTION's SHAPE and TARGET): a click is the next choice */
static int knob_chooses(int k)
{
    return (ui.page == PG_EXCITE && k == EX_TYPE) || (ui.page == PG_MOTION && (k == MO_SHAPE || k == MO_TARGET));
}

static void knob(int k, int32_t e)
{
    const macro_t *mc;
    int32_t p, v, was;
    int za;
    if (ui.perf) {
        mutate(k, e);
        return;
    }
    mc = page_knob(k);
    if (knob_chooses(k)) {
        int z = clamp(zone_near(mc, knob_val(k)) + (e > 0 ? 1 : -1), 0, mc->nzones - 1);
        if (mc->centre[z] != knob_val(k)) {
            knob_store(k, mc->centre[z]);
            ui.knob_pos[k] = pos_of_val(mc, mc->centre[z]);
            dirty();
            big_show(zname(mc, z), mc->name, IN.rgb);
        }
        ui.knob_at[k] = plat_ms();
        ui.redraw = 1;
        return;
    }
    p = clamp(ui.knob_pos[k] + accel(EN_K1 + k, e, 3, 6), 0, pos_max(mc));
    v = val_of_pos(mc, p);
    was = knob_val(k);
    za = zone_at(mc, v);
    ui.knob_pos[k] = p;
    ui.knob_at[k] = plat_ms();
    ui.redraw = 1;
    if (v == was)
        return;
    knob_store(k, v);
    dirty();
    if (za >= 0 && za != zone_at(mc, was))
        big_show(zname(mc, za), mc->name, INST_PAGE(ui.page) ? IN.rgb : RGB(93, 232, 224));
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
    app_say(INST_PAGE(pg) ? IN.name : PAGE_NAME[pg], INST_PAGE(pg) ? PAGE_NAME[pg] : pg == PG_WEB ? IN.name : "");
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

static void mutate_clean(void)
{
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        ui.mut[k] = 0;
        eng_mutate((int)k, 0);
    }
}

static void pattern_commit(void)
{
    set.cur = (uint8_t)ui.browse;
    ui.browse = -1;
    li ^= 1u;
    live[li] = set.pat[set.cur];         /* every pattern starts from its start state */
    eng_bind_next(&PT);
    mutate_clean();
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
        if (ui.perf) {                   /* PERF: MUTATE back to clean */
            mutate_clean();
            knobs_sync();
            app_say("MUTATE", "CLEAN");
            break;
        }
        ui.home_t = plat_ms();
        if (ui.page != PG_SOUND)
            page_set(PG_SOUND);
        break;
    case B_EDIT: page_set(ui.page == PG_ENGINE ? PG_SOUND : PG_ENGINE); break;
    case B_ENV: page_set(ui.page == PG_EXCITE ? PG_SOUND : PG_EXCITE); break;
    case B_LFO: page_set(ui.page == PG_MOTION ? PG_SOUND : PG_MOTION); break;
    case B_SEL: page_set(ui.page == PG_WEB ? PG_SOUND : PG_WEB); break;
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
        app_say("PERFORM", ui.perf ? "PADS + MUTATE" : "OFF");
        knobs_sync();
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
            set.pat[set.cur] = PT;       /* this becomes the pattern's start state */
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
            PT = set.pat[set.cur];
            mutate_clean();
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
        static const uint8_t PB[NPAGES] = {B_HOME, B_EDIT, B_ENV, B_LFO, B_FX, B_GLO, B_SEL};
        btn |= 1u << PB[ui.page];
    }
    dimb = (1u << B_PLAY) | (1u << B_REC) | (1u << B_SEQ) | (1u << B_FX) | (1u << B_HOME) | (1u << B_EDIT)
           | (1u << B_GLO) | (1u << B_ARP) | (1u << B_ENV) | (1u << B_LFO) | (1u << B_SEL);
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
/* The look: 90s cyberpunk, finer grained. Smooth type (Barlow) and soft edges: the rings, dots and rounded keys have
 * their edge pixels blended by how much of them the shape covers, the web's ties are anti-aliased lines (Wu). A deep
 * liquid backdrop with faint scanlines, dark cut-corner panels, neon that glows. A status bar on every page: the name,
 * the page, play, the battery. A hard hit still tears the screen for a frame. The screen is drawn in two bands of 120
 * rows, each drawn with the whole screen's coordinates (what falls outside the band is clipped). */
#define SK_BG RGB(10, 9, 20)
#define SK_PANEL RGB(14, 13, 27)
#define SK_DEEP RGB(5, 7, 15)
#define SK_EDGE RGB(52, 48, 84)
#define SK_NEON RGB(40, 240, 255)
#define SK_MAGENTA RGB(255, 50, 190)
#define SK_ACID RGB(190, 255, 40)
#define SK_PURPLE RGB(120, 70, 255)
#define SK_ORANGE RGB(255, 160, 60)
#define SK_TXT RGB(168, 164, 205)
#define SK_DIM RGB(96, 92, 134)
#define SK_TRACK RGB(36, 34, 60)
#define SK_GRID RGB(17, 25, 44)
#define SK_OFF RGB(28, 26, 48)
#define SK_BAR RGB(6, 5, 13)

static uint32_t sk_frame;                /* frames drawn (the animations' clock) */
static uint8_t sk_tearing;               /* the tear to do in this frame's bands (0: none) */

static void ctext(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

/* the smaller face when the text is wider than w */
static const felucca_font_t *sk_fit(const felucca_font_t *f, const char *s, int32_t w)
{
    return text_w(f, s) <= w ? f : &FONT_T;
}

static uint16_t sk_mix(uint16_t a, uint16_t b, int32_t t)   /* a .. b, t 0..256 */
{
    int32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    if (t <= 0)
        return a;
    if (t >= 256)
        return b;
    return (uint16_t)(((ar + (br - ar) * t / 256) << 11) | ((ag + (bg - ag) * t / 256) << 5) | (ab + (bb - ab) * t / 256));
}

static int sk_in(int32_t y, int32_t h)   /* rows y .. y+h-1 touch the band being drawn */
{
    return y + h + cv_oy > cv_y0 && y + cv_oy < cv_y1;
}

static void sk_blend(int32_t x, int32_t y, uint16_t c, int32_t a)   /* c over the pixel, a 0..256 */
{
    uint32_t i;
    y += cv_oy;
    if (a <= 0 || (uint32_t)x >= cv_w || y < cv_y0 || y >= cv_y1)
        return;
    i = (uint32_t)y * cv_w + (uint32_t)x;
    cv_px[i] = swap16(a >= 256 ? c : sk_mix(swap16(cv_px[i]), c, a));
}

/* the backdrop: a 64 x 48 tile of slow liquid waves, faint scanlines and grain, worked out once (stored as the
 * canvas holds it) */
static uint16_t sk_tile[48][64];

static float sk_atan2(float y, float x)  /* (the rings' tables, once) */
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a, s, r;
    if (ax < 1e-6f && ay < 1e-6f)
        return 0.0f;
    a = ax > ay ? ay / ax : ax / ay;
    s = a * a;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax)
        r = 1.57079637f - r;
    if (x < 0.0f)
        r = 3.14159274f - r;
    return y < 0.0f ? -r : r;
}

/* the rings (a knob's arc, 270 degrees from the lower left): for each pixel of the box, its coverage and where along
 * the arc it is (0..250; 255 the gap at the bottom). Two sizes: the pages' knobs, PERF's MUTATE */
#define RS_HALF 14
#define RL_HALF 24
#define RS_N (2 * RS_HALF + 1)
#define RL_N (2 * RL_HALF + 1)
static uint8_t rs_cov[RS_N * RS_N], rs_pos[RS_N * RS_N], rl_cov[RL_N * RL_N], rl_pos[RL_N * RL_N];
#define RS_R 11.0f
#define RL_R 20.0f

static void ring_init(uint8_t *cov, uint8_t *pos, int32_t half, float r, float w)
{
    int32_t i, j, n = 2 * half + 1;
    for (j = 0; j < n; j++)
        for (i = 0; i < n; i++) {
            float dx = (float)(i - half), dy = (float)(j - half), d = fm_sqrtf(dx * dx + dy * dy);
            float c = fm_clampf(w * 0.5f - fm_fabsf(d - r) + 0.5f, 0.0f, 1.0f);
            float a = sk_atan2(dy, dx) * 57.2957795f - 135.0f;   /* degrees, clockwise on the screen */
            while (a < 0.0f)
                a += 360.0f;
            cov[j * n + i] = (uint8_t)(c * 255.0f);
            pos[j * n + i] = a > 270.0f ? 255u : (uint8_t)(a * (250.0f / 270.0f));
        }
}

static void sk_init(void)
{
    uint32_t h = 0x9E3779B9u;
    int32_t x, y;
    for (y = 0; y < 48; y++)
        for (x = 0; x < 64; x++) {
            float fx = (float)x * (6.2831853f / 64.0f), fy = (float)y * (6.2831853f / 48.0f);
            float w = fm_sinf(fx + 1.6f * fm_sinf(fy)) * fm_cosf(fy - fx);
            int32_t n;
            uint16_t c = sk_mix(RGB(12, 10, 26), RGB(34, 17, 58), (int32_t)(fm_maxf(w, 0.0f) * 110.0f));
            h ^= h << 13;
            h ^= h >> 17;
            h ^= h << 5;
            n = (int32_t)(h & 7u);
            c = sk_mix(c, RGB(40, 36, 64), n * 4);
            if (y & 1)
                c = sk_mix(c, C_BLACK, 50);
            sk_tile[y][x] = swap16(c);
        }
    ring_init(rs_cov, rs_pos, RS_HALF, RS_R, 2.8f);
    ring_init(rl_cov, rl_pos, RL_HALF, RL_R, 4.0f);
}

static void sk_backdrop(void)
{
    uint32_t r, x;
    for (r = 0; r < cv_h; r++) {
        const uint16_t *t = sk_tile[(uint32_t)((int32_t)r - cv_oy) % 48u];
        uint16_t *p = cv_px + r * cv_w;
        for (x = 0; x < cv_w; x++)
            p[x] = t[x & 63u];
    }
}

/* a panel with cut corners: dark, a thin edge */
static void sk_panel(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t fill, uint16_t edge)
{
    int32_t c = 6, k;
    if (!sk_in(y, h))
        return;
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
    for (k = 0; k <= c; k++) {           /* the cuts: 45 degrees, their outer side softened */
        cv_pset(x + k, y + c - k, edge);
        cv_pset(x + w - 1 - k, y + c - k, edge);
        cv_pset(x + k, y + h - 1 - c + k, edge);
        cv_pset(x + w - 1 - k, y + h - 1 - c + k, edge);
        if (k < c) {
            sk_blend(x + k, y + c - k - 1, edge, 70);
            sk_blend(x + w - 1 - k, y + c - k - 1, edge, 70);
            sk_blend(x + k, y + h - c + k, edge, 70);
            sk_blend(x + w - 1 - k, y + h - c + k, edge, 70);
        }
    }
}

/* a rounded rectangle, its corners anti-aliased; SK_HOLLOW: the edge alone */
#define SK_HOLLOW 1
static void sk_rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t fill, uint16_t edge, int flags)
{
    int32_t i, j;
    float fr = (float)r;
    if (w <= 0 || h <= 0 || !sk_in(y, h))
        return;
    if (r * 2 > w)
        r = w / 2, fr = (float)r;
    if (r * 2 > h)
        r = h / 2, fr = (float)r;
    for (j = 0; j < h; j++) {
        int cy = j < r || j >= h - r;
        if (!sk_in(y + j, 1))
            continue;
        for (i = 0; i < w; i++) {
            int cx = i < r || i >= w - r, border = i == 0 || j == 0 || i == w - 1 || j == h - 1;
            if (cx && cy) {              /* a corner: coverage by the distance from its centre */
                float ox = (float)(i < r ? r : w - r) - ((float)i + 0.5f);
                float oy = (float)(j < r ? r : h - r) - ((float)j + 0.5f);
                float d = fm_sqrtf(ox * ox + oy * oy);
                float out = fm_clampf(fr - d + 0.5f, 0.0f, 1.0f), in = fm_clampf(fr - 1.0f - d + 0.5f, 0.0f, 1.0f);
                if (out <= 0.0f)
                    continue;
                if (flags & SK_HOLLOW) {
                    sk_blend(x + i, y + j, edge, (int32_t)((out - in) * 256.0f));
                } else {
                    sk_blend(x + i, y + j, sk_mix(edge, fill, (int32_t)(in * 256.0f)), (int32_t)(out * 256.0f));
                }
            } else if (border) {
                cv_pset(x + i, y + j, edge);
            } else if (!(flags & SK_HOLLOW)) {
                cv_pset(x + i, y + j, fill);
            }
        }
    }
}

/* a dot (thick 0) or a circle line (thick its width), soft-edged, at alpha a (0..256) */
static void sk_disc(float cx, float cy, float r, float thick, uint16_t c, int32_t a)
{
    int32_t x0 = (int32_t)(cx - r - thick - 1.0f), x1 = (int32_t)(cx + r + thick + 2.0f);
    int32_t y0 = (int32_t)(cy - r - thick - 1.0f), y1 = (int32_t)(cy + r + thick + 2.0f), x, y;
    if (!sk_in(y0, y1 - y0))
        return;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = fm_sqrtf(dx * dx + dy * dy), cov;
            cov = thick > 0.0f ? thick * 0.5f - fm_fabsf(d - r) + 0.5f : r - d + 0.5f;
            if (cov > 0.0f)
                sk_blend(x, y, c, (int32_t)(fm_minf(cov, 1.0f) * (float)a));
        }
}

/* an anti-aliased line (Wu), at alpha a */
static void sk_line(float x0, float y0, float x1, float y1, uint16_t c, int32_t a)
{
    int steep = fm_fabsf(y1 - y0) > fm_fabsf(x1 - x0);
    float t, g;
    int32_t x, xe;
    if (steep) {
        t = x0, x0 = y0, y0 = t;
        t = x1, x1 = y1, y1 = t;
    }
    if (x0 > x1) {
        t = x0, x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
    }
    g = x1 - x0 < 0.5f ? 0.0f : (y1 - y0) / (x1 - x0);
    xe = (int32_t)(x1 + 0.5f);
    for (x = (int32_t)(x0 + 0.5f); x <= xe; x++) {
        float yy = y0 + g * ((float)x - x0), f;
        int32_t yi = (int32_t)fm_floorf(yy);
        f = yy - (float)yi;
        if (steep) {
            sk_blend(yi, x, c, (int32_t)((1.0f - f) * (float)a));
            sk_blend(yi + 1, x, c, (int32_t)(f * (float)a));
        } else {
            sk_blend(x, yi, c, (int32_t)((1.0f - f) * (float)a));
            sk_blend(x, yi + 1, c, (int32_t)(f * (float)a));
        }
    }
}

/* a knob's ring: the arc lit to v (0..127) in c over the track, a white bead at its end; big: PERF's size */
static void sk_ring(int32_t cx, int32_t cy, int big, int32_t v, uint16_t c)
{
    const uint8_t *cov = big ? rl_cov : rs_cov, *pos = big ? rl_pos : rs_pos;
    int32_t half = big ? RL_HALF : RS_HALF, n = 2 * half + 1, i, j, lit = v * 250 / 127;
    float r = big ? RL_R : RS_R, ang = (135.0f + 270.0f * (float)v / 127.0f) * 0.0174532925f;
    if (!sk_in(cy - half, n))
        return;
    for (j = 0; j < n; j++)
        for (i = 0; i < n; i++) {
            uint32_t k = (uint32_t)(j * n + i);
            if (cov[k] && pos[k] != 255u)
                sk_blend(cx - half + i, cy - half + j, (int32_t)pos[k] <= lit && v > 0 ? c : SK_TRACK, cov[k] + 1);
        }
    sk_disc((float)cx + 0.5f + fm_cosf(ang) * r, (float)cy + 0.5f + fm_sinf(ang) * r, big ? 2.8f : 2.1f, 0.0f, C_WHITE, 256);
}

/* text with a soft glow of colour g around it */
static void sk_glow_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t g)
{
    uint16_t d = sk_mix(SK_DEEP, g, 120);
    if (!sk_in(y - 1, f->h + 2))
        return;
    cv_text(x - 1, y, f, s, d);
    cv_text(x + 1, y, f, s, d);
    cv_text(x, y - 1, f, s, d);
    cv_text(x, y + 1, f, s, d);
    cv_text(x, y, f, s, c);
}

/* the tear: a few rows of the band just drawn, moved sideways (a hard hit) */
static void sk_tear(void)
{
    uint32_t h = 0x2545F491u ^ (sk_frame * 2654435761u) ^ (uint32_t)cv_oy, r, k;
    if (!sk_tearing)
        return;
    for (k = 0; k < sk_tearing; k++) {
        uint32_t y0, rows, row;
        int32_t sh;
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        y0 = h % cv_h;
        rows = 1u + (h >> 8) % 4u;
        sh = (int32_t)((h >> 16) % 9u) - 4;
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

/* ---- the status bar, on every page: the name (or a message), the page, play, the battery */
static const uint8_t BOLT[9] = {0x0C, 0x18, 0x30, 0x7E, 0x0C, 0x18, 0x30, 0x20, 0x00};   /* 7 wide, a row a byte */

static void draw_battery(int32_t x, int32_t y)
{
    int b = plat_battery(), ch = plat_battery_charging(), cells, i;
    static const uint16_t COL[4] = {SK_MAGENTA, SK_ORANGE, SK_ACID, SK_ACID};
    uint16_t c;
    sk_rrect(x, y, 23, 11, 3, SK_BAR, SK_TXT, 0);
    cv_rect(x + 23, y + 3, 2, 5, SK_TXT);
    if (b < 0)                           /* not read yet: the outline alone */
        return;
    cells = b >= 4 ? 4 : b + 1;
    c = b >= 4 ? SK_NEON : COL[b];
    if (ch == 2)                         /* charging: the cells fill up, over and over */
        cells = 1 + (int)((sk_frame / 6u) % 4u), c = SK_ACID;
    for (i = 0; i < 4; i++) {
        uint16_t cc = i < cells ? c : SK_OFF;
        if (b == 0 && ch != 2 && (sk_frame & 16u))   /* empty: the last cell blinks */
            cc = SK_OFF;
        sk_rrect(x + 3 + i * 5, y + 3, 4, 5, 1, cc, cc, 0);
    }
    if (ch == 2 || b >= 4) {             /* the bolt: 5 V in */
        int32_t bx = x + 8, by = y + 1, r, k;
        for (r = 0; r < 9; r++)
            for (k = 0; k < 7; k++)
                if (BOLT[r] >> (6 - k) & 1u) {
                    cv_pset(bx + k - 1, by + r, SK_BAR);
                    cv_pset(bx + k + 1, by + r, SK_BAR);
                }
        for (r = 0; r < 9; r++)
            for (k = 0; k < 7; k++)
                if (BOLT[r] >> (6 - k) & 1u)
                    cv_pset(bx + k, by + r, C_WHITE);
    }
}

static void draw_status(uint32_t now)
{
    int32_t x;
    if (!sk_in(0, 18))
        return;
    cv_rect(0, 0, 240, 17, SK_BAR);
    cv_rect(0, 17, 240, 1, sk_mix(SK_BAR, SK_NEON, 80));
    cv_rect(0, 17, 66, 1, SK_MAGENTA);
    if (now < ui.msg_until) {            /* a message: in the name's place */
        const felucca_font_t *f = text_w(&FONT_XS, ui.msg[0]) + text_w(&FONT_XS, ui.msg[1]) > 158 ? &FONT_T : &FONT_XS;
        x = cv_text(5, 3, f, ui.msg[0], C_WHITE);
        cv_text(x + 5, 3, f, ui.msg[1], SK_NEON);
    } else {
        sk_glow_text(5, 1, &FONT_B, "COLLAPSE", C_WHITE, SK_MAGENTA);
        cv_text(84, 3, &FONT_XS, ui.perf ? "PERFORM" : PAGE_NAME[ui.page], SK_NEON);
    }
    if (eng_ext())
        cv_text(196 - text_w(&FONT_T, "EXT") - 12, 4, &FONT_T, "EXT", SK_ACID);
    if (eng_playing()) {                 /* play: a soft triangle */
        for (x = 0; x < 8; x++) {
            int32_t hh = 10 - (x * 10) / 8;
            cv_rect(186 + x, 9 - hh / 2, 1, hh, SK_ACID);
        }
    } else {
        sk_rrect(186, 5, 7, 7, 1, SK_DIM, SK_DIM, 0);
    }
    draw_battery(208, 3);
}

/* ---- the hero: the pattern big, its name, the tempo and where the bar is (or a sweetspot just reached) */
static void draw_hero(uint32_t now)
{
    int32_t s = eng_step(), x, i;
    int next = ui.browse >= 0 || eng_next_pending(), pat = ui.browse >= 0 ? ui.browse : set.cur;
    const char *name = FACTORY[pat].name;
    char t[24];
    if (!sk_in(22, 72))
        return;
    sk_panel(4, 22, 116, 72, SK_DEEP, SK_EDGE);
    cv_rect(10, 92, 50, 1, next ? SK_MAGENTA : SK_NEON);
    if (now < ui.big_until && ui.big) {  /* a sweetspot: its name big */
        const felucca_font_t *f = text_w(&FONT_M, ui.big) <= 104 ? &FONT_M : &FONT_B;
        cv_text(11, 26, &FONT_T, ui.big_of, SK_DIM);
        sk_glow_text(11, 38, sk_fit(f, ui.big, 104), ui.big, C_WHITE, ui.big_col);
    } else {
        int blink = next && (sk_frame & 8u);
        two_digits(pat + 1, t);
        sk_glow_text(10, 22, &FONT_L, t, blink ? SK_MAGENTA : RGB(235, 255, 255), next ? SK_MAGENTA : SK_NEON);
        for (i = 0; name[i] && name[i] != ' '; i++)   /* the name over two lines, split at its first space */
            t[i] = name[i];
        t[i] = 0;
        cv_text(52, 29, sk_fit(&FONT_B, t, 64), t, next ? SK_MAGENTA : SK_NEON);
        if (name[i])
            cv_text(52, 43, sk_fit(&FONT_B, name + i + 1, 64), name + i + 1, next ? SK_MAGENTA : SK_NEON);
    }
    if (eng_ext()) {                     /* the tempo, or the clock it follows */
        x = cv_text(10, 64, &FONT_M, "EXT", SK_ACID);
        cv_text(x + 3, 73, &FONT_T, "SYNC", SK_DIM);
    } else {
        int_text(PT.bpm, t);
        x = cv_text(10, 64, &FONT_M, t, C_WHITE);
        cv_text(x + 3, 73, &FONT_T, "BPM", SK_DIM);
    }
    cv_text(74, 64, &FONT_T, "SW", SK_DIM);
    int_text(PT.swing, t);
    cv_text(116 - text_w(&FONT_XS, t), 63, &FONT_XS, t, SK_TXT);
    cv_text(74, 77, &FONT_T, "BAR", SK_DIM);
    {
        int32_t bar = s >= 0 ? s / 16 % 100 + 1 : 1, beat = s >= 0 ? (s % 16) / 4 + 1 : 1;
        two_digits(bar, t);
        t[2] = '.';
        t[3] = (char)('0' + beat);
        t[4] = 0;
        cv_text(116 - text_w(&FONT_XS, t), 76, &FONT_XS, t, s >= 0 ? SK_NEON : SK_DIM);
    }
}

/* ---- the web: a fine grid that brightens as it moves, the ties anti-aliased, the masses glowing */
static const int8_t HEX_X[NINST] = {0, 30, 30, 0, -30, -30}, HEX_Y[NINST] = {-20, -10, 10, 20, 10, -10};
static void draw_web(void)
{
    int32_t cx = 180, cy = 56, i, j, topo = eng_topology(PT.chaos);
    float nx[NINST], ny[NINST], e = 0.0f;
    uint16_t grid;
    if (!sk_in(22, 72))
        return;
    for (i = 0; i < NINST; i++)
        e += eng_energy((int)i);
    e = fm_clampf(e * 0.4f, 0.0f, 1.0f);
    sk_panel(124, 22, 112, 72, SK_DEEP, SK_EDGE);
    grid = sk_mix(SK_DEEP, sk_mix(SK_GRID, SK_PURPLE, (int32_t)(e * 120.0f)), 200);
    for (i = 132; i < 234; i += 12)
        cv_rect(i, 24, 1, 68, grid);
    for (j = 30; j < 92; j += 12)
        cv_rect(126, j, 108, 1, grid);
    for (i = 0; i < NINST; i++) {
        float d[4];
        eng_disp((int)i, d);
        nx[i] = fm_clampf((float)(cx + HEX_X[i] * 5 / 4) + d[0] * 0.27f + d[2] * 0.15f, 132.0f, 228.0f);
        ny[i] = fm_clampf((float)(cy + HEX_Y[i] * 5 / 4) + d[1] * 0.22f + d[3] * 0.12f, 30.0f, 80.0f);
    }
    for (i = 0; i < NINST; i++)
        for (j = i + 1; j < NINST; j++) {
            int tie = topo == 3 || (topo == 2 && i == 0) || ((topo <= 1) && j == i + 1)
                      || (topo == 1 && i == 0 && j == NINST - 1);
            if (tie)
                sk_line(nx[i], ny[i], nx[j], ny[j], sk_mix(INST[i].rgb, INST[j].rgb, 128), topo == 3 ? 110 : 210);
        }
    for (i = 0; i < NINST; i++) {        /* the masses: a glow, a core; the picked one ringed */
        float en = fm_clampf(eng_env((int)i), 0.0f, 1.0f);
        uint16_t c = (ui.muted >> i & 1u) ? SK_DIM : INST[i].rgb;
        sk_disc(nx[i], ny[i], 3.5f + en * 4.0f, 0.0f, c, 60 + (int32_t)(en * 60.0f));
        sk_disc(nx[i], ny[i], 1.8f + en * 1.4f, 0.0f, c, 256);
        if ((uint32_t)i == set.sel)
            sk_disc(nx[i], ny[i], 6.5f + en * 2.0f, 1.0f, C_WHITE, 220);
    }
    cv_text(131, 79, &FONT_T, WEB_NAME[topo], SK_ACID);
}

/* ---- the six instruments: rounded keys, the picked one tinted, a level bar each */
static void draw_insts(void)
{
    int32_t i;
    if (!sk_in(98, 26))
        return;
    for (i = 0; i < NINST; i++) {
        int32_t x = 4 + i * 39;
        int sel = (uint32_t)i == set.sel, mute = ui.muted >> i & 1u;
        float en = fm_clampf(eng_env((int)i) * 1.5f, 0.0f, 1.0f);
        uint16_t c = INST[i].rgb;
        sk_rrect(x, 98, 36, 26, 4, sel ? sk_mix(SK_PANEL, c, 64) : SK_PANEL,
                 sel ? c : sk_mix(SK_EDGE, c, mute ? 0 : (int32_t)(en * 160.0f)), 0);
        ctext(x + 18, 102, &FONT_T, INST[i].name, mute ? SK_DIM : sel ? C_WHITE : SK_TXT);
        if (mute)
            sk_line((float)x + 6.0f, 108.5f, (float)x + 30.0f, 108.5f, SK_MAGENTA, 256);
        else if (en > 0.02f)
            sk_rrect(x + 5, 117, 2 + (int32_t)(en * 24.0f), 3, 1, c, c, 0);
    }
}

/* ---- the four knobs: rings, their sweetspots as beads around them, the value or the zone under them */
static void draw_knobs(uint32_t now)
{
    int32_t k;
    uint16_t col = INST_PAGE(ui.page) ? IN.rgb : SK_NEON;
    if (!sk_in(128, 50))
        return;
    for (k = 0; k < 4; k++) {
        const macro_t *mc = page_knob(k);
        int32_t x = 4 + k * 58, v = knob_val(k), z = zone_at(mc, v), zn = zone_near(mc, v), cx = x + 28, cy = 155;
        int hot = ui.knob_at[k] && now - ui.knob_at[k] < 600u;
        uint32_t i;
        const char *val;
        char t[8];
        sk_panel(x, 128, 56, 50, SK_PANEL, hot ? col : SK_EDGE);
        ctext(cx, 130, &FONT_T, mc->name, hot ? C_WHITE : SK_DIM);
        for (i = 0; i < mc->nzones; i++) {   /* the sweetspots: beads just outside the ring */
            float a = (135.0f + 270.0f * (float)mc->centre[i] / 127.0f) * 0.0174532925f;
            sk_disc((float)cx + 0.5f + fm_cosf(a) * 14.5f, (float)cy + 0.5f + fm_sinf(a) * 14.5f, 1.0f, 0.0f,
                    (int)i == z ? C_WHITE : SK_EDGE, 256);
        }
        sk_ring(cx, cy, 0, v, z >= 0 ? col : sk_mix(col, SK_PANEL, 70));
        int_text(knob_shows_number(k) ? (k == 0 ? v + 60 : v + 1) : v, t);
        ctext(cx + 1, cy - 6, &FONT_T, t, hot ? C_WHITE : SK_TXT);
        if (knob_shows_number(k) || (hot && z < 0))
            val = t;
        else
            val = zname(mc, z >= 0 ? z : zn);
        ctext(cx, 165, sk_fit(&FONT_XS, val, 52), val, z >= 0 || hot ? C_WHITE : SK_TXT);
    }
}

/* ---- MASS and CHAOS: two sliders */
static void draw_macros(void)
{
    int32_t m;
    if (!sk_in(178, 14))
        return;
    for (m = 0; m < 2; m++) {
        int32_t x = 4 + m * 118, v = m ? PT.chaos : PT.mass, w = (v * 70) / 127;
        uint16_t c = m ? SK_MAGENTA : SK_NEON;
        cv_text(x + 2, 179, &FONT_XS, m ? "CHAOS" : "MASS", SK_TXT);
        sk_rrect(x + 42, 182, 72, 5, 2, SK_TRACK, SK_TRACK, 0);
        sk_rrect(x + 42, 182, w + 2, 5, 2, c, c, 0);
        sk_disc((float)(x + 43 + w), 184.5f, 4.5f, 0.0f, c, 90);
        sk_disc((float)(x + 43 + w), 184.5f, 3.2f, 0.0f, C_WHITE, 256);
    }
}

/* ---- the steps of the picked track on the page shown: accents tall, hits short, the others' hits as dots */
static void draw_steps(int32_t s)
{
    int32_t i, j, x;
    char t[24];
    if (!sk_in(194, 42))
        return;
    sk_panel(4, 194, 232, 42, SK_DEEP, SK_EDGE);
    x = cv_text(10, 196, &FONT_T, INST_PAGE(ui.page) ? IN.name : PAGE_NAME[ui.page], INST_PAGE(ui.page) ? IN.rgb : SK_NEON);
    if (INST_PAGE(ui.page) && ui.page != PG_SOUND)
        x = cv_text(x + 5, 196, &FONT_T, PAGE_NAME[ui.page], SK_MAGENTA);
    if (ui.page == PG_WEB)
        x = cv_text(x + 5, 196, &FONT_T, IN.name, IN.rgb);
    t[0] = 0;
    int_text(ui.view * 16 + 1, t);
    text_cat(t, "-");
    int_text(ui.view * 16 + 16, t + text_len(t));
    text_cat(t, " / ");
    int_text(len_steps(), t + text_len(t));
    cv_text(x + 6, 196, &FONT_T, t, SK_TXT);
    t[0] = 0;
    text_cat(t, "CPU ");
    int_text((int32_t)plat_cpu_pct(), t + 4);
    text_cat(t, "%");
    cv_text(230 - text_w(&FONT_T, t), 196, &FONT_T, t, plat_cpu_pct() > 80u ? SK_MAGENTA : SK_DIM);
    for (i = 1; i < 4; i++)
        cv_rect(7 + i * 56, 210, 1, 17, SK_EDGE);
    for (i = 0; i < 16; i++) {
        int32_t st = i + 16 * ui.view, sx = 9 + i * 14, mx = sx + 1;
        int on, acc;
        if (st >= len_steps()) {         /* past the track's length */
            sk_rrect(sx, 215, 11, 12, 2, SK_DEEP, SK_OFF, 0);
            continue;
        }
        on = step_on(&TR, st);
        acc = on && step_acc(&TR, st);
        if (acc)
            sk_rrect(sx, 209, 11, 18, 2, IN.rgb, IN.rgb, 0);
        else
            sk_rrect(sx, 215, 11, 12, 2, on ? sk_mix(SK_OFF, IN.rgb, 190) : SK_OFF, on ? sk_mix(SK_OFF, IN.rgb, 190) : SK_OFF, 0);
        for (j = 0; j < NINST; j++)
            if ((uint32_t)j != set.sel && step_on(&PT.tr[j], st % PT.tr[j].len) && mx < sx + 10) {
                cv_rect(mx, 229, 2, 2, (ui.muted >> j & 1u) ? SK_DIM : INST[j].rgb);
                mx += 2;
            }
        if (st == s)
            sk_rrect(sx - 2, 206, 15, 24, 3, 0, C_WHITE, SK_HOLLOW);
    }
}

/* ---- PERF: the sixteen pads, MUTATE's four rings, the live keys and the pattern */
static void draw_perf(void)
{
    static const uint16_t MCOL[4] = {SK_ACID, SK_MAGENTA, SK_NEON, SK_PURPLE};
    int32_t i, x;
    char t[24];
    if (sk_in(22, 98))
        for (i = 0; i < 16; i++) {
            int32_t px = 4 + (i % 4) * 58, py = 22 + (i / 4) * 25;
            int on = ui.pads >> i & 1u;
            uint16_t c = i == P_COLLAPSE ? SK_MAGENTA : SK_NEON;
            const char *n = text_w(&FONT_T, PAD_NAME[i]) <= 52 ? PAD_NAME[i] : PAD_SHORT[i];
            sk_rrect(px, py, 56, 22, 4, on ? sk_mix(SK_PANEL, c, 90) : SK_PANEL, on ? c : SK_EDGE, 0);
            ctext(px + 28, py + 4, sk_fit(&FONT_XS, n, 52), n, on ? C_WHITE : i == P_COLLAPSE ? SK_MAGENTA : SK_TXT);
        }
    if (sk_in(124, 72))
        for (i = 0; i < 4; i++) {
            const macro_t *mc = &MUTATE_KNOB[i];
            int32_t px = 4 + i * 58, v = ui.mut[i], z = zone_at(mc, v), zn = zone_near(mc, v);
            const char *zn_s = zname(mc, z >= 0 ? z : zn);
            sk_panel(px, 124, 56, 72, SK_PANEL, v ? MCOL[i] : SK_EDGE);
            ctext(px + 28, 127, &FONT_T, mc->name, v ? C_WHITE : SK_DIM);
            sk_ring(px + 28, 159, 1, v, MCOL[i]);
            int_text(v, t);
            ctext(px + 29, 151, &FONT_B, t, C_WHITE);
            ctext(px + 28, 180, sk_fit(&FONT_XS, zn_s, 52), zn_s, v ? C_WHITE : SK_TXT);
        }
    if (!sk_in(200, 36))
        return;
    sk_panel(4, 200, 232, 36, SK_DEEP, SK_EDGE);
    x = 10;
    for (i = P_GATE; i < NPADS; i++) {   /* the live keys, lit while held */
        int on = ui.pads >> i & 1u;
        int32_t w = text_w(&FONT_T, PAD_SHORT[i]) + 10;
        sk_rrect(x, 204, w, 13, 3, on ? SK_NEON : SK_OFF, on ? SK_NEON : SK_EDGE, 0);
        cv_text(x + 5, 204, &FONT_T, PAD_SHORT[i], on ? SK_DEEP : SK_TXT);
        x += w + 4;
    }
    {
        int next = ui.browse >= 0 || eng_next_pending(), pat = ui.browse >= 0 ? ui.browse : set.cur;
        two_digits(pat + 1, t);
        text_cat(t, " ");
        text_cat(t, FACTORY[pat].name);
        x = cv_text(10, 219, &FONT_XS, t, next ? SK_MAGENTA : C_WHITE);
        if (next && (sk_frame & 8u))
            cv_text(x + 6, 220, &FONT_T, "NEXT", SK_MAGENTA);
        int_text(PT.bpm, t);
        text_cat(t, " BPM");
        cv_text(230 - text_w(&FONT_XS, t), 219, &FONT_XS, t, SK_ACID);
    }
}

static void draw(uint32_t now)
{
    int32_t s = eng_track_step(set.sel), band;
    sk_frame++;
    sk_tearing = eng_env(I_KICK) > 0.6f ? 2 : 0;   /* a hard kick: the screen tears */
    if (ui.pads >> P_COLLAPSE & 1u)
        sk_tearing = 5;
    for (band = 0; band < 2; band++) {
        cv_begin(240, 120, SK_BG);
        cv_oy = -120 * band;             /* the whole screen's coordinates; this band's rows kept */
        sk_backdrop();
        draw_status(now);
        if (ui.perf) {
            draw_perf();
        } else {
            draw_hero(now);
            draw_web();
            draw_insts();
            draw_knobs(now);
            draw_macros();
            draw_steps(s);
        }
        sk_tear();
        cv_blit(0, (uint32_t)(120 * band));
    }
    ui.drawn_at = now;
}

/* ---------------------------------------------------------- splash, intro --- */
/* chrome type, split in colour (a 90s electronics ad on a VHS tape) */
static void chrome(int32_t cx, int32_t y, const felucca_font_t *f, const char *s)
{
    int32_t x = cx - text_w(f, s) / 2;
    cv_text(x - 2, y, f, s, SK_MAGENTA);
    cv_text(x + 2, y + 1, f, s, SK_NEON);
    cv_text(x + 1, y + 2, f, s, RGB(40, 30, 70));
    cv_text(x, y, f, s, RGB(235, 240, 255));
}

static void draw_logo(int lit)          /* the name over a neon horizon; lit: how many of the six masses have fallen */
{
    int32_t i;
    cv_begin(240, 120, SK_BG);
    for (i = 0; i < 120; i += 2)
        cv_rect(0, i, 240, 1, RGB(16, 12, 30));
    for (i = 0; i < 6; i++)              /* the bands behind the name */
        cv_rect(0, 40 + i * 6, 240, 2, sk_mix(RGB(40, 10, 60), RGB(10, 60, 80), i * 50));
    chrome(120, 44, &APP_BIG_FONT, "COLLAPSE");
    cv_rect(30, 86, 180, 1, SK_MAGENTA);
    ctext(120, 92, &FONT_XS, "PHYSICS DRUM MACHINE", SK_NEON);
    cv_blit(0, 0);
    cv_begin(240, 120, SK_BG);
    for (i = 0; i < 7; i++)              /* the horizon grid */
        cv_rect(0, 50 + i * i * 2, 240, 1, sk_mix(SK_PURPLE, SK_MAGENTA, i * 40));
    for (i = -6; i <= 6; i++)
        cv_line(120 + i * 8, 50, 120 + i * 40, 119, RGB(60, 20, 90));
    for (i = 0; i < NINST; i++) {
        int on = i < lit;
        int32_t x = 120 + (on ? HEX_X[i] / 3 : HEX_X[i] * 2), yy = on ? 30 + HEX_Y[i] / 3 : 30 + HEX_Y[i];
        if (on)
            sk_disc((float)x + 0.5f, (float)yy + 0.5f, 7.0f, 0.0f, INST[i].rgb, 70);
        sk_disc((float)x + 0.5f, (float)yy + 0.5f, 3.5f, 0.0f, on ? INST[i].rgb : RGB(40, 40, 70), 256);
    }
    ctext(120, 100, &FONT_XS, "by swtdr34mz", RGB(170, 160, 210));
    cv_blit(0, 120);
}

void app_splash(void)
{
    lcd_fill(0, 0, 240, 240, SK_BG);
    sk_init();
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
    li = 0;
    live[0] = set.pat[set.cur];
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
    sk_init();
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
    li = 0;
    live[0] = set.pat[set.cur];
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
    else if (!strncmp(what, "mut", 3) && what[3] >= '1' && what[3] <= '4')
        *got = ui.mut[what[3] - '1'];
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
