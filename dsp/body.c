/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE's sound engine, the PHYSICAL BODIES: instruments that are not oscillators but masses on springs, simulated
 * at the sample rate (44.1 kHz), as Anukari does. A mallet pushes a mass for a moment; the springs carry the push
 * around the body; a microphone picks up how fast the masses move. Nothing is played back: the sound is the body
 * moving. A new hit on a body still ringing just pushes it again (as on a real drum), and the bodies push each other:
 * the kick shakes the snare's head (its wires buzz), the snare shakes the hats.
 *
 * Units: one time step is one sample, so a spring's stiffness over its mass is w^2, w = 2 pi f / 44100 (f the
 * frequency it rings at on its own). Integrated with symplectic Euler (velocity first, then position): stable while
 * w < 2, which every spring here stays well under.
 *
 * A DRUM HEAD (KICK, SNARE): a heavy centre on a NONLINEAR spring (it stiffens the further it is pushed: a hard hit
 * starts high and falls to its note as it rings out, the pitch sweep, with nobody drawing an envelope; a drag takes
 * the big swings out fast) and a ring of six light masses round it, each on the rim, tied to the centre and to its
 * neighbours (the overtones, the knock). Where the mallet lands moves between the centre and the ring at each hit.
 * The SNARE adds its wires: eight tiny masses under the head that the head slaps (a contact spring that only pushes),
 * each ringing on its own and scraping (rough contact: a little noise while they touch): the rattle.
 * The HATS: a metal plate, eight stiff masses at inharmonic pitches tied by springs that harden (cubic): the energy
 * of a hit spreads from mode to mode, a shimmer (ties that give way when stretched: the force stays bounded).
 * Part of dsp/engine.c (included there). */
/* ============================================================ the bodies === */
#define KR 6                             /* a head's ring */

typedef struct {
    float x0, v0;                        /* the centre */
    float x[KR], v[KR];                  /* the ring */
    float k0, kg, nl, nd, kr, ks, kn;    /* stiffness: w0^2, the centre's to ground, its nonlinearity; drag; ring to
                                          * rim, ring to centre, ring to ring */
    float inv_r;                         /* 1 / a ring mass's mass (the centre's is 1) */
    float d0, dr;                        /* damping each sample: the centre's, the ring's */
    float w0;
    float m_amp, m_len, m_left, m_pos;   /* the mallet: a raised cosine of force; m_pos 0 the centre .. 1 the ring */
    int m_mass;                          /* the ring mass it lands by */
    float mic_ring;                      /* how much of the ring the microphone hears */
} head_t;

/* a head tuned to f0 (Hz), its ring at ratio x f0, a full hit at most `sweep` times above its note, drag, decay (s),
 * the ring's decay (s) */
static void head_set(head_t *b, float f0, float ratio, float sweep, float drag, float decay, float ring_decay)
{
    float w0 = FM_TWO_PI * f0 / ENG_SR, wr = w0 * ratio, r2 = sweep * sweep;
    b->w0 = w0;
    b->k0 = w0 * w0;
    b->nl = (r2 - 1.0f) * r2 * r2 / (0.81f * 0.81f);   /* y^2 (1 + nl y^4) = v^2 at the top of a full hit (v ~0.9) */
    b->nd = drag;
    b->inv_r = 1.0f / 0.25f;
    b->kr = wr * wr * 0.25f;
    b->ks = b->k0 * 0.06f;
    b->kg = b->k0 - (float)KR * b->ks;   /* (the ties to the ring stiffen the centre: the ground less) */
    b->kn = b->kr * 0.4f;
    b->d0 = fm_expf(-1.0f / (decay * ENG_SR));
    b->dr = fm_expf(-1.0f / (ring_decay * ENG_SR));
}

