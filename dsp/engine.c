/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE's sound engine (engine.h): the instruments, the web, the sequencer, the master. No libc here: the device
 * builds this unit on its own. */
#include "engine.h"
#include "fastmath.h"
#include "plat.h"

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#include "tables.c"

#include "helpers.h"

#include "body.c"

#include "web.c"

static void body_hit(int t, const float *p, float vel, uint32_t *ns);
static void later(int t, float wait, float vel);

/* =============================================================== the state === */
static const pattern_t *PAT;
static volatile const pattern_t *pat_next;
static pattern_t pat0;                   /* (until the app binds its own) */

typedef struct {                         /* an instrument: its body, the block's peak (the screen's), its noise */
    union {
        kick_body_t k;
        snare_body_t s;
        hats_body_t h;
        glitch_body_t g;
        perc_body_t p;
        bass_body_t b;
    } body;
    float peak;
    uint32_t noise;
    uint16_t quiet;                      /* blocks it has been silent: past SLEEP_BLOCKS it sleeps (not computed) */
} inst_state_t;
#define SLEEP_BLOCKS 30u                 /* 87 ms: longer than any effect's memory (GRAIN keeps 46 ms) */
#define QUIET 3e-5f                      /* -90 dB */
static inst_state_t S[NINST];
static volatile uint8_t muted;

/* the hits of a block: when, how hard, with which knobs (worked out where they are queued, on this core) */
#define NEV 8
typedef struct {
    uint16_t at;
    float vel, p[4];
    float semi;                          /* MOTION's PITCH: this hit tuned up or down (semitones) */
} ev_t;
static ev_t ev[NINST][NEV];
static uint8_t nev[NINST];

/* ------------------------------------------------------------ the sequencer --- */
static volatile int8_t play_req = -1;    /* -1 nothing asked, 0 stop, 1 start */
static volatile uint8_t hit_req, slam_req;
static volatile float hit_vel[NINST];
static volatile int32_t step_now = -1;
static volatile uint32_t hits;
static volatile float env_out[NINST], energy_out[NINST], disp_out[NINST][4];
static float step_left;                  /* samples to the next step */
static int32_t step_pos;                 /* the next step to play */
static int8_t running;                   /* the internal clock runs */
/* MIDI clock in */
static volatile uint8_t ext_clocks;      /* clocks arrived, not yet counted */
static volatile int8_t ext_req = -1;     /* -1 nothing, 0 stop, 1 start, 2 continue */
static volatile float ext_bpm;
static int8_t ext_run;
static uint32_t ext_count;               /* clocks since START: a step every 6 */
static uint32_t ext_quiet = 0xFFFFFFFFu; /* samples since the last clock */
static float gain_now;

void eng_bind(const pattern_t *p)
{
    uint32_t f = plat_lock();
    PAT = p;
    pat_next = 0;
    plat_unlock(f);
}
void eng_bind_next(const pattern_t *p)
{
    if (step_now < 0) {
        eng_bind(p);
        return;
    }
    pat_next = p;
}
int eng_next_pending(void) { return pat_next != 0; }
void eng_mute(int inst, int on)
{
    uint32_t f = plat_lock();
    if (on)
        muted |= (uint8_t)(1u << inst);
    else
        muted &= (uint8_t)~(1u << inst);
    plat_unlock(f);
}
void eng_play(int on) { play_req = (int8_t)(on ? 1 : 0); }
void eng_ext_start(int cont)             /* (the tick's: the clocks after it count from here) */
{
    ext_clocks = 0;
    ext_req = (int8_t)(cont ? 2 : 1);
}
void eng_ext_stop(void) { ext_req = 0; }
void eng_ext_clock(void)
{
    if (ext_clocks < 250u)
        ext_clocks++;
}
void eng_ext_bpm(float bpm) { ext_bpm = bpm; }
int eng_ext(void) { return ext_quiet < (uint32_t)(ENG_SR * 0.5f); }
void eng_hit(int inst, float vel)
{
    uint32_t f;
    if (inst < 0 || inst >= NINST)
        return;
    f = plat_lock();
    hit_vel[inst] = vel;
    hit_req |= (uint8_t)(1u << inst);
    plat_unlock(f);
}
void eng_kick_web(void) { slam_req = 1; }
/* ------------------------------------------------------------- the pads --- */
static volatile uint32_t pads;           /* held now (the main loop's) */
static uint32_t pads_was;
static float g_mass = 64.0f, g_chaos = 64.0f, g_drift, g_dil = 1.0f, g_fxchaos, g_couple_x = 1.0f;   /* the pattern's,
                                          * as the pads move them now (smoothed) */
static int32_t rep_start = -1, rep_len, half_start = -1;
static uint8_t rep_twice;                /* REPEAT 1/32: each hit twice */
static float pad_t[NPADS];               /* how long each has been held (samples) */
void eng_pads(uint32_t held) { pads = held; }

/* ----------------------------------------------------------------- MUTATE --- */
static volatile uint8_t mut[4];          /* the knobs (the main loop's) */
static uint8_t mut_was[4];
void eng_mutate(int k, int v)
{
    if (k >= 0 && k < 4)
        mut[k] = (uint8_t)clamp(v, 0, 127);
}
/* ERODE: per track, the steps the acid has eaten (bit s); it spreads each bar, heals when turned back */
static uint32_t rot[NINST][2];
static uint32_t mu_noise = 0x1B873593u;
static inline float mrnd(void) { return 0.5f + 0.5f * white(&mu_noise); }
static int rotten(int t, int32_t s) { return (rot[t][(s >> 5) & 1] >> (s & 31) & 1u) != 0; }
static void rot_set(int t, int32_t s, int on)
{
    uint32_t b = 1u << (s & 31);
    if (on)
        rot[t][(s >> 5) & 1] |= b;
    else
        rot[t][(s >> 5) & 1] &= ~b;
}
/* the acid now: seeded at once when turned up (so a click is heard), spreading to the neighbours once a bar, healing
 * when turned down */
static void erode(int grow)
{
    float a = (float)mut[MU_ERODE] / 127.0f, seed = a > 0.0f ? 0.12f + 0.6f * a : 0.0f;   /* (a click eats at once) */
    int t;
    int32_t s;
    for (t = 0; t < NINST; t++) {
        int32_t len = PAT ? (PAT->tr[t].len >= 1u && PAT->tr[t].len <= 64u ? PAT->tr[t].len : 16) : 16;
        uint32_t was[2] = {rot[t][0], rot[t][1]};
        for (s = 0; s < len; s++) {
            int r = (was[(s >> 5) & 1] >> (s & 31) & 1u) != 0;
            if (r && mrnd() > 0.35f + 0.65f * a)   /* healing: less acid, more heals */
                rot_set(t, s, 0);
            else if (!r && mrnd() < seed)          /* new pits */
                rot_set(t, s, 1);
            else if (!r && grow) {                  /* the spread: a neighbour eaten */
                int32_t l = (s + len - 1) % len, n = (s + 1) % len;
                int nb = ((was[(l >> 5) & 1] >> (l & 31)) & 1u) || ((was[(n >> 5) & 1] >> (n & 31)) & 1u);
                if (nb && mrnd() < a * 0.7f)
                    rot_set(t, s, 1);
            }
        }
        if (a <= 0.0f)
            rot[t][0] = rot[t][1] = 0;
    }
}

/* MORPH: four worlds the sounds go through, after the pattern's own: TOY, GLASS, RUBBER, BRUTAL (each instrument's
 * four knobs there), and what else each world moves: the pitch (semitones), drive, room, bounce */
