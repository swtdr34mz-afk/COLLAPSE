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
 * The PERC: three masses tuned to a struck object's modes (wood, skin, metal, glass), tied loosely: an LXR's modal
 * voice, built from springs.
 * The BASS: a string, sixteen masses in a row between two fixed ends, plucked near one end; its tension rises with how
 * far it is pulled (a hard pluck starts sharp and growls); the WOBBLE moves the pickup along the string in time.
 * The GLITCH: a ball dropped on a small plate. It bounces, each bounce shorter than the last (an accelerating stutter);
 * every impact strikes the plate (a click at the plate's pitch); at DATA each impact retunes the plate. When another
 * body shakes the plate, the ball dances on it.
 * Part of dsp/engine.c (included there). */
/* ============================================================ the bodies === */
static float g_step;                     /* samples a step, now (the render's: the BASS's wobble follows it) */
/* PITCH, as Anukari's: time in the bodies runs faster or slower. Every frequency is scaled by it and every decay
 * shortened by it, as if the body were played back faster: (w = 2 pi f g_wscale, a decay of tau seconds lasts
 * tau g_tscale samples) */
static float g_wsc[2] = {1.0f / ENG_SR, 1.0f / ENG_SR}, g_tsc[2] = {ENG_SR, ENG_SR};   /* (each core its own: a hit
                                          * may be tuned for itself, MOTION's PITCH, while the other core strikes) */
#define g_wscale g_wsc[plat_core() & 1u]
#define g_tscale g_tsc[plat_core() & 1u]
#define WMAX 1.2f                        /* (a spring never rings above this w, whatever PITCH does: stable) */
static inline float k01(float v) { return fm_clampf(v, 0.0f, 127.0f) * (1.0f / 127.0f); }

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
/* a body retuned while it rings keeps its energy: its swing scaled by sqrt(k before / k now) (a stiffer spring with
 * the same swing would hold more energy: retuned often, a body would be pumped up) */
static inline float keep(float k_was, float k_now) { return k_was > 0.0f && k_now > 0.0f ? fm_sqrtf(k_was / k_now) : 1.0f; }

static void head_set(head_t *b, float f0, float ratio, float sweep, float drag, float decay, float ring_decay)
{
    float w0 = FM_TWO_PI * f0 * g_wscale, wr = w0 * ratio, r2 = sweep * sweep, k0_was = b->k0, kr_was = b->kr, c, cr;
    int i;
    b->w0 = w0;
    b->k0 = w0 * w0;
    b->nl = (r2 - 1.0f) * r2 * r2 / (0.81f * 0.81f);   /* y^2 (1 + nl y^4) = v^2 at the top of a full hit (v ~0.9) */
    b->nd = drag;
    b->inv_r = 1.0f / 0.25f;
    b->kr = wr * wr * 0.25f;
    b->ks = b->k0 * 0.06f;
    b->kg = b->k0 - (float)KR * b->ks;   /* (the ties to the ring stiffen the centre: the ground less) */
    b->kn = b->kr * 0.4f;
    c = keep(k0_was, b->k0);
    cr = keep(kr_was, b->kr);
    b->x0 *= c;
    for (i = 0; i < KR; i++)
        b->x[i] *= cr;
    b->d0 = fm_expf(-1.0f / (decay * g_tscale));
    b->dr = fm_expf(-1.0f / (ring_decay * g_tscale));
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

static int body_bad(float out, float x)  /* a body that ran away (never: but then it is stopped); looked at every 64th */
{
    static uint32_t n;
    if (++n & 63u)
        return 0;
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
        float w = fm_minf(FM_TWO_PI * WIRE_HZ[i] * wt * g_wscale, 0.7f);   /* (under 5 kHz: a contact switching
                                          * on and off near the sample rate would pump energy in) */
        b->wk[i] = w * w;
    }
    b->wd = fm_expf(-1.0f / ((0.004f + 0.03f * decay + 0.02f * snap) * g_tscale));
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
        if (pen < 0.0f) {                /* touching: the contact pushes the wire down (with a loss), and it scrapes */
            f = b->kc * pen - 0.15f * b->wv[i];
            scrape -= pen;
        }
        b->wv[i] = (b->wv[i] - b->wk[i] * b->wx[i] + f) * b->wd;
        b->wv[i] *= 1.0f - fm_minf(0.05f * b->wv[i] * b->wv[i], 0.5f);
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
    float kc, nl, lim, nl3;              /* the ties along the plate; how far they pull before they give way */
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
        float w = fm_minf(FM_TWO_PI * PLATE_HZ[i] * t * (1.0f + 0.01f * white(ns)) * g_wscale, WMAX);
        b->x[i] *= keep(b->k[i], w * w);
        b->k[i] = w * w;
    }
    b->kc = 0.01f + 0.08f * metal;
    b->nl = 2.0f + 60.0f * metal * metal;
    b->lim = 1.0f / fm_sqrtf(b->nl);
    b->nl3 = b->nl * (1.0f / 3.0f);
    b->d = fm_expf(-1.0f / ((0.012f + 0.6f * decay * decay * decay) * g_tscale));
    b->m_len = b->m_left = 3.0f;
    b->m_amp = 2.0f * 1.6f * vel / 3.0f;
    b->hp_a = onepole(3000.0f * t);
    b->sizzle = 2.6f * (1.0f - metal) * fm_sqrtf(1.0f - metal);   /* (RING: none left) */
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
            float d = fm_clampf(b->x[i] - b->x[o], -b->lim, b->lim), t = b->kc * d * (1.0f - b->nl3 * d * d);
                                          /* (it gives way smoothly when stretched: d - nl d^3 / 3 up to lim =
                                          * 1 / sqrt(nl), flat past it; bounded, and without a kink a coarse step could
                                          * pump energy through) */
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
    for (i = 0; i < NP; i++) {           /* (and a drag: the air takes the big swings out fast, the small ones
                                          * hardly; the nonlinear ties alone can hold a big swing up) */
        float v = (b->v[i] + f[i]) * b->d;
        b->v[i] = v * (1.0f - fm_minf(0.01f * v * v, 0.5f));
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
        out = drive_clip((m * b->ring + n * b->sizzle) * 0.85f, b->drive) * b->makeup;   /* (x2.8: the drag's loss) */
    }
    if (body_bad(out, b->x[0])) {
        zero(b, sizeof *b);
        return 0.0f;
    }
    return out;
}