static void head_strike(head_t *b, float vel, float len, float pos, uint32_t *ns)
{
    b->m_len = b->m_left = len;
    b->m_amp = 2.0f * 0.9f * vel / len;  /* its impulse: the centre's speed ~0.9 * vel */
    b->m_pos = fm_clampf(pos, 0.0f, 0.9f);
    b->m_mass = (int)((0.5f + 0.5f * white(ns)) * (float)KR) % KR;
}

/* one sample: push is a force from outside on the centre (another body); f_ring[i] more on ring mass i (0: none) */
static inline void head_run(head_t *b, float push, const float *f_ring)
{
    float f0 = push, f[KR], y, k;
    int i;
    y = b->x0 * b->w0;                   /* the centre's spring: stiffer the further out (clamped inside stability) */
    y *= y;
    k = b->kg + b->k0 * b->nl * y * y;
    if (k > 0.5f)
        k = 0.5f;
    f0 -= k * b->x0;
    for (i = 0; i < KR; i++) {           /* each ring mass: to the rim, to the centre, to its two neighbours */
        float l = b->x[i == 0 ? KR - 1 : i - 1], r = b->x[i + 1 == KR ? 0 : i + 1], d = b->x[i] - b->x0;
        f[i] = -b->kr * b->x[i] - b->ks * d - b->kn * (2.0f * b->x[i] - l - r);
        f0 += b->ks * d;
        if (f_ring)
            f[i] += f_ring[i];
    }
    if (b->m_left > 0.0f) {              /* the mallet */
        float ph = 1.0f - b->m_left / b->m_len, m = b->m_amp * (0.5f - 0.5f * fm_cosf(FM_TWO_PI * ph));
        f0 += m * (1.0f - b->m_pos);
        f[b->m_mass] += m * b->m_pos * 0.25f;
        b->m_left -= 1.0f;
    }
    b->v0 = (b->v0 + f0) * b->d0;
    b->v0 /= 1.0f + b->nd * b->v0 * b->v0;
    b->x0 += b->v0;
    for (i = 0; i < KR; i++) {
        b->v[i] = (b->v[i] + f[i] * b->inv_r) * b->dr;
        b->x[i] += b->v[i];
    }
}

static inline float head_mic(const head_t *b) { return b->v0 + b->mic_ring * b->v[b->m_mass]; }

static int body_bad(float out, float x)  /* a body that ran away (never: but then it is stopped) */
{
    return !(out == out) || fm_fabsf(x) > 1e5f;
}
static void zero(void *p, uint32_t n)
{
    uint8_t *z = (uint8_t *)p;
    while (n--)
        *z++ = 0;
}

/* ----------------------------------------------------------------- KICK --- */
typedef struct {
    head_t h;
    float drive, makeup;
} kick_body_t;

static void kick_body_hit(kick_body_t *b, const float *p, float vel, uint32_t *ns)
{
    float tune = k01(p[0]), punch = k01(p[1]), decay = k01(p[2]), dirt = k01(p[3]);
    float f0 = 34.0f * fm_exp2f(tune * 1.35f);
    head_set(&b->h, f0, 2.2f + 1.6f * punch, 1.6f + 5.4f * punch, 0.002f + 0.02f * punch,
             0.06f + 1.4f * decay * decay, 0.008f + 0.05f * punch);
    b->h.mic_ring = 0.15f + 0.6f * punch;
    b->drive = 1.0f + 9.0f * dirt * dirt;
    b->makeup = 1.0f / (1.0f + 0.85f * dirt);
    head_strike(&b->h, vel, 2.0f + (1.0f - punch) * 180.0f,
                0.08f + 0.25f * (0.5f + 0.5f * white(ns)) + 0.004f * (p[1] - 64.0f), ns);
}

static inline float kick_body_sample(kick_body_t *b, float push)
{
    float out;
    head_run(&b->h, push, 0);
    out = drive_clip(head_mic(&b->h), b->drive) * b->makeup;
    if (body_bad(out, b->h.x0)) {
        zero(b, sizeof *b);
        return 0.0f;
    }
    return out;
}

