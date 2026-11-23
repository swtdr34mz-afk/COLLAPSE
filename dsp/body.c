/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE's sound engine, the PHYSICAL BODIES: instruments that are not oscillators but masses on springs, simulated
 * at the sample rate (44.1 kHz), as Anukari does. A mallet pushes a mass for a moment; the springs carry the push
 * around the body; a microphone picks up how fast the masses move. Nothing is played back: the sound is the body
 * moving. A new hit on a body still ringing just pushes it again (as on a real drum).
 *
 * Units: one time step is one sample, so a spring's stiffness over its mass is w^2, w = 2 pi f / 44100 (f the
 * frequency it rings at on its own). Integrated with symplectic Euler (velocity first, then position): stable while
 * w < 2, which the stiffest springs here stay far under.
 *
 * The KICK: a drum head. A heavy centre on a NONLINEAR spring (it stiffens the further it is pushed: a hard hit starts
 * high and falls to its note as it rings out, the kick's pitch sweep, with nobody drawing an envelope) and a ring of
 * six light masses round it, each on the rim, tied to the centre and to its neighbours (the overtones, the knock).
 * Where the mallet lands moves between the centre and the ring at each hit. Part of dsp/engine.c (included there). */
/* ============================================================ the bodies === */
#define KR 6                             /* the ring's masses */

typedef struct {
    float x0, v0;                        /* the centre */
    float x[KR], v[KR];                  /* the ring */
    float k0, kg, nl, nd, kr, ks, kn;            /* stiffness: centre to ground (w0^2), its nonlinearity, ring to rim, ring to
                                          * centre, ring to ring */
    float inv_r;                         /* 1 / a ring mass's mass (the centre's is 1) */
    float d0, dr;                        /* damping each sample: the centre's, the ring's */
    float w0;                            /* the centre's w (the nonlinear spring's scale) */
    /* the mallet: a raised cosine of force, `left` samples still to go of `len` */
    float m_amp, m_len, m_left, m_pos;   /* m_pos: 0 on the centre .. 1 on the ring */
    int m_mass;                          /* the ring mass it lands next to */
    float mic_ring;                      /* how much of the ring the microphone hears */
    float drive, makeup;
    float peak;
} kick_body_t;

static void kick_body_set(kick_body_t *b, const float *p)
{
    float tune = k01(p[0]), punch = k01(p[1]), decay = k01(p[2]), dirt = k01(p[3]);
    float f0 = 34.0f * fm_exp2f(tune * 1.35f), w0 = FM_TWO_PI * f0 / ENG_SR;
    float fr = f0 * (2.2f + 1.6f * punch), wr = FM_TWO_PI * fr / ENG_SR;
    b->w0 = w0;
    b->k0 = w0 * w0;
    /* the sweep: a full hit starts R times above the note (y^2 (1 + nl y^4) = v^2 at the top, v ~0.9) */
    {
        float r = 1.6f + 5.4f * punch, r2 = r * r;
        b->nl = (r2 - 1.0f) * r2 * r2 / (0.81f * 0.81f);   /* (k grows with y^4: the pitch falls fast as it rings down) */
    }
    b->nd = 0.002f + 0.02f * punch;              /* drag: loses the big swings fast, the small ones hardly */
    b->inv_r = 1.0f / 0.25f;
    b->kr = wr * wr * 0.25f;
    b->ks = b->k0 * 0.06f;
    b->kg = b->k0 - (float)KR * b->ks;               /* (the ties to the ring stiffen the centre: the ground less) */
    b->kn = b->kr * 0.4f;
    b->d0 = fm_expf(-1.0f / ((0.06f + 1.4f * decay * decay) * ENG_SR));
    b->dr = fm_expf(-1.0f / ((0.008f + 0.05f * punch) * ENG_SR));
    b->mic_ring = 0.15f + 0.6f * punch;
    b->drive = 1.0f + 9.0f * dirt * dirt;
    b->makeup = 1.0f / (1.0f + 0.85f * dirt);
}

/* a hit: the mallet's hardness (how short its push is) from PUNCH, where it lands from the web's push on the knobs
 * (and a little chance) */
static void kick_body_hit(kick_body_t *b, const float *p, float vel, uint32_t *ns)
{
    float punch = k01(p[1]), len = 2.0f + (1.0f - punch) * 180.0f;   /* 0.05 .. 4 ms */
    kick_body_set(b, p);
    b->m_len = b->m_left = len;
    b->m_amp = 2.0f * 0.9f * vel / len;              /* its impulse: the centre's speed ~0.9 * vel */
    b->m_pos = fm_clampf(0.08f + 0.25f * (0.5f + 0.5f * white(ns)) + 0.004f * (p[1] - 64.0f), 0.0f, 0.7f);
    b->m_mass = (int)((0.5f + 0.5f * white(ns)) * (float)KR) % KR;
}

static inline float kick_body_sample(kick_body_t *b)
{
    float f0 = 0.0f, f[KR], y, k, out;
    int i;
    /* the centre's spring to the ground: stiffer the further it is out (clamped well inside stability) */
    y = b->x0 * b->w0;
    y *= y;
    k = b->kg + b->k0 * b->nl * y * y;
    if (k > 0.5f)
        k = 0.5f;
    f0 -= k * b->x0;
    for (i = 0; i < KR; i++) {           /* each ring mass: to the rim, to the centre, to its two neighbours */
        float l = b->x[i == 0 ? KR - 1 : i - 1], r = b->x[i + 1 == KR ? 0 : i + 1], d = b->x[i] - b->x0;
        f[i] = -b->kr * b->x[i] - b->ks * d - b->kn * (2.0f * b->x[i] - l - r);
        f0 += b->ks * d;
    }
    if (b->m_left > 0.0f) {              /* the mallet */
        float ph = 1.0f - b->m_left / b->m_len, push = b->m_amp * (0.5f - 0.5f * fm_cosf(FM_TWO_PI * ph));
        f0 += push * (1.0f - b->m_pos);
        f[b->m_mass] += push * b->m_pos * 0.25f;   /* (a ring mass is a quarter of the centre) */
        b->m_left -= 1.0f;
    }
    b->v0 = (b->v0 + f0) * b->d0;
    b->v0 /= 1.0f + b->nd * b->v0 * b->v0;
    b->x0 += b->v0;
    for (i = 0; i < KR; i++) {
        b->v[i] = (b->v[i] + f[i] * b->inv_r) * b->dr;
        b->x[i] += b->v[i];
    }
    /* the microphone: how fast the head moves, at the centre and at the ring mass by the mallet */
    out = b->v0 + b->mic_ring * b->v[b->m_mass];
    out = drive_clip(out, b->drive) * b->makeup;
    if (!(out == out) || fm_fabsf(b->x0) > 1e4f) {   /* (never: but a body that ever ran away is stopped) */
        uint8_t *z = (uint8_t *)b;
        uint32_t n;
        for (n = 0; n < sizeof *b; n++)
            z[n] = 0;
        return 0.0f;
    }
    return out;
}