/* ----------------------------------------------------------------- PERC --- */
/* RESO's objects: the second and third modes over the first, how long it rings (x DECAY's) */
static const bp_t P_R1[] = {{10, 2.30f}, {50, 1.59f}, {85, 2.76f}, {120, 3.90f}};
static const bp_t P_R2[] = {{10, 4.10f}, {50, 2.14f}, {85, 5.40f}, {120, 6.80f}};
static const bp_t P_RING[] = {{10, 0.35f}, {50, 0.7f}, {85, 1.2f}, {120, 1.6f}};

typedef struct {
    float x[3], v[3], k[3], d[3], kc;
    float m_amp, m_len, m_left, m_pos;
    float drive, makeup;
} perc_body_t;

static void perc_body_hit(perc_body_t *b, const float *p, float vel, uint32_t *ns)
{
    float tune = k01(p[0]), decay = k01(p[1]), dirt = k01(p[3]), f = 70.0f * fm_exp2f(tune * 5.0f);
    float r[3], tau = (0.02f + 0.8f * decay * decay) * CURVE(P_RING, p[2]);
    int i;
    r[0] = 1.0f;
    r[1] = CURVE(P_R1, p[2]);
    r[2] = CURVE(P_R2, p[2]);
    for (i = 0; i < 3; i++) {
        float w = fm_minf(FM_TWO_PI * f * r[i] * g_wscale, WMAX);
        b->x[i] *= keep(b->k[i], w * w);
        b->k[i] = w * w;
        b->d[i] = fm_expf(-1.0f / (tau / fm_sqrtf(r[i]) * g_tscale));   /* the higher modes die sooner */
    }
    b->kc = b->k[0] * 0.05f;
    b->m_len = b->m_left = 2.0f + 30.0f * (1.0f - k01(p[2]));        /* glass: a hard little stick, wood: soft */
    b->m_amp = 2.0f * 0.9f * vel / b->m_len;
    b->m_pos = 0.3f + 0.5f * (0.5f + 0.5f * white(ns));
    b->drive = 1.0f + 9.0f * dirt * dirt;
    b->makeup = 1.0f / (1.0f + 0.85f * dirt);
}

