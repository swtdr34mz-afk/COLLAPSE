/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE's sound engine, the six instruments. Each is struck with its four knobs as the web has pushed them (0..127
 * each, worked out at the hit: a hit never changes under itself) and renders a sample at a time. Part of dsp/engine.c
 * (included there: the device builds the engine as one unit). */
/* ============================================================ the instruments === */
typedef struct {
    uint8_t on;
    float vel, fade, k_fade, makeup;
    float ph[6], inc[6];                 /* oscillators */
    float e_amp, k_amp, e_pitch, k_pitch, e_noise, k_noise, e_click, k_click, e_filt, k_filt;
    float hz, sweep, body, noise, click, metal;
    float drive, fold;
    float lp, lp_a, hp, hp_a, hpx;
    svf_c f1, f2;
    svf_s s1, s2;
    float crush_hold, crush_q, crush_n, crush_s;
    float gate_ph, gate_inc, sh, jump;   /* GLITCH: the stutter's gate, its random pitch */
    float lfo_ph, lfo_steps, lfo_depth, cut, env_oct, q, detune;   /* BASS */
    uint32_t n;                          /* samples since the hit */
} voice_t;

static float g_step;                     /* samples a step, now (the render's; the BASS's wobble follows it) */

static inline float k01(float v) { return fm_clampf(v, 0.0f, 127.0f) * (1.0f / 127.0f); }

/* the dirt every instrument has: drive into a soft clip, a fold at the top, and a bit crusher */
static void dirt_set(voice_t *v, float dirt, float crush_from)
{
    float d = k01(dirt);
    v->drive = 1.0f + 10.0f * d * d;
    v->fold = d > 0.7f ? (d - 0.7f) / 0.3f : 0.0f;
    v->makeup = 1.0f / (1.0f + 0.9f * d);
    if (d > crush_from) {
        float c = (d - crush_from) / (1.0f - crush_from);
        v->crush_hold = 1.0f + 6.0f * c;
        v->crush_q = 128.0f - 118.0f * c;
    } else {
        v->crush_hold = 1.0f;
        v->crush_q = 0.0f;
    }
}

static inline float dirt_run(voice_t *v, float x)
{
    float y = drive_clip(x, v->drive);
    if (v->fold > 0.0f)
        y += v->fold * (fm_sinf(y * 4.0f) * 0.8f - y);
    if (v->crush_q > 0.0f) {
        v->crush_n += 1.0f;
        if (v->crush_n >= v->crush_hold) {
            v->crush_n -= v->crush_hold;
            v->crush_s = fm_floorf(y * v->crush_q + 0.5f) / v->crush_q;
        }
        y = v->crush_s;
    }
    return y * v->makeup;
}

static void voice_clear(voice_t *v, float vel)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)v;
    for (i = 0; i < sizeof *v; i++)
        b[i] = 0;
    v->on = 1;
    v->vel = vel;
    v->fade = v->k_fade = 1.0f;
}

/* ----------------------------------------------------------------- KICK --- */
static void kick_start(voice_t *v, const float *p)
{
    float pu = k01(p[1]), de = k01(p[2]);
    v->hz = 32.0f * fm_exp2f(k01(p[0]) * 1.4f);
    v->sweep = 1.2f + 4.2f * pu;
    v->k_pitch = coef(0.040f - 0.030f * pu);
    v->click = 0.15f + 0.85f * pu;
    v->k_click = coef(0.0015f);
    v->k_amp = coef(0.07f + 1.3f * de * de);
    v->e_amp = v->e_pitch = v->e_click = 1.0f;
    dirt_set(v, p[3], 0.85f);
    v->makeup *= 0.95f;
}

static inline float kick_sample(voice_t *v, uint32_t *ns)
{
    float n, x;
    v->ph[0] += v->hz * fm_exp2f(v->sweep * v->e_pitch) * (1.0f / ENG_SR);
    if (v->ph[0] >= 1.0f)
        v->ph[0] -= 1.0f;
    x = fm_sin_turns(v->ph[0]) * v->e_amp;
    if (v->e_click > 0.001f) {           /* the click: a high-passed burst of noise */
        n = white(ns);
        x += (n - v->hpx) * 0.45f * v->click * v->e_click;
        v->hpx = n;
    }
    x = dirt_run(v, x);
    v->e_amp *= v->k_amp;
    v->e_pitch *= v->k_pitch;
    v->e_click *= v->k_click;
    if (v->e_amp < 0.0003f)
        v->on = 0;
    return x;
}