static const uint8_t WORLD[4][NINST][4] = {
    {{110, 10, 15, 0}, {120, 0, 10, 0}, {127, 5, 127, 0}, {127, 60, 5, 0}, {120, 20, 120, 0}, {127, 15, 0, 0}},    /* TOY */
    {{80, 30, 110, 0}, {90, 0, 120, 0}, {90, 110, 127, 0}, {40, 85, 127, 0}, {90, 120, 127, 0}, {64, 120, 0, 0}},   /* GLASS */
    {{20, 127, 60, 20}, {10, 10, 40, 30}, {20, 30, 64, 20}, {10, 30, 90, 0}, {15, 60, 50, 20}, {0, 60, 127, 60}},  /* RUBBER */
    {{60, 120, 50, 127}, {40, 127, 60, 127}, {60, 50, 20, 127}, {80, 115, 40, 127}, {40, 50, 100, 127}, {0, 110, 96, 127}},   /* BRUTAL */
};
static const float W_SEMI[5] = {0.0f, 7.0f, 3.0f, -5.0f, -3.0f};
static const float W_DRIVE[5] = {0.0f, 0.0f, 0.0f, 20.0f, 100.0f};
static const float W_SPACE[5] = {0.0f, 0.0f, 70.0f, 15.0f, 0.0f};
static const float W_BOUNCE[5] = {0.0f, 20.0f, 0.0f, 100.0f, 30.0f};
static float morph_w(int *i)             /* the world's segment (0: as set -> TOY .. 3: RUBBER -> BRUTAL), how far */
{
    float m = (float)mut[MU_MORPH] * (4.0f / 127.0f);
    int k = (int)m;
    if (k > 3)
        k = 3;
    *i = k;
    return m - (float)k;
}
static float morph_lerp(const float *tab)
{
    int i;
    float f = morph_w(&i);
    return tab[i] + f * (tab[i + 1] - tab[i]);
}
static void morph_knobs(int t, float *p)
{
    int i, k;
    float f;
    if (!mut[MU_MORPH])
        return;
    f = morph_w(&i);
    for (k = 0; k < 4; k++) {
        float a = i == 0 ? p[k] : (float)WORLD[i - 1][t][k], b = (float)WORLD[i][t][k];
        p[k] = a + f * (b - a);
    }
}

/* ENTANGLE: which body plays instrument t's hit: its own, its partner's (KICK-BASS, SNARE-PERC, HATS-GLITCH), the
 * next one's, or any (by chance, more the further it is turned) */
static const uint8_t PARTNER[NINST] = {I_BASS, I_PERC, I_GLITCH, I_HATS, I_SNARE, I_KICK};
static int entangle(int t)
{
    float e = (float)mut[MU_ENTANGLE] / 127.0f, r = mrnd();
    if (e <= 0.0f)
        return t;
    if (e < 0.25f)
        return r < 0.3f + e * 2.8f ? PARTNER[t] : t;
    if (e < 0.5f)
        return r < (e - 0.25f) * 4.0f ? (t + 1) % NINST : PARTNER[t];
    if (e < 0.85f)
        return r < (e - 0.5f) * 2.8f ? (int)(mrnd() * (float)NINST) % NINST : (t + 1 + (int)(e * 4.0f)) % NINST;
    return (int)(mrnd() * (float)NINST) % NINST;
}
static float sweep_x, gate_g = 1.0f;
static uint32_t gate_n;
static float gate_inv;                   /* 1 / a thirty-second, in samples */
static struct {                          /* the buffer's readers */
    float speed, lag, frz_len, frz_pos, mix, last_lag, slice_e, frz_start;
} bf = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
#define HELD(p) ((pads >> (p) & 1u) != 0)
static volatile uint32_t shed;           /* blocks left of the emergency brake (overload) */
void eng_overload(void) { shed = 344u; }
int eng_step(void) { return step_now; }
int eng_playing(void) { return step_now >= 0 || ext_run; }
float eng_env(int inst) { return inst >= 0 && inst < NINST ? env_out[inst] : 0.0f; }
float eng_energy(int inst) { return inst >= 0 && inst < NINST ? energy_out[inst] : 0.0f; }
void eng_disp(int inst, float out[4])
{
    int k;
    for (k = 0; k < 4; k++)
        out[k] = disp_out[inst][k];
}
uint32_t eng_hits(void) { return hits; }
void eng_panic(void)
{
    uint32_t t;
    for (t = 0; t < NINST; t++) {        /* still, but tuned (another body may shake it before its first hit) */
        float p[4] = {64.0f, 64.0f, 64.0f, 30.0f};
        uint32_t ns = 1;
        if (PAT)
            for (ns = 0; ns < 4u; ns++)
                p[ns] = (float)PAT->tr[t].p[ns];
        zero(&S[t].body, sizeof S[t].body);
        ns = 1;
        body_hit((int)t, p, 0.0f, &ns);
    }
}

/* MOTION: each instrument's slow movement, on its own clock in steps (from PLAY) */
static float mo_pos;                     /* steps since PLAY, at the top of the block */
static int32_t mo_cycle[NINST];          /* RANDOM: the turn its value is for */
static float mo_val[NINST];
static uint32_t mo_noise = 0x68E31DA4u;
static float motion(int t, uint32_t at, int *target)   /* -1 .. 1 x DEPTH now, and where it goes (0..3 a knob, 4 PITCH) */
{
    const uint8_t *m = PAT->tr[t].mot;
    float d = (float)m[MO_DEPTH] / 127.0f, cyc, x, v;
    int32_t turn;
    if (d <= 0.0f)
        return 0.0f;
    cyc = 4.0f * fm_exp2f((float)m[MO_RATE] * (6.0f / 127.0f));     /* 4 .. 256 steps */
    x = (mo_pos + (float)at / fm_maxf(g_step, 1.0f)) / cyc;
    turn = (int32_t)x;
    x -= (float)turn;
    switch (exc_type(m[MO_SHAPE])) {     /* (the same four zones as EXCITE's TYPE) */
    case 0: v = fm_sin_turns(x); break;
    case 1: v = 2.0f * x - 1.0f; break;
    case 2:
        if (turn != mo_cycle[t]) {
            mo_cycle[t] = turn;
            mo_val[t] = white(&mo_noise);
        }
        v = mo_val[t];
        break;
    default: {                           /* BOUNCE: a ball's hops, lower and quicker each */
        float y = 1.0f - (1.0f - x) * (1.0f - x);
        v = 2.0f * (1.0f - x) * fm_fabsf(fm_sin_turns(y * 2.5f)) - 1.0f;
        break;
    }
    }
    *target = m[MO_TARGET] / 26 > 4 ? 4 : m[MO_TARGET] / 26;
    return v * d;
}

/* a hit queued at sample `at` of the block: its knobs now (as set, pushed by the web, moved by MASS), then its push */
static void push_ev(int t, uint32_t at, float vel)
{
    ev_t *e;
    if (nev[t] >= NEV)
        return;
    e = &ev[t][nev[t]++];
    e->at = (uint16_t)at;
    e->vel = vel;
    web_knobs(t, PAT->tr[t].p, (int)g_mass, e->p);
    morph_knobs(t, e->p);
    e->semi = 0.0f;
    {                                    /* MOTION: a knob moved, or the pitch */
        int tg = 0;
        float mv = motion(t, at, &tg);
        if (mv != 0.0f) {
            if (tg < 4)
                e->p[tg] = fm_clampf(e->p[tg] + mv * 64.0f, 0.0f, 127.0f);
            else
                e->semi = mv * 12.0f;
        }
    }
    {                                    /* EXCITE's BOUNCE: the mallet springs back: a flam, a rattle, a buzz */
        float b = (float)PAT->tr[t].exc[EX_BOUNCE] / 127.0f;
        if (b > 0.02f && vel > 0.2f)
            later(t, (float)at + (30.0f + 260.0f * (1.0f - b)) * (0.7f + 0.6f * rnd01()), vel * (0.25f + 0.35f * b));
    }
    web_push(t, vel, (int)g_mass, (int)g_chaos);
    hits++;
}

static float step_base(void)             /* samples a step (a sixteenth) */
{
    float bpm = eng_ext() && ext_bpm > 20.0f ? ext_bpm : (float)PAT->bpm;
    return ENG_SR * 15.0f / fm_clampf(bpm, 30.0f, 300.0f);
}
static float step_len(int32_t s)         /* swing: the even sixteenths long, the odd ones short */
{
    float sw = (float)PAT->swing * (0.33f / 127.0f);
    return step_base() * ((s & 1) ? 1.0f - sw : 1.0f + sw);
}
static int32_t trk_len(const track_t *tr) { return tr->len >= 1u && tr->len <= 64u ? tr->len : 16; }
int eng_track_step(int inst)
{
    int32_t s = step_now;
    return s < 0 || !PAT ? -1 : (int)(s % trk_len(&PAT->tr[inst]));
}