static inline float perc_body_sample(perc_body_t *b, float push)
{
    float f[3], out;
    int i;
    for (i = 0; i < 3; i++)
        f[i] = -b->k[i] * b->x[i];
    f[0] -= b->kc * (b->x[0] - b->x[1]) - push;
    f[1] += b->kc * (b->x[0] - b->x[1]) - b->kc * (b->x[1] - b->x[2]);
    f[2] += b->kc * (b->x[1] - b->x[2]);
    if (b->m_left > 0.0f) {
        float ph = 1.0f - b->m_left / b->m_len, m = b->m_amp * (0.5f - 0.5f * fm_cosf(FM_TWO_PI * ph));
        f[0] += m;
        f[1] += m * b->m_pos;
        f[2] += m * b->m_pos * 0.6f;
        b->m_left -= 1.0f;
    }
    for (i = 0; i < 3; i++) {
        float v = (b->v[i] + f[i]) * b->d[i];
        b->v[i] = v * (1.0f - fm_minf(0.02f * v * v, 0.5f));   /* (the air, as the string's) */
        b->x[i] += b->v[i];
    }
    out = drive_clip((b->v[0] + 0.7f * b->v[1] + 0.5f * b->v[2]) * 0.8f, b->drive) * b->makeup;
    if (body_bad(out, b->x[0])) {
        zero(b, sizeof *b);
        return 0.0f;
    }
    return out;
}

/* ----------------------------------------------------------------- BASS --- */
#define NS 16                            /* the string's masses */
static const bp_t B_NOTE[] = {{0, 0.0f}, {32, 7.0f}, {64, 12.0f}, {96, 19.0f}, {127, 24.0f}};

typedef struct {
    float x[NS], v[NS];
    float k, nl, d, dh;                  /* tension (w^2 between neighbours), its rise with the pull, damping, the
                                          * highs' damping */
    float kt;                            /* the tension now */
    uint32_t n;
    float m_amp, m_len, m_left;
    int m_at;
    float lfo_ph, lfo_steps, lfo_depth, mic;   /* the pickup: where, how far the wobble moves it */
    float drive, makeup;
} bass_body_t;

static void bass_body_hit(bass_body_t *b, const float *p, float vel, uint32_t *ns)
{
    float semi = fm_floorf(CURVE(B_NOTE, p[0]) + 0.5f), decay = k01(p[1]), wo = k01(p[2]), dirt = k01(p[3]);
    float f = 41.2034f * fm_exp2f(semi / 12.0f), w1 = FM_TWO_PI * f * g_wscale, sk = w1 * (float)(NS + 1) / FM_PI;
    (void)ns;
    {
        float k = fm_minf(sk * sk, 0.25f);   /* (its highest mode, 2 sqrt(k), kept well under 2: stable, whatever the
                                          * pitch is moved to) */
        float c = keep(b->k, k);
        int i;
        for (i = 0; i < NS; i++)
            b->x[i] *= c;
        b->kt = b->kt > 0.0f ? b->kt * k / fm_maxf(b->k, 1e-9f) : k;
        b->k = k;
    }
    b->nl = 0.3f + 2.5f * dirt;
    b->d = fm_expf(-1.0f / ((0.15f + 3.5f * decay * decay) * g_tscale));
    b->dh = 0.0002f + 0.004f * (1.0f - dirt);   /* clean: the highs die fast (round); dirty: they ring (growl) */
    b->m_at = 2;
    b->m_len = b->m_left = 30.0f - 22.0f * dirt;
    b->m_amp = 2.0f * 0.5f * vel / b->m_len;
    b->lfo_depth = wo < 0.1f ? 0.0f : 0.3f + 0.7f * wo;
    b->lfo_steps = wo < 0.37f ? 4.0f : wo < 0.63f ? 2.0f : wo < 0.88f ? 1.0f : 0.5f;
    b->lfo_ph = 0.25f;
    b->mic = (float)NS * 0.45f;
    b->drive = 1.0f + 5.0f * dirt * dirt;
    b->makeup = 1.0f / (1.0f + 0.6f * dirt);
}