/* ---------------------------------------------------------------- SNARE --- */
static void snare_start(voice_t *v, const float *p)
{
    float sn = k01(p[1]), de = k01(p[2]);
    v->hz = 150.0f * fm_exp2f((k01(p[0]) - 0.4f) * 1.6f);
    v->noise = 0.35f + 0.9f * sn;
    v->body = 1.0f - 0.6f * sn;
    v->k_pitch = coef(0.018f);
    v->k_amp = coef(0.045f + 0.18f * de);
    v->k_noise = coef(0.035f + 0.45f * de * de);
    v->e_amp = v->e_pitch = v->e_noise = 1.0f;
    svf_set(&v->f1, 1400.0f + 5200.0f * sn, 0.9f);
    dirt_set(v, p[3], 0.6f);
    v->makeup *= 0.9f;
}

static inline float snare_sample(voice_t *v, uint32_t *ns)
{
    float f = v->hz * (1.0f + 0.35f * v->e_pitch), lp, bp, hp, x;
    v->ph[0] += f * (1.0f / ENG_SR);
    if (v->ph[0] >= 1.0f)
        v->ph[0] -= 1.0f;
    v->ph[1] += f * 1.62f * (1.0f / ENG_SR);
    if (v->ph[1] >= 1.0f)
        v->ph[1] -= 1.0f;
    hp = svf_run(&v->f1, &v->s1, white(ns), &lp, &bp);
    x = (fm_sin_turns(v->ph[0]) * 0.75f + fm_sin_turns(v->ph[1]) * 0.3f) * v->body * v->e_amp
        + (hp * 0.6f + bp * 0.7f) * v->noise * v->e_noise;
    x = dirt_run(v, x);
    v->e_amp *= v->k_amp;
    v->e_noise *= v->k_noise;
    v->e_pitch *= v->k_pitch;
    if (v->e_amp < 0.0003f && v->e_noise < 0.0003f)
        v->on = 0;
    return x;
}

/* ----------------------------------------------------------------- HATS --- */
static const float HAT_HZ[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};   /* the 808's six squares */

static void hats_start(voice_t *v, const float *p)
{
    float t = fm_exp2f((k01(p[0]) - 0.4f) * 2.0f), de = k01(p[1]), me = k01(p[2]);
    uint32_t i;
    for (i = 0; i < 6u; i++)
        v->inc[i] = HAT_HZ[i] * t * 1.7f * (1.0f / ENG_SR);
    v->metal = 0.15f + 0.85f * me;
    v->noise = 1.0f - 0.75f * me;
    v->k_amp = coef(0.010f + 0.55f * de * de * de);
    v->e_amp = 1.0f;
    svf_set(&v->f1, fm_clampf(7500.0f * t, 3000.0f, 16000.0f), 1.1f + 2.5f * me);
    v->hp_a = onepole(fm_clampf(6000.0f * t, 2500.0f, 12000.0f));
    dirt_set(v, p[3], 0.35f);
}

static inline float hats_sample(voice_t *v, uint32_t *ns)
{
    float m = 0.0f, lp, bp, x;
    uint32_t i;
    for (i = 0; i < 6u; i++) {
        v->ph[i] += v->inc[i];
        if (v->ph[i] >= 1.0f)
            v->ph[i] -= 1.0f;
        m += v->ph[i] < 0.5f ? 1.0f : -1.0f;
    }
    x = m * (1.0f / 6.0f) * v->metal + white(ns) * v->noise;
    svf_run(&v->f1, &v->s1, x, &lp, &bp);
    v->hp += v->hp_a * (bp - v->hp);
    x = (bp - v->hp) * 2.2f * v->e_amp;
    x = dirt_run(v, x);
    v->e_amp *= v->k_amp;
    if (v->e_amp < 0.0005f)
        v->on = 0;
    return x;
}