/* What follows a hit, the physics of the rhythm (the sample's: no grid, clusters):
 * DRIFT: the hit itself lands late, as far as the web has pushed the instrument (and a little chance): off the grid.
 * BOUNCE: the instrument's own CHAOS (ENGINE page) is the chance that a hit bounces: it strikes again and again, each
 * time sooner and softer, like a ball coming to rest (how soon the first, how many: MASS and its CHAOS). */
#define NPEND 6
static float pend_wait[NINST][NPEND], pend_vel[NINST][NPEND];
static float bc_wait[NINST], bc_gap[NINST], bc_vel[NINST], bc_e[NINST];

static uint8_t waiting;                  /* bit t: something drifting or bouncing on instrument t (else: not looked at) */
static void later(int t, float wait, float vel)
{
    int k;
    for (k = 0; k < NPEND; k++)
        if (pend_wait[t][k] <= 0.0f) {
            pend_wait[t][k] = wait;
            pend_vel[t][k] = vel;
            waiting |= (uint8_t)(1u << t);
            return;
        }
}

static void play_step(int32_t s, uint32_t at)
{
    uint32_t t;
    float sb = step_base(), dr = g_drift / 127.0f, mass = g_mass / 127.0f;
    step_now = s;
    if (rep_start >= 0) {                /* REPEAT: the window's steps, round */
        s = rep_start + (s - rep_start) % rep_len;
    } else if (half_start >= 0) {        /* HALF TIME: every step twice as long (the odd ones rest) */
        int32_t d = s - half_start;
        if (d & 1)
            return;
        s = half_start + d / 2;
    }
    for (t = 0; t < NINST; t++) {
        const track_t *tr = &PAT->tr[t];
        int32_t ts = s % trk_len(tr);
        float vel, delay, c = fm_maxf(fm_maxf((float)tr->fx[FX_CHAOS], g_fxchaos), morph_lerp(W_BOUNCE)) / 127.0f;
        int b;
        if (!step_on(tr, ts) || (muted >> t & 1u) || (HELD(P_BREAK) && (t == I_KICK || t == I_BASS)))
            continue;
        vel = step_acc(tr, ts) ? 1.0f : 0.68f;
        if (mut[MU_ERODE]) {             /* ERODE: eaten, or brittle next to a hole */
            int32_t len = trk_len(tr);
            if (rotten((int)t, ts))
                continue;
            if (rotten((int)t, (ts + 1) % len) || rotten((int)t, (ts + len - 1) % len))
                vel *= 0.25f + 0.6f * mrnd();
        }
        b = entangle((int)t);            /* ENTANGLE: the body that plays it */
        delay = sb * 0.6f * dr * dr
                * fm_clampf(0.35f * rnd01() + 0.65f * (fm_fabsf(wd[t][1]) + fm_fabsf(wd[t][2])) * (1.0f / 40.0f), 0.0f, 1.0f);
        if (mut[MU_ERODE] && rotten((int)t, (ts + 1) % trk_len(tr)))
            delay += sb * 0.3f * mrnd();   /* (brittle: a little late too) */
        if (delay < 1.0f)
            push_ev(b, at, vel);
        else
            later(b, (float)at + delay, vel);
        if (rep_twice)                   /* REPEAT 1/32: again half a step on */
            later(b, (float)at + delay + sb * 0.5f, vel * 0.8f);
        if (mut[MU_RIPPLE]) {            /* RIPPLE: the hit runs through the web, the tied ones echo it */
            float r = (float)mut[MU_RIPPLE] / 127.0f;
            uint32_t j;
            for (j = 0; j < NINST; j++)
                if ((int)j != b && wtie[b][j] > 0.0f && mrnd() < 0.5f + 0.5f * r) {
                    float d = sb * (r < 0.5f ? (float)(1 + (int)(mrnd() * 3.0f)) * 0.5f   /* echoes on the grid */
                                             : 0.3f + 2.2f * mrnd());                        /* waves: off it */
                    later((int)j, (float)at + delay + d, vel * (0.5f + 0.4f * r) * fm_minf(wtie[b][j] * 1.4f, 1.0f));
                    if (r > 0.6f)        /* SWARM: the echo echoes */
                        later((int)((j + 1 + (uint32_t)(mrnd() * 4.0f)) % NINST), (float)at + delay + d * 1.8f,
                              vel * 0.35f * r);
                }
        }
        if (c > 0.0f && rnd01() < c * c * 0.9f + 0.1f * c) {   /* a bounce: its first rebound, its fall */
            bc_gap[b] = sb * (1.1f - 0.7f * mass) * (0.55f + 0.45f * rnd01());
            bc_e[b] = 0.5f + 0.42f * c - 0.1f * mass;
            bc_vel[b] = vel * (0.6f + 0.3f * c);
            bc_wait[b] = delay + bc_gap[b];
            waiting |= (uint8_t)(1u << b);
        }
    }
}

static int next_pattern(void)            /* on a beat: the pattern waiting takes over, from its top (1: it did) */
{
    if (!pat_next)
        return 0;
    PAT = (const pattern_t *)pat_next;
    pat_next = 0;
    return 1;
}

/* the pads: what changed since the last block, and where they move the pattern's values now */
static void pads_block(uint32_t n)
{
    uint32_t p = pads, on = p & ~pads_was, k;
    float a = 1.0f - fm_expf(-(float)n / (0.08f * ENG_SR)), mass, chaos, drift, dil, fxc, cx;
    static const bp_t P_PITCH[] = {{0, -12.0f}, {32, -5.0f}, {64, 0.0f}, {96, 7.0f}, {127, 12.0f}};
    for (k = 0; k < NPADS; k++)
        pad_t[k] = (p >> k & 1u) ? pad_t[k] + (float)n : 0.0f;
    /* REPEAT: the shortest held wins; its window from where it was pressed */
    {
        int r = HELD(P_REP32) ? 2 : HELD(P_REP16) ? 1 : HELD(P_REP8) ? 0 : -1;
        if (r >= 0 && step_now >= 0) {
            int32_t l = r == 0 ? 2 : 1;
            if (rep_start < 0 || l != rep_len)
                rep_start = step_now - step_now % l, rep_len = l;
            rep_twice = r == 2;
        } else {
            rep_start = -1;
            rep_twice = 0;
        }
    }
    if (!HELD(P_HALF))
        half_start = -1;
    else if (half_start < 0)
        half_start = step_now >= 0 ? step_now : 0;
    if ((on >> P_COLLAPSE & 1u) || (on >> P_DROP & 1u)) {   /* the collapse: everything shoved, everything struck */
        uint32_t t;
        web_slam();
        for (t = 0; t < NINST; t++)
            if (!(muted >> t & 1u))
                hit_req |= (uint8_t)(1u << t), hit_vel[t] = 1.0f;
    }
    if (on >> P_REWIND & 1u)              /* REWIND: from a standstill */
        bf.speed = 0.0f;
    if ((on >> P_DROP & 1u) && step_now >= 0) {   /* DROP: from the top, now */
        step_pos = 0;
        step_left = 0.0f;
    }
    /* where they move the values: each glides there (80 ms) */
    mass = (float)PAT->mass;
    chaos = (float)PAT->chaos;
    drift = (float)PAT->drift;
    dil = CURVE(P_PITCH, PAT->pitch);
    fxc = 0.0f;
    cx = 1.0f;
    dil += morph_lerp(W_SEMI);           /* MORPH's world: its pitch */
    if (HELD(P_ZEROG)) {
        mass = 0.0f;
        fxc = 110.0f;
        dil += 5.0f;
    }
    if (HELD(P_BLACKHOLE)) {
        mass = 127.0f;
        dil -= 7.0f;
    }
    if (HELD(P_COLLAPSE)) {              /* the springs torn: everything shakes everything, the pitch falls, then
                                          * (let go) it springs back */
        chaos = 127.0f;
        cx = 6.0f;
        dil -= 12.0f * fm_minf(pad_t[P_COLLAPSE] / (2.0f * 16.0f * step_base()), 1.0f);
    }
    if (HELD(P_AVALANCHE)) {
        chaos = 127.0f;
        fxc = 127.0f;
    }
    if (HELD(P_DRIFT))
        drift = 127.0f;
    if (HELD(P_UP))
        dil += 12.0f;
    if (HELD(P_DOWN))
        dil -= 12.0f;
    g_mass += (mass - g_mass) * a;
    g_chaos += (chaos - g_chaos) * a;
    g_drift += (drift - g_drift) * a;
    g_fxchaos = fxc;
    g_couple_x += (cx - g_couple_x) * a;
    g_dil += (fm_exp2f(fm_clampf(dil, -24.0f, 19.0f) / 12.0f) - g_dil) * (HELD(P_COLLAPSE) ? 1.0f : a);
    pads_was = p;
}

