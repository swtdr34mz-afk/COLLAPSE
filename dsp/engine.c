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

#include "voices.c"

#include "web.c"

/* =============================================================== the state === */
static const pattern_t *PAT;
static volatile const pattern_t *pat_next;
static pattern_t pat0;                   /* (until the app binds its own) */

typedef struct {
    voice_t v[2];                        /* two voices: a new hit takes the other one, the old one fades */
    uint8_t next;
    float peak;                          /* the block's peak (the screen's) */
    uint32_t noise;
} inst_state_t;
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
    for (t = 0; t < NINST; t++)
        S[t].v[0].on = S[t].v[1].on = 0;
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
static int32_t pat_len(void)
{
    int32_t l = PAT->length;
    return l == 32 || l == 48 || l == 64 ? l : 16;
}


static float rt_wait[NINST], rt_vel[NINST];   /* a stutter's second hit: samples to it (0: none) */
static void play_step(int32_t s, uint32_t at)
{
    uint32_t t;
    float ch = (float)PAT->chaos;
    step_now = s;
    for (t = 0; t < NINST; t++) {
        const track_t *tr = &PAT->tr[t];
        float vel;
        if (!step_on(tr, s) || (muted >> t & 1u))
            continue;
        vel = step_acc(tr, s) ? 1.0f : 0.68f;
        push_ev((int)t, at, vel);
        /* CHAOS at the top: SNARE, HATS, GLITCH and PERC sometimes stutter (a second hit in the step) */
        if (ch > 100.0f && t != I_KICK && t != I_BASS
            && 0.5f + 0.5f * white(&seq_noise) < (ch - 100.0f) * (0.4f / 27.0f)) {
            rt_wait[t] = step_base() * 0.5f;
            rt_vel[t] = vel * 0.7f;
        }
    }
}

static void next_pattern(void)           /* at the top of a loop */
{
    if (pat_next) {
        PAT = (const pattern_t *)pat_next;
        pat_next = 0;
    }
}

/* the sequencer over the block: where each instrument is struck */
static void sequence(uint32_t n)
{
    uint32_t i, t;
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
            if (ext_run && ext_count % 6u == 0u) {
                if (step_pos >= pat_len())
                    step_pos = 0;
                if (step_pos == 0)
                    next_pattern();
                play_step(step_pos, 0);
                step_pos++;
            }
            ext_count++;
        }
    } else if (ext_quiet < 0x7FFFFFFFu) {
        ext_quiet += n;
    }
    for (i = 0; i < n; i++) {
        for (t = 0; t < NINST; t++)
            if (rt_wait[t] > 0.0f && (rt_wait[t] -= 1.0f) <= 0.0f)
                push_ev((int)t, i, rt_vel[t]);
        if (!running)
            continue;
        step_left -= 1.0f;
        if (step_left <= 0.0f) {
            if (step_pos >= pat_len())
                step_pos = 0;
            if (step_pos == 0)
                next_pattern();
            play_step(step_pos, i);
            step_left += step_len(step_pos);
            step_pos++;
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
static const float TRIM[NINST] = {0.95f, 0.75f, 0.42f, 0.5f, 0.6f, 0.62f};
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
    pat0.length = 16;
    PAT = &pat0;
    web_topology(eng_topology(64));
}

#define BLK_MAX 256u
static float ibuf[NINST][BLK_MAX];
static uint32_t blk_n;

static void render_inst(int t)
{
    inst_state_t *s = &S[t];
    float *out = ibuf[t], pk = 0.0f;
    uint32_t i, e = 0, n = blk_n;
    for (i = 0; i < n; i++) {
        float y = 0.0f, a;
        while (e < nev[t] && ev[t][e].at <= i) {   /* a hit: the other voice, the old one fades in 4 ms */
            voice_t *v = &s->v[s->next];
            s->v[s->next ^ 1u].k_fade = coef(0.004f);
            voice_start(t, v, ev[t][e].p, ev[t][e].vel);
            s->next ^= 1u;
            e++;
        }
        if (s->v[0].on)
            y += voice_sample(t, &s->v[0], &s->noise);
        if (s->v[1].on)
            y += voice_sample(t, &s->v[1], &s->noise);
        a = fm_fabsf(y);
        if (a > pk)
            pk = a;
        out[i] = y;
    }
    s->peak = pk;
}

/* the second core's share: GLITCH, PERC, BASS (the first renders KICK, SNARE, HATS and the master) */
static void job_insts(uint32_t arg)
{
    (void)arg;
    render_inst(I_GLITCH);
    render_inst(I_PERC);
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
    g_step = step_base();
    master_block();
    sequence(n);
    if (slam_req) {
        slam_req = 0;
        web_slam();
    }
    web_step((float)n / ENG_SR, PAT->mass, PAT->chaos);
    /* the instruments: half on the second core, half here; if it cannot, all here */
    on2 = plat_cpu2_on() && plat_cpu2_run(job_insts, 0);
    render_inst(I_KICK);
    render_inst(I_SNARE);
    render_inst(I_HATS);
    if (!on2 || plat_cpu2_wait() != 0)
        job_insts(0);
    for (t = 0; t < NINST; t++) {
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