/* --------------------------------------------------------------- GLITCH --- */
/* a square that a gate chops into stutters; each chop jumps to a random pitch (more the higher PITCH goes: DATA) */
static void glitch_start(voice_t *v, const float *p)
{
    float ra = k01(p[0]), pi = k01(p[1]), le = k01(p[2]);
    v->gate_inc = 7.0f * fm_exp2f(ra * 4.6f) * (1.0f / ENG_SR);
    v->hz = 90.0f * fm_exp2f(pi * 5.2f);
    v->jump = 0.1f + 1.4f * pi * pi;
    v->k_amp = coef(0.015f + 0.7f * le * le);
    v->e_amp = 1.0f;
    v->gate_ph = 0.999f;                 /* a new chop at once */
    v->lp_a = onepole(2500.0f + 9000.0f * pi);
    dirt_set(v, p[3], 0.0f);
    v->drive = 1.4f + v->drive * 0.4f;
    v->makeup = 0.75f;
}

static inline float glitch_sample(voice_t *v, uint32_t *ns)
{
    float x;
    v->gate_ph += v->gate_inc;
    if (v->gate_ph >= 1.0f) {
        v->gate_ph -= 1.0f;
        v->sh = white(ns);
    }
    v->ph[0] += v->hz * fm_exp2f(v->sh * v->jump) * (1.0f / ENG_SR);
    if (v->ph[0] >= 1.0f)
        v->ph[0] -= 1.0f;
    x = (v->ph[0] < 0.5f ? 0.6f : -0.6f) * (v->gate_ph < 0.55f ? 1.0f : 0.0f);
    v->lp += v->lp_a * (x - v->lp);
    x = dirt_run(v, v->lp * v->e_amp);
    v->e_amp *= v->k_amp;
    if (v->e_amp < 0.0005f)
        v->on = 0;
    return x;
}

/* ----------------------------------------------------------------- PERC --- */
/* a knock into two resonators (LXR-style modal): RESO from wood to glass moves their ratio and their ring */
static void perc_start(voice_t *v, const float *p)
{
    float re = k01(p[2]), de = k01(p[1]), hz = 70.0f * fm_exp2f(k01(p[0]) * 5.0f), q = 4.0f + 70.0f * re * re;
    svf_set(&v->f1, hz, q);
    svf_set(&v->f2, hz * (1.48f + 1.28f * re), q * 0.8f);
    v->q = q;
    v->k_amp = coef(0.03f + 1.2f * de * de);
    v->k_noise = coef(0.0025f);
    v->e_amp = v->e_noise = 1.0f;
    v->click = 1.0f;
    v->metal = 0.25f + 0.55f * re;
    dirt_set(v, p[3], 0.6f);
    v->makeup *= 1.6f + 3.0f / q;
}

static inline float perc_sample(voice_t *v, uint32_t *ns)
{
    float ex = white(ns) * v->e_noise + v->click, lp, bp, b2, x;
    v->click = 0.0f;
    svf_run(&v->f1, &v->s1, ex, &lp, &bp);
    svf_run(&v->f2, &v->s2, ex, &lp, &b2);
    x = (bp + b2 * v->metal) * (2.0f / v->q + 0.05f) * 3.0f * v->e_amp;
    x = dirt_run(v, x);
    v->e_amp *= v->k_amp;
    v->e_noise *= v->k_noise;
    if (v->e_amp < 0.0003f)
        v->on = 0;
    return x;
}

/* ----------------------------------------------------------------- BASS --- */
/* a reese: two saws a little apart and a sub, through a low-pass that an envelope opens and the WOBBLE swings, in time */
static const bp_t B_NOTE[] = {{0, 0.0f}, {32, 7.0f}, {64, 12.0f}, {96, 19.0f}, {127, 24.0f}};