/* GLITCH (the master's): at a step, by chance (more the further it is turned), the last slice of the mix is caught
 * and repeated for a step or two, sometimes backwards; turned up the slices get shorter (SKIP, STUTTER, SHRED) */
#define GL_N 16384u                      /* 0.37 s: GLITCH, TAPE STOP, REWIND, FREEZE */
static int16_t gl_buf[2][GL_N] PLAT_POOL;
static uint32_t gl_w, gl_start, gl_len, gl_pos, gl_left, gl_at = 0xFFFFu;
static uint8_t gl_rev;
static uint32_t gl_noise = 0x7F4A7C15u;
static void glitch_maybe(uint32_t at)
{
    float g = HELD(P_STUTTER) ? 1.0f : (float)PAT->glitch / 127.0f, r = 0.5f + 0.5f * white(&gl_noise), sb = step_base(), len;
    if (g < 0.02f || gl_left || r > 0.08f + 0.5f * g * g)
        return;
    r = 0.5f + 0.5f * white(&gl_noise);
    len = sb * (r < 0.5f - 0.3f * g ? 0.5f : r < 0.85f - 0.2f * g ? 0.25f : 0.125f);
    gl_len = (uint32_t)fm_clampf(len, 256.0f, (float)GL_N * 0.9f);
    gl_left = (uint32_t)(sb * (r < 0.6f ? 1.0f : 2.0f));
    gl_rev = (0.5f + 0.5f * white(&gl_noise)) < 0.25f * g;
    gl_at = at;
}

/* one step of the clock (internal or MIDI): the next pattern on a beat, then the step */
static void clock_step(uint32_t at)
{
    glitch_maybe(at);
    if (mut[MU_ERODE] && (step_pos & 15) == 0)   /* ERODE: the acid spreads, once a bar */
        erode(1);
    if (pat_next && (step_pos & 3) == 0 && next_pattern())
        step_pos = 0;
    play_step(step_pos, at);
    step_pos++;
    if (step_pos >= 0x40000000)
        step_pos = 0;
}

/* the sequencer over the block: where each instrument is struck */
static void sequence(uint32_t n)
{
    uint32_t i, t;
    int k;
    for (t = 0; t < NINST; t++)
        nev[t] = 0;
    if (hit_req) {
        for (t = 0; t < NINST; t++)
            if (hit_req >> t & 1u)
                push_ev((int)t, 0, hit_vel[t]);
        hit_req = 0;
    }
    /* MIDI clock in: each clock counted at the top of the block (a 1 kHz tick and a 2.9 ms block: within ~3 ms) */
    if (mut[MU_ERODE] != mut_was[MU_ERODE]) {   /* ERODE turned: the acid now, not at the next bar */
        erode(0);
        mut_was[MU_ERODE] = mut[MU_ERODE];
    }
    if (ext_req >= 0) {
        if (ext_req == 0) {
            ext_run = 0;
            step_now = -1;
        } else {
            ext_run = 1;
            running = 0;
            ext_count = 0;
            if (ext_req == 1)
                step_pos = 0;
        }
        ext_req = -1;
    }
    if (ext_clocks) {
        uint32_t f = plat_lock(), c = ext_clocks;
        ext_clocks = 0;
        plat_unlock(f);
        ext_quiet = 0;
        while (c--) {
            if (ext_run && ext_count % 6u == 0u)
                clock_step(0);
            ext_count++;
        }
    } else if (ext_quiet < 0x7FFFFFFFu) {
        ext_quiet += n;
    }
    for (i = 0; i < n; i++) {
        if (waiting)
            for (t = 0; t < NINST; t++) {
                int left = 0;
                if (!(waiting >> t & 1u))
                    continue;
                for (k = 0; k < NPEND; k++)   /* the hits drifted late */
                    if (pend_wait[t][k] > 0.0f) {
                        if ((pend_wait[t][k] -= 1.0f) <= 0.0f)
                            push_ev((int)t, i, pend_vel[t][k]);
                        else
                            left = 1;
                    }
                if (bc_wait[t] > 0.0f) {      /* a bounce */
                    if ((bc_wait[t] -= 1.0f) <= 0.0f) {
                        if (shed && bc_vel[t] < 0.4f)   /* braking: the soft bounces are let go */
                            bc_vel[t] = 0.0f;
                        else
                            push_ev((int)t, i, bc_vel[t]);
                        bc_gap[t] *= bc_e[t];
                        bc_vel[t] *= 0.55f + 0.4f * bc_e[t];
                        bc_wait[t] = bc_vel[t] < 0.08f || bc_gap[t] < 250.0f ? 0.0f : bc_gap[t];
                    }
                    if (bc_wait[t] > 0.0f)
                        left = 1;
                }
                if (!left)
                    waiting &= (uint8_t)~(1u << t);
            }
        if (!running)
            continue;
        step_left -= 1.0f;
        if (step_left <= 0.0f) {
            clock_step(i);
            step_left += step_len(step_pos - 1);
        }
    }
}

/* ------------------------------------------------------------- the master --- */
#define PRE_N 1800u
static float rv_comb0[1116] PLAT_POOL, rv_comb1[1188] PLAT_POOL, rv_comb2[1277] PLAT_POOL, rv_comb3[1356] PLAT_POOL;
static float rv_ap0[556] PLAT_POOL, rv_ap1[441] PLAT_POOL, rv_ap2[579] PLAT_POOL, rv_ap3[464] PLAT_POOL;
static float rv_pre[PRE_N] PLAT_POOL;
static struct {
    uint32_t ci[4], ai[4], pi;
    float cf[4], fb, damp, ret;
    float d_g, d_norm;
    float kick_env;
    svf_c sw;                            /* SWEEP */
    svf_s sws[2];
    uint32_t lf_n;                       /* LO-FI */
    float lf_s[2];
} fx;

static inline float comb(float *buf, uint32_t n, uint32_t *i, float *f, float x)
{
    float y = buf[*i];
    *f = y + fx.damp * (*f - y);
    buf[*i] = x + *f * fx.fb;
    if (++*i >= n)
        *i = 0;
    return y;
}
static inline float allpass(float *buf, uint32_t n, uint32_t *i, float x)
{
    float b = buf[*i], y = b - x;
    buf[*i] = x + b * 0.5f;
    if (++*i >= n)
        *i = 0;
    return y;
}

static const float PAN[NINST] = {0.0f, 0.05f, 0.3f, -0.35f, 0.25f, 0.0f};
static const float SEND[NINST] = {0.0f, 0.35f, 0.12f, 0.4f, 0.45f, 0.0f};
static const float TRIM[NINST] = {0.95f, 0.8f, 1.05f, 1.25f, 0.6f, 1.0f};
static float gl[NINST], gr[NINST];