/* ---------------------------------------------------------------- SNARE --- */
#define NW 8                             /* the wires */
static const float WIRE_HZ[NW] = {1130.0f, 1370.0f, 1690.0f, 1980.0f, 2410.0f, 2870.0f, 3390.0f, 4120.0f};

typedef struct {
    head_t h;
    float wx[NW], wv[NW];                /* the wires (in the head's units scaled by w0: ~1 at a full hit) */
    float wk[NW];                        /* their springs (w^2) */
    float wd;                            /* their damping a sample */
    float kc;                            /* the contact's stiffness */
    float gap;                           /* how far under the head they hang */
    float rough;                         /* the scrape: noise while touching */
    float snap;                          /* how loud the wires are */
    float drive, makeup;
    uint32_t ns;
} snare_body_t;

static void snare_body_hit(snare_body_t *b, const float *p, float vel, uint32_t *ns)
{
    float tune = k01(p[0]), snap = k01(p[1]), decay = k01(p[2]), dirt = k01(p[3]);
    float f0 = 150.0f * fm_exp2f((tune - 0.4f) * 1.6f), wt = fm_exp2f((tune - 0.4f) * 0.8f);
    int i;
    head_set(&b->h, f0, 1.59f, 1.35f, 0.01f, 0.05f + 0.25f * decay, 0.03f + 0.08f * decay);
    b->h.mic_ring = 0.5f;
    for (i = 0; i < NW; i++) {
        float w = FM_TWO_PI * WIRE_HZ[i] * wt / ENG_SR;
        b->wk[i] = w * w;
    }
    b->wd = fm_expf(-1.0f / ((0.004f + 0.03f * decay + 0.02f * snap) * ENG_SR));
    b->kc = 0.6f;
    b->gap = 0.02f * (1.0f - snap);      /* tight wires (SNAP up): touching at once */
    b->rough = 0.3f + 1.2f * snap;
    b->snap = 1.2f + 3.5f * snap;
    b->drive = 1.0f + 9.0f * dirt * dirt;
    b->makeup = 0.8f / (1.0f + 0.85f * dirt);
    b->ns = *ns;
    head_strike(&b->h, vel, 6.0f + (1.0f - snap) * 30.0f, 0.2f + 0.4f * (0.5f + 0.5f * white(ns)), ns);
}

static inline float snare_body_sample(snare_body_t *b, float push)
{
    float wire = 0.0f, scrape = 0.0f, out;
    int i;
    head_run(&b->h, push, 0);
    for (i = 0; i < NW; i++) {           /* each wire under ring mass i % 6: the head slaps it down, never pulls it
                                          * (a wire is so light the head does not feel it) */
        float hy = b->h.x[i % KR] * b->h.w0, pen = hy - b->wx[i] + b->gap, f = 0.0f;
        if (pen < 0.0f) {                /* touching: the contact pushes the wire down, and it scrapes */
            f = b->kc * pen;
            scrape -= pen;
        }
        b->wv[i] = (b->wv[i] - b->wk[i] * b->wx[i] + f) * b->wd;
        b->wx[i] += b->wv[i];
        wire += b->wv[i];
    }
    out = head_mic(&b->h) + (wire * (4.0f / NW) + scrape * b->rough * white(&b->ns) * 0.05f) * b->snap;
    out = drive_clip(out, b->drive) * b->makeup;
    if (body_bad(out, b->h.x0) || body_bad(wire, b->wx[0])) {
        zero(b, sizeof *b);
        return 0.0f;
    }
    return out;
}

/* ----------------------------------------------------------------- HATS --- */
#define NP 8                             /* the plate's masses */
static const float PLATE_MIC[NP] = {1.0f, -0.7f, 0.5f, 0.9f, -0.4f, 0.8f, -1.0f, 0.6f};   /* where the mic is */
static const float PLATE_HZ[NP] = {2810.0f, 3360.0f, 4070.0f, 4740.0f, 5390.0f, 6080.0f, 6800.0f, 7760.0f};