static inline float bass_body_sample(bass_body_t *b, float push)
{
    float f[NS], k, prev = 0.0f, pos, out;
    int i, j;
    if (!(b->n++ & 7u)) {                /* the pull: how far the string is stretched (~1 at a hard pluck), every 8th */
        float e = b->x[0] * b->x[0] + b->x[NS - 1] * b->x[NS - 1];
        for (i = 0; i < NS - 1; i++) {
            float d = b->x[i + 1] - b->x[i];
            e += d * d;
        }
        e *= b->k * (1.0f / NS) * 4.0f;
        b->kt = fm_minf(b->k * (1.0f + fm_minf(b->nl * e, 3.0f)), 0.6f);
    }
    k = b->kt;
    for (i = 0; i < NS; i++) {           /* each mass between its neighbours (the ends fixed) */
        float next = i + 1 < NS ? b->x[i + 1] : 0.0f;
        f[i] = k * (prev + next - 2.0f * b->x[i]);
        prev = b->x[i];
    }
    f[NS / 2] += push;
    if (b->m_left > 0.0f) {              /* the pluck */
        float ph = 1.0f - b->m_left / b->m_len;
        f[b->m_at] += b->m_amp * (0.5f - 0.5f * fm_cosf(FM_TWO_PI * ph));
        b->m_left -= 1.0f;
    }
    prev = 0.0f;
    for (i = 0; i < NS; i++) {           /* velocities, the highs damped by the string's bending */
        float next = i + 1 < NS ? b->v[i + 1] : 0.0f, nv = (b->v[i] + f[i]) * b->d;
        nv += b->dh * (prev + next - 2.0f * b->v[i]);
        if (!(b->n & 3u))                /* (the air, every 4th sample: a string driven at its note by the others
                                          * never runs away) */
            nv *= 1.0f - fm_minf(0.08f * nv * nv, 0.5f);
        prev = b->v[i];
        b->v[i] = nv;
    }
    for (i = 0; i < NS; i++)
        b->x[i] += b->v[i];
    /* the pickup: between two masses, moved along the string by the wobble, in time */
    pos = b->mic;
    if (b->lfo_depth > 0.0f) {
        b->lfo_ph += 1.0f / (b->lfo_steps * fm_maxf(g_step, 1000.0f));
        if (b->lfo_ph >= 1.0f)
            b->lfo_ph -= 1.0f;
        pos = 1.0f + ((float)NS * 0.5f - 1.0f) * (1.0f - b->lfo_depth * (0.5f + 0.5f * fm_sin_turns(b->lfo_ph)));
    }
    j = (int)pos;
    pos -= (float)j;
    out = b->v[j] + pos * (b->v[j + 1] - b->v[j]);
    out = drive_clip(out * 2.2f, b->drive) * b->makeup;
    if (body_bad(out, b->x[NS / 2])) {
        zero(b, sizeof *b);
        return 0.0f;
    }
    return out;
}

/* --------------------------------------------------------------- GLITCH --- */
typedef struct {
    float y, vy, g, e;                   /* the ball: height, speed, gravity, its bounce */
    float x0, v0, x1, v1, k0, k1, kc, d0, d1;   /* the plate's two modes */
    float data;                          /* DATA: each impact retunes the plate this far */
    float k0_set;
    float crush_hold, crush_q, crush_n, crush_s, makeup;
    uint32_t ns;
} glitch_body_t;