static void master_block(void)
{
    float sp = fm_minf((float)PAT->space + morph_lerp(W_SPACE), 127.0f) / 127.0f;
    float dr = fm_minf((float)PAT->drive + morph_lerp(W_DRIVE), 127.0f) / 127.0f;
    uint32_t t;
    fx.fb = 0.62f + 0.3f * sp;
    fx.damp = 0.45f - 0.25f * sp;
    fx.ret = sp < 0.02f ? 0.0f : 0.25f + 0.9f * sp;
    if (HELD(P_THROW)) {                 /* SPACE THROW: the room opened wide */
        fx.ret = 1.6f;
        fx.fb = 0.93f;
    }
    gate_inv = 1.0f / fm_maxf(step_base() * 0.5f, 1.0f);
    if (!HELD(P_GATE))
        gate_n = 0;
    if (HELD(P_SWEEP))                   /* SWEEP: closes over a bar, opens in a beat */
        sweep_x = fm_minf(sweep_x + 128.0f / (16.0f * step_base()), 1.0f);
    else
        sweep_x = fm_maxf(sweep_x - 128.0f / (4.0f * step_base()), 0.0f);
    if (sweep_x > 0.0f)
        svf_set(&fx.sw, 18000.0f * fm_exp2f(-sweep_x * 6.5f), 1.8f);
    fx.d_g = 1.0f + 5.0f * dr * dr;
    fx.d_norm = 1.0f / fm_tanhf(fx.d_g * 0.6f) * 0.6f;
    for (t = 0; t < NINST; t++) {
        float lv = (float)PAT->tr[t].level / 127.0f;
        lv = lv * lv * TRIM[t] * 1.6f;
        gl[t] = lv * (1.0f - fm_maxf(PAN[t], 0.0f));
        gr[t] = lv * (1.0f + fm_minf(PAN[t], 0.0f));
    }
}

/* =============================================================== the render === */
void eng_init(void)
{
    uint32_t t;
    for (t = 0; t < NINST; t++) {
        pat0.tr[t].p[0] = pat0.tr[t].p[1] = pat0.tr[t].p[2] = 64;
        pat0.tr[t].level = 100;
        S[t].noise = 0x2545F491u + 0x9E3779B9u * (t + 1u);
    }
    pat0.mass = pat0.chaos = 64;
    pat0.bpm = 124;
    for (t = 0; t < NINST; t++) {
        pat0.tr[t].len = 16;
        pat0.tr[t].fx[FX_FILTER] = 64;
        pat0.tr[t].exc[EX_TYPE] = 10;
        pat0.tr[t].exc[EX_HARD] = pat0.tr[t].exc[EX_POS] = 64;
        pat0.tr[t].mot[MO_RATE] = 64;
        pat0.tr[t].link = 64;
    }
    pat0.pitch = 64;
    pat0.web[0] = pat0.web[1] = pat0.web[2] = 64;
    PAT = &pat0;
    web_topology(eng_topology(64));
    {                                    /* the bodies tuned before their first hit: another may shake them first */
        float p[4] = {64.0f, 64.0f, 64.0f, 30.0f};
        uint32_t ns = 1;
        for (t = 0; t < NINST; t++)
            body_hit((int)t, p, 0.0f, &ns);
    }
}

#define BLK_MAX 256u
static float ibuf[NINST][BLK_MAX];
static uint32_t blk_n;
static float couple;                     /* how hard the bodies shake each other (CHAOS) */

/* ------------------------------------------------- each instrument's effects --- */
/* The ENGINE page, after the body:
 * FM: the sound read from a short line whose length swings at audio rate (phase modulation: sidebands); turned up the
 *     swing gets faster and deeper, from a warble to bells to noise.
 * GRAIN: the last 46 ms kept, two grains read from it at random places, at half, the same or twice the speed, in
 *     windows: a haze, a cloud, a spray.
 * FILTER: a state variable filter, low-pass below the middle, high-pass above, a little resonance at the ends. */
#define FM_N 256u
#define GR_N 2048u
typedef struct {
    float fm_buf[FM_N];
    uint32_t fm_w;
    float fm_ph, fm_inc, fm_depth;
    int16_t gr_buf[GR_N];
    uint32_t gr_w;
    float g_pos[2], g_rate[2], g_ph[2], g_inc, g_mix;
    svf_c flt;
    svf_s flt_s;
    int flt_mode;                        /* 0 none, 1 low-pass, 2 high-pass */
    uint32_t ns;
} inst_fx_t;
static inst_fx_t IFX[NINST] PLAT_POOL;

static void ifx_block(int t)             /* the knobs to coefficients, once a block */
{
    inst_fx_t *x = &IFX[t];
    const uint8_t *f = PAT->tr[t].fx;
    float fm = (float)f[FX_FM] / 127.0f, gr = (float)f[FX_GRAIN] / 127.0f, fl = (float)f[FX_FILTER];
    if (!x->ns)
        x->ns = 0x51ED270Bu + 0x9E3779B9u * (uint32_t)t;
    x->fm_depth = fm < 0.02f ? 0.0f : 2.0f + 60.0f * fm * fm;                    /* samples */
    x->fm_inc = (6.0f * fm_exp2f(fm * 9.0f)) / ENG_SR;                       /* 6 Hz .. 3 kHz */
    x->g_mix = gr < 0.02f || shed ? 0.0f : 0.3f + 0.7f * gr;
    x->g_inc = (8.0f + 50.0f * gr) / ENG_SR;                                 /* grains a second, each */
    if (fl < 60.0f) {
        x->flt_mode = 1;
        svf_set(&x->flt, 150.0f * fm_exp2f(fl / 60.0f * 7.0f), fl < 20.0f ? 2.0f : 0.8f);
    } else if (fl > 68.0f) {
        x->flt_mode = 2;
        svf_set(&x->flt, 40.0f * fm_exp2f((fl - 68.0f) / 59.0f * 7.0f), fl > 110.0f ? 2.0f : 0.8f);
    } else {
        x->flt_mode = 0;
    }
}

static inline float ifx_run(int t, float y)
{
    inst_fx_t *x = &IFX[t];
    if (x->fm_depth > 0.0f) {            /* FM: a line swung at audio rate */
        float d, fr, a, b;
        uint32_t r0;
        x->fm_buf[x->fm_w] = y;
        x->fm_ph += x->fm_inc;
        if (x->fm_ph >= 1.0f)
            x->fm_ph -= 1.0f;
        d = 1.0f + x->fm_depth * (1.0f + fm_sin_turns(x->fm_ph));
        d = (float)x->fm_w - d + (float)FM_N;
        r0 = (uint32_t)d;
        fr = d - (float)r0;
        a = x->fm_buf[r0 & (FM_N - 1u)];
        b = x->fm_buf[(r0 + 1u) & (FM_N - 1u)];
        x->fm_w = (x->fm_w + 1u) & (FM_N - 1u);
        y = a + fr * (b - a);
    }
    if (x->g_mix > 0.0f) {               /* GRAIN: two grains from the last 46 ms */
        float g = 0.0f;
        int k;
        x->gr_buf[x->gr_w] = (int16_t)(fm_clampf(y, -1.0f, 1.0f) * 32767.0f);
        for (k = 0; k < 2; k++) {
            float w, pos;
            uint32_t r0;
            x->g_ph[k] += x->g_inc;
            if (x->g_ph[k] >= 1.0f) {    /* a new grain: somewhere behind, a speed of half, one or two */
                float r = 0.5f + 0.5f * white(&x->ns);
                x->g_ph[k] -= 1.0f;
                x->g_pos[k] = (float)x->gr_w - 200.0f - r * 1500.0f;
                x->g_rate[k] = r < 0.3f ? 0.5f : r < 0.75f ? 1.0f : 2.0f;
            }
            pos = x->g_pos[k];
            x->g_pos[k] += x->g_rate[k];
            while (pos < 0.0f)
                pos += (float)GR_N;
            r0 = (uint32_t)pos & (GR_N - 1u);
            w = 1.0f - fm_fabsf(2.0f * x->g_ph[k] - 1.0f);   /* the window: a smoothed triangle */
            g += (float)x->gr_buf[r0] * (1.0f / 32768.0f) * w * w * (3.0f - 2.0f * w);
        }
        x->gr_w = (x->gr_w + 1u) & (GR_N - 1u);
        y += x->g_mix * (g * 1.2f - 0.5f * y);
    }
    if (x->flt_mode) {
        float lp, bp, hp = svf_run(&x->flt, &x->flt_s, y, &lp, &bp);
        y = x->flt_mode == 1 ? lp : hp;
        if (!(y == y)) {                 /* (never: but a filter that ever held a NaN is cleared, not kept) */
            x->flt_s.ic1 = x->flt_s.ic2 = 0.0f;
            y = 0.0f;
        }
    }
    return y;
}