typedef struct {
    float x[NP], v[NP];
    float k[NP];                         /* each to its ground (w^2) */
    float kc, nl;                        /* the ties along the plate, their hardening */
    float d;                             /* damping a sample */
    float m_amp, m_len, m_left;          /* the stick */
    float hp, hp_a;                      /* the microphone's high-pass */
    float sizzle, ring;                  /* the noise the moving plate makes, how much of the plate itself */
    float s_env, s_hp;
    uint32_t ns;
    float drive, makeup;
} hats_body_t;

static void hats_body_hit(hats_body_t *b, const float *p, float vel, uint32_t *ns)
{
    float tune = k01(p[0]), decay = k01(p[1]), metal = k01(p[2]), dirt = k01(p[3]);
    float t = fm_exp2f((tune - 0.5f) * 1.0f);
    int i;
    for (i = 0; i < NP; i++) {
        float w = FM_TWO_PI * fm_minf(PLATE_HZ[i] * t * (1.0f + 0.01f * white(ns)), 8000.0f) / ENG_SR;
        b->k[i] = w * w;
    }
    b->kc = 0.01f + 0.08f * metal;
    b->nl = 2.0f + 60.0f * metal * metal;
    b->d = fm_expf(-1.0f / ((0.012f + 0.6f * decay * decay * decay) * ENG_SR));
    b->m_len = b->m_left = 3.0f;
    b->m_amp = 2.0f * 1.6f * vel / 3.0f;
    b->hp_a = onepole(3000.0f * t);
    b->sizzle = 2.6f * (1.0f - 0.8f * metal);
    b->ring = 0.35f + 0.9f * metal;
    b->ns = *ns;
    b->drive = 1.0f + 9.0f * dirt * dirt;
    b->makeup = 1.0f / (1.0f + 0.85f * dirt);
}

static inline float hats_body_sample(hats_body_t *b, float push)
{
    float f[NP], m = 0.0f, out, ph = 0.0f;
    int i;
    for (i = 0; i < NP; i++)
        f[i] = -b->k[i] * b->x[i];
    for (i = 0; i < NP; i++) {           /* the ties across the plate (to the next and the third), nonlinear: the
                                          * energy spreads from mode to mode, the shimmer */
        int j;
        for (j = 1; j <= 3; j += 2) {
            int o = (i + j) % NP;
            float d = b->x[i] - b->x[o], t = b->kc * d / (1.0f + b->nl * d * d);   /* (it gives way when
                                          * stretched: a bounded force, so the plate can never gain energy) */
            f[i] -= t;
            f[o] += t;
        }
    }
    f[0] += push;
    if (b->m_left > 0.0f) {              /* the stick, on two masses */
        ph = 1.0f - b->m_left / b->m_len;
        ph = b->m_amp * (0.5f - 0.5f * fm_cosf(FM_TWO_PI * ph));
        f[0] += ph;
        f[3] += 0.5f * ph;
        b->m_left -= 1.0f;
    }
    for (i = 0; i < NP; i++) {
        b->v[i] = (b->v[i] + f[i]) * b->d;
        b->x[i] += b->v[i];
        m += b->v[i] * PLATE_MIC[i];
    }
    b->hp += b->hp_a * (m - b->hp);
    m -= b->hp;
    /* the sizzle: noise as loud as the plate moves (its many small modes, the air), high-passed */
    b->s_env += 0.01f * (fm_fabsf(m) - b->s_env);
    {
        float n = white(&b->ns) * b->s_env;
        b->s_hp += 0.35f * (n - b->s_hp);
        n -= b->s_hp;
        out = drive_clip((m * b->ring + n * b->sizzle) * 0.3f, b->drive) * b->makeup;
    }
    if (body_bad(out, b->x[0])) {
        zero(b, sizeof *b);
        return 0.0f;
    }
    return out;
}
