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

/* a hit queued at sample `at` of the block: its knobs now (as set, pushed by the web, moved by MASS), then its push */
static void push_ev(int t, uint32_t at, float vel)
{
    ev_t *e;
    if (nev[t] >= NEV)
        return;
    e = &ev[t][nev[t]++];
    e->at = (uint16_t)at;
    e->vel = vel;
    web_knobs(t, PAT->tr[t].p, PAT->mass, e->p);
    web_push(t, vel, PAT->mass, PAT->chaos);
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
    float sb = step_base(), dr = (float)PAT->drift / 127.0f, mass = (float)PAT->mass / 127.0f;
    step_now = s;
    for (t = 0; t < NINST; t++) {
        const track_t *tr = &PAT->tr[t];
        int32_t ts = s % trk_len(tr);
        float vel, delay, c = (float)tr->fx[FX_CHAOS] / 127.0f;
        if (!step_on(tr, ts) || (muted >> t & 1u))
            continue;
        vel = step_acc(tr, ts) ? 1.0f : 0.68f;
        delay = sb * 0.6f * dr * dr
                * fm_clampf(0.35f * rnd01() + 0.65f * (fm_fabsf(wd[t][1]) + fm_fabsf(wd[t][2])) * (1.0f / 40.0f), 0.0f, 1.0f);
        if (delay < 1.0f)
            push_ev((int)t, at, vel);
        else
            later((int)t, (float)at + delay, vel);
        if (c > 0.0f && rnd01() < c * c * 0.9f + 0.1f * c) {   /* a bounce: its first rebound, its fall */
            bc_gap[t] = sb * (1.1f - 0.7f * mass) * (0.55f + 0.45f * rnd01());
            bc_e[t] = 0.5f + 0.42f * c - 0.1f * mass;
            bc_vel[t] = vel * (0.6f + 0.3f * c);
            bc_wait[t] = delay + bc_gap[t];
            waiting |= (uint8_t)(1u << t);
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

/* GLITCH (the master's): at a step, by chance (more the further it is turned), the last slice of the mix is caught
 * and repeated for a step or two, sometimes backwards; turned up the slices get shorter (SKIP, STUTTER, SHRED) */
#define GL_N 8192u
static int16_t gl_buf[2][GL_N] PLAT_POOL;
static uint32_t gl_w, gl_start, gl_len, gl_pos, gl_left, gl_at = 0xFFFFu;
static uint8_t gl_rev;
static uint32_t gl_noise = 0x7F4A7C15u;
static void glitch_maybe(uint32_t at)
{
    float g = (float)PAT->glitch / 127.0f, r = 0.5f + 0.5f * white(&gl_noise), sb = step_base(), len;
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
    float sp = (float)PAT->space / 127.0f, dr = (float)PAT->drive / 127.0f;
    uint32_t t;
    fx.fb = 0.62f + 0.3f * sp;
    fx.damp = 0.45f - 0.25f * sp;
    fx.ret = sp < 0.02f ? 0.0f : 0.25f + 0.9f * sp;
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
    }
    pat0.pitch = 64;
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
    }
    return y;
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
            gain[ns] = couple * SENS[t] * wtie[a][t] * own;
            pmax += gain[ns] * (now ? S[a].peak : prevpeak[a]);
            ns++;
        }
    /* asleep (silent a while, nothing struck, nothing pushing it): not computed at all */
    if (s->quiet >= SLEEP_BLOCKS && !nev[t] && pmax < 1e-6f) {
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
            body_hit(t, ev[t][e].p, ev[t][e].vel, &s->noise);
            e++;
        }
        for (j = 0; j < ns; j++)
            push += gain[j] * src[j][i];
        switch (t) {
        case I_KICK: y = kick_body_sample(&s->body.k, push); break;
        case I_SNARE: y = snare_body_sample(&s->body.s, push); break;
        case I_HATS: y = hats_body_sample(&s->body.h, push); break;
        case I_GLITCH: y = glitch_body_sample(&s->body.g, push); break;
        case I_PERC: y = perc_body_sample(&s->body.p, push); break;
        default: y = bass_body_sample(&s->body.b, push); break;
        }
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
    master_block();
    {                                    /* PITCH: time in the bodies, faster or slower */
        static const bp_t P_PITCH[] = {{0, -12.0f}, {32, -5.0f}, {64, 0.0f}, {96, 7.0f}, {127, 12.0f}};
        float dil = fm_exp2f(CURVE(P_PITCH, PAT->pitch) / 12.0f);
        g_wscale = dil / ENG_SR;
        g_tscale = ENG_SR / dil;
    }
    sequence(n);
    if (slam_req) {
        slam_req = 0;
        web_slam();
    }
    web_step((float)n / ENG_SR, PAT->mass, PAT->chaos);
    {
        float ch = (float)PAT->chaos / 127.0f;
        couple = 0.0003f + 0.012f * ch * ch;   /* (gentle in the middle, wild at the top) */
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
            rv_pre[fx.pi] = rv * 0.25f;
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
        /* GLITCH: the mix kept; a slice of it repeated while one is caught */
        if (PAT->glitch || gl_left) {
            gl_buf[0][gl_w] = (int16_t)(fm_clampf(L, -1.0f, 1.0f) * 32767.0f);
            gl_buf[1][gl_w] = (int16_t)(fm_clampf(R, -1.0f, 1.0f) * 32767.0f);
            if (gl_at == i) {            /* caught: the slice that just went by */
                gl_start = (gl_w + GL_N - gl_len) % GL_N;
                gl_pos = 0;
                gl_at = 0xFFFFu;
            }
            if (gl_left && gl_at == 0xFFFFu) {
                uint32_t o = gl_rev ? gl_len - 1u - gl_pos : gl_pos, r = (gl_start + o) % GL_N;
                float e = fm_minf(1.0f, (float)fm_minf((float)gl_pos, (float)(gl_len - gl_pos)) * (1.0f / 64.0f));
                float g = e * (1.0f / 32768.0f);
                L = (float)gl_buf[0][r] * g;
                R = (float)gl_buf[1][r] * g;
                if (++gl_pos >= gl_len)
                    gl_pos = 0;
                gl_left--;
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