/* ------------------------------------------------------------- EXCITE --- */
/* How a body is struck (the EXCITE page). The body's own mallet, made softer or harder and moved (HARDNESS, POSITION),
 * or instead:
 * PLUCK: a push of a sample or two at the rim: bright, thin;
 * BOW: friction: a bow drawn across the body for a beat. It pulls the mass it touches along until it slips (a force
 * that falls as the two speeds part: P dv k / (1 + (k dv)^2), dv the bow's speed less the mass's): stick and slip, the
 * body sings on its own note for as long as the bow is drawn;
 * BRUSH: a scrape: noise pushing the body, fading over 30 .. 120 ms.
 * The pushes go where the web's do (the mass a body is pushed at). */
typedef struct {
    uint8_t type;
    float env, att, rel, left;           /* the bow's and the brush's envelope: rising, held `left` samples, falling */
    float pres, vb, k;                   /* the bow */
    float brush;                         /* the brush's level */
    uint32_t ns;
} exc_t;
static exc_t EXC[NINST];

static float *contact_v(int t)           /* the speed of the mass a body is pushed at */
{
    inst_state_t *s = &S[t];
    switch (t) {
    case I_KICK: return &s->body.k.h.v0;
    case I_SNARE: return &s->body.s.h.v0;
    case I_HATS: return &s->body.h.v[0];
    case I_GLITCH: return &s->body.g.v0;
    case I_PERC: return &s->body.p.v[0];
    default: return &s->body.b.v[NS / 2];
    }
}

/* the body's own mallet, softer or harder (f: its length x f, the same push in all; ampf: the push x ampf), moved
 * (pos 0 centre .. 1 rim, < 0 as it was) */
static void mallet_shape(int t, float f, float pos, float ampf)
{
    inst_state_t *s = &S[t];
    float *len, *left, *amp;
    switch (t) {
    case I_KICK: len = &s->body.k.h.m_len; left = &s->body.k.h.m_left; amp = &s->body.k.h.m_amp;
        if (pos >= 0.0f)
            s->body.k.h.m_pos = pos * 0.9f;
        break;
    case I_SNARE: len = &s->body.s.h.m_len; left = &s->body.s.h.m_left; amp = &s->body.s.h.m_amp;
        if (pos >= 0.0f)
            s->body.s.h.m_pos = pos * 0.9f;
        break;
    case I_HATS: len = &s->body.h.m_len; left = &s->body.h.m_left; amp = &s->body.h.m_amp; break;
    case I_PERC: len = &s->body.p.m_len; left = &s->body.p.m_left; amp = &s->body.p.m_amp;
        if (pos >= 0.0f)
            s->body.p.m_pos = pos;
        break;
    case I_BASS: len = &s->body.b.m_len; left = &s->body.b.m_left; amp = &s->body.b.m_amp;
        if (pos >= 0.0f)
            s->body.b.m_at = 1 + (int)(pos * (float)(NS / 2 - 1));
        break;
    default: return;                     /* (the GLITCH's ball is its own mallet) */
    }
    if (*len <= 0.0f)
        return;
    {
        float nl = fm_clampf(*len * f, 2.0f, 600.0f);   /* (two samples at least: one is no push at all) */
        *amp *= *len / nl * ampf;
        *len = *left = nl;
    }
}

/* a hit with its EXCITE: after the body's own strike */
static void excite(int t, float vel)
{
    const uint8_t *x = PAT->tr[t].exc;
    exc_t *e = &EXC[t];
    float hard = fm_exp2f(((float)x[EX_HARD] - 64.0f) / 32.0f);   /* 0.25 .. 4 */
    float pos = fm_fabsf((float)x[EX_POS] - 64.0f) < 3.0f ? -1.0f : (float)x[EX_POS] / 127.0f;
    e->type = (uint8_t)exc_type(x[EX_TYPE]);
    if (!e->ns)
        e->ns = 0x2F6B1C3Du + 977u * (uint32_t)t;
    switch (e->type) {
    case XT_PLUCK:
        mallet_shape(t, 0.12f / hard, pos >= 0.0f ? pos : 0.9f, 1.0f);
        break;
    case XT_BOW:                         /* the mallet barely; the bow drawn for a beat */
        mallet_shape(t, 4.0f, pos, 1.0f);
        if (t == I_GLITCH)
            S[t].body.g.g = 0.0f;        /* (a bowed plate: no ball) */
        e->att = 1.0f / (0.015f * ENG_SR);
        e->rel = 1.0f / (0.06f * ENG_SR);
        e->left = 4.0f * g_step;
        {                                /* (how hard each body takes a bow: the string is easy to drive) */
            static const float BOWP[NINST] = {1.0f, 1.0f, 1.2f, 0.8f, 1.0f, 0.3f};
            e->pres = 0.004f * vel * hard * BOWP[t];
        }
        e->vb = 0.08f + 0.1f * vel;
        e->k = 14.0f;
        break;
    case XT_BRUSH:
        mallet_shape(t, 3.0f, pos, 0.25f);   /* (the brush instead of most of the hit) */
        e->brush = 0.05f * vel;
        e->env = 1.0f;
        e->rel = 1.0f / ((0.03f + 0.09f / hard) * ENG_SR);
        break;
    default:
        mallet_shape(t, 1.0f / hard, pos, 1.0f);
        break;
    }
    if (e->type != XT_BOW && e->type != XT_BRUSH)
        e->env = 0.0f;
}

static inline float excite_push(int t)   /* the bow's or the brush's push now */
{
    exc_t *e = &EXC[t];
    if (e->env <= 0.0f && e->left <= 0.0f)
        return 0.0f;
    if (e->type == XT_BOW) {
        float dv, kd;
        if (e->left > 0.0f) {
            e->left -= 1.0f;
            e->env = fm_minf(e->env + e->att, 1.0f);
        } else {
            e->env -= e->rel;
            if (e->env <= 0.0f) {
                e->env = 0.0f;
                return 0.0f;
            }
        }
        dv = e->vb - *contact_v(t);
        kd = e->k * dv;
        return e->env * e->pres * kd / (1.0f + kd * kd);
    }
    if (e->type == XT_BRUSH) {
        e->env -= e->rel;
        if (e->env <= 0.0f) {
            e->env = 0.0f;
            return 0.0f;
        }
        return e->env * e->env * e->brush * white(&e->ns);
    }
    return 0.0f;
}

static void body_hit(int t, const float *p, float vel, uint32_t *ns)
{
    inst_state_t *s = &S[t];
    switch (t) {
    case I_KICK: kick_body_hit(&s->body.k, p, vel, ns); break;
    case I_SNARE: snare_body_hit(&s->body.s, p, vel, ns); break;
    case I_HATS: hats_body_hit(&s->body.h, p, vel, ns); break;
    case I_GLITCH: glitch_body_hit(&s->body.g, p, vel, ns); break;
    case I_PERC: perc_body_hit(&s->body.p, p, vel, ns); break;
    default: bass_body_hit(&s->body.b, p, vel, ns); break;
    }
}

/* THE WEB AT THE SAMPLE RATE: each body is pushed by the sound of the bodies tied to it (CHAOS's topology: wtie),
 * scaled by how easily it moves (SENS). A body rendered earlier on the same core pushes with this block's sound; the
 * others (the other core, or later) with the last block's (2.9 ms later: a tie of a few milliseconds' delay). */