static inline float blep(float t, float dt)   /* a saw's step, smoothed (polyBLEP) */
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static void bass_start(voice_t *v, const float *p)
{
    float semi = fm_floorf(CURVE(B_NOTE, p[0]) + 0.5f), de = k01(p[1]), wo = k01(p[2]), di = k01(p[3]);
    v->hz = 41.2034f * fm_exp2f(semi / 12.0f);   /* E1 and up */
    v->detune = 0.002f + 0.012f * di;
    v->inc[0] = v->hz * (1.0f + v->detune) * (1.0f / ENG_SR);
    v->inc[1] = v->hz * (1.0f - v->detune) * (1.0f / ENG_SR);
    v->inc[2] = v->hz * (1.0f / ENG_SR);
    v->ph[1] = 0.37f;
    v->k_amp = coef(0.09f + 2.4f * de * de);
    v->e_amp = v->e_filt = 1.0f;
    v->k_filt = coef(0.05f + 0.25f * de);
    v->lfo_depth = wo < 0.1f ? 0.0f : 0.5f + 3.0f * wo;
    v->lfo_steps = wo < 0.37f ? 4.0f : wo < 0.63f ? 2.0f : wo < 0.88f ? 1.0f : 0.5f;
    v->q = 0.8f + 2.5f * wo * wo + 1.5f * di;
    v->env_oct = 2.0f + 2.0f * di;
    v->cut = 2.6f + 1.2f * di;           /* octaves over the note */
    v->lfo_ph = 0.25f;                   /* the wobble starts open */
    dirt_set(v, p[3] * 0.8f, 0.9f);
    v->makeup *= 0.85f;
}

static inline float bass_sample(voice_t *v, uint32_t *ns)
{
    float a, b, x, lp, bp, l;
    (void)ns;
    v->ph[0] += v->inc[0];
    if (v->ph[0] >= 1.0f)
        v->ph[0] -= 1.0f;
    v->ph[1] += v->inc[1];
    if (v->ph[1] >= 1.0f)
        v->ph[1] -= 1.0f;
    v->ph[2] += v->inc[2];
    if (v->ph[2] >= 1.0f)
        v->ph[2] -= 1.0f;
    a = 2.0f * v->ph[0] - 1.0f - blep(v->ph[0], v->inc[0]);
    b = 2.0f * v->ph[1] - 1.0f - blep(v->ph[1], v->inc[1]);
    x = (a + b) * 0.35f + fm_sin_turns(v->ph[2]) * 0.55f;
    if ((v->n & 15u) == 0u) {            /* the filter, every 16th sample */
        if (v->lfo_depth > 0.0f) {
            v->lfo_ph += 16.0f / (v->lfo_steps * fm_maxf(g_step, 1000.0f));
            if (v->lfo_ph >= 1.0f)
                v->lfo_ph -= 1.0f;
            l = 0.5f + 0.5f * fm_sin_turns(v->lfo_ph);
        } else {
            l = 0.0f;
        }
        svf_set(&v->f1, v->hz * fm_exp2f(v->cut + v->env_oct * v->e_filt + v->lfo_depth * (l - 0.6f)), v->q);
    }
    v->n++;
    svf_run(&v->f1, &v->s1, x, &lp, &bp);
    x = dirt_run(v, lp * v->e_amp);
    v->e_amp *= v->k_amp;
    v->e_filt *= v->k_filt;
    if (v->e_amp < 0.0003f)
        v->on = 0;
    return x;
}

/* ------------------------------------------------------- one interface --- */
static void voice_start(int t, voice_t *v, const float *p, float vel)
{
    voice_clear(v, vel);
    switch (t) {
    case I_KICK: kick_start(v, p); break;
    case I_SNARE: snare_start(v, p); break;
    case I_HATS: hats_start(v, p); break;
    case I_GLITCH: glitch_start(v, p); break;
    case I_PERC: perc_start(v, p); break;
    default: bass_start(v, p); break;
    }
}

static inline float voice_sample(int t, voice_t *v, uint32_t *ns)
{
    float y;
    switch (t) {
    case I_KICK: y = kick_sample(v, ns); break;
    case I_SNARE: y = snare_sample(v, ns); break;
    case I_HATS: y = hats_sample(v, ns); break;
    case I_GLITCH: y = glitch_sample(v, ns); break;
    case I_PERC: y = perc_sample(v, ns); break;
    default: y = bass_sample(v, ns); break;
    }
    y *= v->vel * v->fade;
    v->fade *= v->k_fade;
    if (v->fade < 0.0005f)
        v->on = 0;
    return y;
}