#define GL_MB 0.3f                       /* the ball's mass (the plate's: 1) */

static void glitch_body_hit(glitch_body_t *b, const float *p, float vel, uint32_t *ns)
{
    float rate = k01(p[0]), pitch = k01(p[1]), len = k01(p[2]), crush = k01(p[3]);
    float ts = (0.003f + 0.07f * (1.0f - rate) * (1.0f - rate)) * g_tscale, vimp = 0.4f + 1.0f * vel;
    float f = 150.0f * fm_exp2f(pitch * 5.2f), w = fm_minf(FM_TWO_PI * f * g_wscale, 0.9f);
    float w1 = fm_minf(FM_TWO_PI * f * 2.71f * g_wscale, WMAX);
    b->k0 = b->k0_set = w * w;
    b->k1 = w1 * w1;
    b->kc = b->k0 * 0.08f;
    b->d0 = fm_expf(-1.0f / ((0.008f + 0.12f * len) * g_tscale));
    b->d1 = fm_expf(-1.0f / ((0.004f + 0.05f * len) * g_tscale));
    b->e = 0.45f + 0.5f * len;
    b->g = vimp / ts;                    /* dropped from rest: it lands after ts samples at vimp */
    if (vel > 0.0f) {
        b->y = b->x0 + 0.5f * vimp * ts;
        b->vy = 0.0f;
    }
    b->data = pitch > 0.75f ? (pitch - 0.75f) * 4.0f : 0.0f;
    b->crush_hold = 1.0f + 10.0f * crush * crush;
    b->crush_q = crush > 0.05f ? 96.0f - 88.0f * crush : 0.0f;
    b->makeup = 1.0f / (1.0f + 0.5f * crush);
    b->ns = *ns;
}

static inline float glitch_body_sample(glitch_body_t *b, float push)
{
    float f0 = -b->k0 * b->x0 - b->kc * (b->x0 - b->x1) + push, f1 = -b->k1 * b->x1 + b->kc * (b->x0 - b->x1), out;
    b->v0 = (b->v0 + f0) * b->d0;
    b->x0 += b->v0;
    b->v1 = (b->v1 + f1) * b->d1;
    b->x1 += b->v1;
    if (b->g > 0.0f) {                   /* the ball */
        b->vy -= b->g;
        b->y += b->vy;
        if (b->y <= b->x0) {
            float vr = b->vy - b->v0;
            b->y = b->x0;
            if (vr < -0.02f) {           /* an impact: momentum shared, some of it bounced back (e) */
                float j = -(1.0f + b->e) * vr / (1.0f / GL_MB + 1.0f);
                b->vy += j / GL_MB;
                b->v0 -= j;
                if (b->data > 0.0f && vr < -0.06f) {   /* DATA: the plate retuned at each real impact (its energy
                                          * kept: the swing scaled, so a retune never pumps it up) */
                    float k = b->k0_set * fm_exp2f(b->data * 2.0f * white(&b->ns));
                    b->x0 *= fm_sqrtf(b->k0 / k);
                    b->k0 = k;
                }
            } else {                     /* lying on the plate: carried along, silently (a plate shaken down faster
                                          * than the ball falls throws it up again: it dances) */
                b->vy = b->v0;
            }
        }
    }
    out = b->v0 + 0.6f * b->v1;
    if (b->crush_q > 0.0f) {             /* CRUSH: fewer samples, fewer levels */
        b->crush_n += 1.0f;
        if (b->crush_n >= b->crush_hold) {
            b->crush_n -= b->crush_hold;
            b->crush_s = fm_floorf(out * b->crush_q + 0.5f) / b->crush_q;
        }
        out = b->crush_s;
    }
    out = fm_tanhf(out * 1.4f) * b->makeup;
    if (body_bad(out, b->x0) || body_bad(out, b->y)) {
        zero(b, sizeof *b);
        return 0.0f;
    }
    return out;
}