static const float SENS[NINST] = {0.25f, 0.3f, 1.5f, 1.2f, 0.6f, 0.4f};
static const uint8_t CORE_OF[NINST] = {0, 0, 1, 0, 1, 1};
static const uint8_t ORDER[NINST] = {0, 1, 4, 2, 3, 5};   /* KICK first (both cores use it), then SNARE, GLITCH |
                                                           * PERC, HATS, BASS (about even, with the master on the first) */
static float prevbuf[NINST][BLK_MAX], prevpeak[NINST];
/* the pushes go one way only, from the kick outward: a body never pushes one that pushes it back (no loop: the web
 * can shake as hard as CHAOS likes, it can never run away) */
static const uint8_t RANK[NINST] = {0, 1, 3, 4, 2, 5};   /* KICK, SNARE, PERC, HATS, GLITCH, BASS */

static void render_inst(int t)
{
    inst_state_t *s = &S[t];
    float *out = ibuf[t], pk = 0.0f, gain[NINST];
    const float *src[NINST];
    uint32_t i, e = 0, n = blk_n, ns = 0, a;
    float own = 0.25f + 1.5f * (float)PAT->tr[t].fx[FX_CHAOS] / 127.0f;   /* its CHAOS: how easily it is pushed */
    float pmax = 0.0f;
    int fx_on;
    for (a = 0; a < NINST; a++)          /* who pushes it */
        if (RANK[a] < RANK[t] && wtie[a][t] > 0.0f) {
            int now = a == I_KICK || (CORE_OF[a] == CORE_OF[t] && ORDER[a] < ORDER[t]);
            src[ns] = now ? ibuf[a] : prevbuf[a];
            gain[ns] = couple * SENS[t] * wtie[a][t] * own * g_lk[a] * g_lk[t];
            pmax += gain[ns] * (now ? S[a].peak : prevpeak[a]);
            ns++;
        }
    /* asleep (silent a while, nothing struck, nothing pushing it): not computed at all */
    if (s->quiet >= SLEEP_BLOCKS && !nev[t] && pmax < 1e-6f && EXC[t].left <= 0.0f && EXC[t].env <= 0.0f) {
        for (i = 0; i < n; i++)
            out[i] = 0.0f;
        s->peak = 0.0f;
        return;
    }
    ifx_block(t);
    fx_on = IFX[t].fm_depth > 0.0f || IFX[t].g_mix > 0.0f || IFX[t].flt_mode;
    for (i = 0; i < n; i++) {
        float y, m, push = 0.0f;
        uint32_t j;
        while (e < nev[t] && ev[t][e].at <= i) {
            uint32_t c = plat_core() & 1u;
            float ws = g_wsc[c], ts = g_tsc[c];
            if (ev[t][e].semi != 0.0f) { /* MOTION's PITCH: this hit's time faster or slower */
                float d = fm_exp2f(ev[t][e].semi / 12.0f);
                g_wsc[c] = ws * d;
                g_tsc[c] = ts / d;
            }
            body_hit(t, ev[t][e].p, ev[t][e].vel, &s->noise);
            excite(t, ev[t][e].vel);
            g_wsc[c] = ws;
            g_tsc[c] = ts;
            e++;
        }
        for (j = 0; j < ns; j++)
            push += gain[j] * src[j][i];
        if (EXC[t].env > 0.0f || EXC[t].left > 0.0f)
            push += excite_push(t);
        switch (t) {
        case I_KICK: y = kick_body_sample(&s->body.k, push); break;
        case I_SNARE: y = snare_body_sample(&s->body.s, push); break;
        case I_HATS: y = hats_body_sample(&s->body.h, push); break;
        case I_GLITCH: y = glitch_body_sample(&s->body.g, push); break;
        case I_PERC: y = perc_body_sample(&s->body.p, push); break;
        default: y = bass_body_sample(&s->body.b, push); break;
        }
        if (fm_fabsf(y) > 1.5f)          /* (a safety: whatever a body does, never far over full scale) */
            y = 1.5f * fm_tanhf(y * (1.0f / 1.5f));
        if (fx_on)
            y = ifx_run(t, y);
        m = fm_fabsf(y);
        if (m > pk)
            pk = m;
        out[i] = y;
    }
    s->peak = pk;
    if (pk < QUIET && !nev[t]) {
        if (s->quiet < SLEEP_BLOCKS)
            s->quiet++;
    } else {
        s->quiet = 0;
    }
}

/* the second core's share: PERC, HATS, BASS (the first renders KICK, SNARE, GLITCH and the master) */
static void job_insts(uint32_t arg)
{
    (void)arg;
    render_inst(I_PERC);
    render_inst(I_HATS);
    render_inst(I_BASS);
}

void eng_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t i, t;
    int on2, k;
    float x;
    if (n > BLK_MAX)
        n = BLK_MAX;
    blk_n = n;
    if (play_req >= 0) {
        if (play_req) {
            step_pos = 0;
            step_left = 0.0f;
            running = 1;
            ext_run = 0;
            step_now = 0;
        } else {
            running = 0;
            ext_run = 0;
            step_now = -1;
        }
        play_req = -1;
    }
    if (!running && !ext_run)
        step_now = -1;
    if (step_now < 0 && pat_next)
        next_pattern();
    if (step_now < 0)                    /* stopped: what was drifting or bouncing is let go */
        for (t = 0; t < NINST; t++) {
            bc_wait[t] = 0.0f;
            for (k = 0; k < NPEND; k++)
                pend_wait[t][k] = 0.0f;
            waiting = 0;
        }
    g_step = step_base();
    if (shed)
        shed--;
    if (step_now >= 0)                   /* MOTION's clock */
        mo_pos += (float)n / fm_maxf(g_step, 1.0f);
    else
        mo_pos = 0.0f;
    g_tens = fm_exp2f(((float)PAT->web[WB_TENSION] - 64.0f) / 32.0f);   /* WEB: 0.25 .. 4 */
    g_damp = fm_exp2f(((float)PAT->web[WB_DAMPING] - 64.0f) / 40.0f);
    g_reach = fm_exp2f(((float)PAT->web[WB_REACH] - 64.0f) / 32.0f);
    for (t = 0; t < NINST; t++)
        g_lk[t] = (float)PAT->tr[t].link / 64.0f;
    master_block();
    pads_block(n);
    g_wsc[0] = g_wsc[1] = g_dil / ENG_SR;   /* PITCH (and the pads): time in the bodies, faster or slower */
    g_tsc[0] = g_tsc[1] = ENG_SR / g_dil;
    sequence(n);
    if (slam_req) {
        slam_req = 0;
        web_slam();
    }
    web_step((float)n / ENG_SR, (int)g_mass, (int)g_chaos);
    {
        float ch = g_chaos / 127.0f;
        couple = (0.0003f + 0.012f * ch * ch) * g_couple_x;   /* (gentle in the middle, wild at the top) */
    }
    /* the instruments: half on the second core, half here; if it cannot, all here */
    render_inst(I_KICK);                 /* (first: both cores' bodies hear it) */
    on2 = plat_cpu2_on() && plat_cpu2_run(job_insts, 0);
    render_inst(I_SNARE);
    render_inst(I_GLITCH);
    if (!on2 || plat_cpu2_wait() != 0)
        job_insts(0);
    for (t = 0; t < NINST; t++) {
        for (i = 0; i < n; i++)          /* the next block's pushes from this one */
            prevbuf[t][i] = ibuf[t][i];
        prevpeak[t] = S[t].peak;
        env_out[t] = S[t].peak > env_out[t] ? S[t].peak : env_out[t] * 0.85f;
        energy_out[t] = web_energy((int)t);
        for (k = 0; k < 4; k++)
            disp_out[t][k] = wd[t][k];
    }
    /* the mix and the master */
    for (i = 0; i < n; i++) {
        float L = 0.0f, R = 0.0f, rv = 0.0f, kick = ibuf[I_KICK][i], duck, ka = fm_fabsf(kick);
        fx.kick_env += (ka - fx.kick_env) * (ka > fx.kick_env ? 0.05f : 0.0003f);
        duck = 1.0f - 0.55f * fm_clampf(fx.kick_env * 2.0f, 0.0f, 1.0f);   /* the bass and the room under the kick */
        for (t = 0; t < NINST; t++) {
            float y = ibuf[t][i];
            if (y == 0.0f)
                continue;
            if (t == I_BASS)
                y *= duck;
            L += y * gl[t];
            R += y * gr[t];
            rv += y * SEND[t];
        }
        if (fx.ret > 0.0f) {             /* the reverb: pre-delay, four combs, two all-passes a side */
            float in, c0, c1, c2, c3;
            uint32_t pr = (fx.pi + PRE_N - 900u) % PRE_N;
            rv_pre[fx.pi] = rv * (HELD(P_THROW) ? 0.6f : 0.25f);
            in = rv_pre[pr];
            if (++fx.pi >= PRE_N)
                fx.pi = 0;
            c0 = comb(rv_comb0, 1116u, &fx.ci[0], &fx.cf[0], in);
            c1 = comb(rv_comb1, 1188u, &fx.ci[1], &fx.cf[1], in);
            c2 = comb(rv_comb2, 1277u, &fx.ci[2], &fx.cf[2], in);
            c3 = comb(rv_comb3, 1356u, &fx.ci[3], &fx.cf[3], in);
            L += allpass(rv_ap1, 441u, &fx.ai[1], allpass(rv_ap0, 556u, &fx.ai[0], c0 + c2 - 0.3f * c1)) * fx.ret * duck;
            R += allpass(rv_ap3, 464u, &fx.ai[3], allpass(rv_ap2, 579u, &fx.ai[2], c1 + c3 - 0.3f * c2)) * fx.ret * duck;
        }
        if (fx.d_g > 1.01f) {            /* DRIVE: the whole beat into a tanh */
            L = fm_tanhf(L * fx.d_g) * fx.d_norm;
            R = fm_tanhf(R * fx.d_g) * fx.d_norm;
        }
        /* SWEEP: a low-pass closing while held, opening when let go */
        if (sweep_x > 0.001f) {
            float lp, bp;
            svf_run(&fx.sw, &fx.sws[0], L, &lp, &bp);
            L = L + sweep_x * (lp - L);
            svf_run(&fx.sw, &fx.sws[1], R, &lp, &bp);
            R = R + sweep_x * (lp - R);
        }
        /* LO-FI: a quarter of the rate, 5 bits */
        if (HELD(P_LOFI)) {
            if (++fx.lf_n >= 4u) {
                fx.lf_n = 0;
                fx.lf_s[0] = fm_floorf(L * 16.0f + 0.5f) * (1.0f / 16.0f);
                fx.lf_s[1] = fm_floorf(R * 16.0f + 0.5f) * (1.0f / 16.0f);
            }
            L = fx.lf_s[0];
            R = fx.lf_s[1];
        }
        /* GATE: open the first half of each thirty-second */
        {
            float ph = (float)(gate_n++) * gate_inv, tg;
            ph -= fm_floorf(ph);
            tg = (!HELD(P_GATE) || ph < 0.5f) ? 1.0f : 0.0f;
            gate_g += (tg - gate_g) * 0.08f;
            L *= gate_g;
            R *= gate_g;
        }
        /* the mix kept (0.37 s): GLITCH's slices, TAPE STOP, REWIND, FREEZE read it back */
        gl_buf[0][gl_w] = (int16_t)(fm_clampf(L, -1.0f, 1.0f) * 32767.0f);
        gl_buf[1][gl_w] = (int16_t)(fm_clampf(R, -1.0f, 1.0f) * 32767.0f);
        {
            float lag = -1.0f;           /* how far behind the read is (-1: the live sound) */
            if (HELD(P_TAPE)) {          /* slower and slower, as a tape stopping */
                bf.speed = fm_maxf(bf.speed - 1.0f / (0.7f * ENG_SR), 0.0f);
                bf.lag = fm_minf(bf.lag + 1.0f - bf.speed, (float)GL_N - 4.0f);
                lag = bf.lag;
            } else if (HELD(P_REWIND)) { /* backwards, faster and faster */
                bf.speed = fm_minf(bf.speed + 1.0f / (0.25f * ENG_SR), 1.0f);
                bf.lag = fm_minf(bf.lag + 1.0f + bf.speed, (float)GL_N - 4.0f);
                lag = bf.lag;
            } else if (HELD(P_FREEZE)) { /* the last sixteenth, round and round */
                if (bf.frz_len == 0.0f) {   /* caught: the sixteenth that just went by */
                    bf.frz_len = fm_clampf(step_base(), 512.0f, (float)GL_N * 0.45f);
                    bf.frz_pos = 0.0f;
                    bf.frz_start = (float)gl_w - bf.frz_len;
                }
                bf.frz_pos += 1.0f;
                if (bf.frz_pos >= bf.frz_len)
                    bf.frz_pos -= bf.frz_len;
                lag = (float)gl_w - (bf.frz_start + bf.frz_pos);
                while (lag < 0.0f)
                    lag += (float)GL_N;
                while (lag >= (float)GL_N)
                    lag -= (float)GL_N;
                bf.slice_e = fm_minf(1.0f, fm_minf(bf.frz_pos, bf.frz_len - bf.frz_pos) * (1.0f / 48.0f));
            } else {                     /* none: the live sound */
                bf.speed = 1.0f;
                bf.lag = 0.0f;
                bf.frz_len = 0.0f;
            }
            if (lag < 0.0f && gl_left) {  /* GLITCH: a slice repeated */
                if (gl_at == i) {
                    gl_start = (gl_w + GL_N - gl_len) % GL_N;
                    gl_pos = 0;
                    gl_at = 0xFFFFu;
                }
                if (gl_at == 0xFFFFu) {
                    uint32_t o = gl_rev ? gl_len - 1u - gl_pos : gl_pos;
                    lag = (float)((gl_w + GL_N - (gl_start + o) % GL_N) % GL_N);
                    bf.slice_e = fm_minf(1.0f, fm_minf((float)gl_pos, (float)(gl_len - gl_pos)) * (1.0f / 64.0f));
                    if (++gl_pos >= gl_len)
                        gl_pos = 0;
                    gl_left--;
                }
            } else if (!HELD(P_FREEZE)) {
                bf.slice_e = 1.0f;
            }
            bf.mix += ((lag >= 0.0f ? 1.0f : 0.0f) - bf.mix) * 0.01f;   /* in and out in ~2 ms */
            if (lag >= 0.0f)
                bf.last_lag = lag;
            if (bf.mix > 0.001f) {
                float pos = (float)gl_w - bf.last_lag, fr, l, r, g;
                uint32_t r0, r1;
                while (pos < 0.0f)
                    pos += (float)GL_N;
                r0 = (uint32_t)pos % GL_N;
                fr = pos - fm_floorf(pos);
                r1 = (r0 + 1u) % GL_N;
                g = (HELD(P_TAPE) ? fm_minf(1.0f, bf.speed * 4.0f) : 1.0f) * bf.slice_e * (1.0f / 32768.0f);
                l = ((float)gl_buf[0][r0] + fr * (float)(gl_buf[0][r1] - gl_buf[0][r0])) * g;
                r = ((float)gl_buf[1][r0] + fr * (float)(gl_buf[1][r1] - gl_buf[1][r0])) * g;
                L += bf.mix * (l - L);
                R += bf.mix * (r - R);
            }
            if (++gl_w >= GL_N)
                gl_w = 0;
        }
        /* the limiter: clean below 0.6, rounded off above, never over full scale */
        if (fm_fabsf(L) > 0.6f) {
            x = 0.6f + 0.4f * fm_tanhf((fm_fabsf(L) - 0.6f) * 2.5f);
            L = L > 0.0f ? x : -x;
        }
        if (fm_fabsf(R) > 0.6f) {
            x = 0.6f + 0.4f * fm_tanhf((fm_fabsf(R) - 0.6f) * 2.5f);
            R = R > 0.0f ? x : -x;
        }
        gain_now += (gain - gain_now) * 0.004f;
        lr[2 * i] = (int32_t)(L * gain_now);
        lr[2 * i + 1] = (int32_t)(R * gain_now);
    }
}
