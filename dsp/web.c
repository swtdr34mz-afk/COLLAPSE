/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE's sound engine, the PHYSICS WEB. Each instrument is a mass on four springs, one per knob: its displacement
 * is how far the web has pushed that knob from where it was set. A hit kicks the instrument struck (a random way) and
 * shoves its neighbours (each tie its own way, always the same: a kick on the snare always pulls the bass the same
 * way, so the web has a character); springs between tied instruments carry the push on, and the springs home pull
 * everything back. Integrated once a block (2.9 ms), 24 values: almost free.
 *
 * MASS (SELECT): the inertia, 0.35 (FEATHER: big fast jumps that settle at once) .. 3.5 (BLACK HOLE: slow, long swings);
 * it also moves every instrument at once: heavy is lower, longer and dirtier, light is higher, shorter and cleaner.
 * CHAOS (ALGORITHM): the topology (CHAIN, RING, STAR around the kick, MESH: all with all) and, inside each, how hard
 * the hits push and how loosely the springs hold; at the top it ratchets (a hit sometimes stutters).
 * Part of dsp/engine.c (included there). */
/* ================================================================== the web === */
static float wd[NINST][4], wv[NINST][4];   /* displacement (knob units), velocity (units a second) */
static float wtie[NINST][NINST];           /* the ties now (0: none) */
static int w_topo = -1;
/* the WEB page: TENSION, DAMPING, REACH (x, 1 as made), each instrument's LINK (0 apart .. 1 as made .. 2 welded) */
static float g_tens = 1.0f, g_damp = 1.0f, g_reach = 1.0f, g_lk[NINST] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
static uint32_t web_noise = 0x6C8E9CF5u;

/* the way a push travels along a tie, i to j, on knob k: fixed, -1 .. 1, never small */
static float tie_sign(int i, int j, int k)
{
    uint32_t h = (uint32_t)(i * 73 + j * 151 + k * 19 + 7) * 2654435761u;
    float s = 0.5f + 0.5f * (float)((h >> 16) & 255u) / 255.0f;
    return (h >> 9 & 1u) ? s : -s;
}

static void web_topology(int topo)
{
    int i, j;
    if (topo == w_topo)
        return;
    w_topo = topo;
    for (i = 0; i < NINST; i++)
        for (j = 0; j < NINST; j++)
            wtie[i][j] = 0.0f;
    switch (topo) {
    case 0:                              /* CHAIN: KICK - SNARE - HATS - GLITCH - PERC - BASS */
    case 1:                              /* RING: the chain closed, BASS back to KICK */
        for (i = 0; i < NINST - 1; i++)
            wtie[i][i + 1] = wtie[i + 1][i] = 1.0f;
        if (topo == 1)
            wtie[I_BASS][I_KICK] = wtie[I_KICK][I_BASS] = 1.0f;
        break;
    case 2:                              /* STAR: everything on the kick, the bass hardest */
        for (i = 1; i < NINST; i++)
            wtie[0][i] = wtie[i][0] = i == I_BASS ? 1.3f : 0.9f;
        break;
    default:                             /* MESH: all with all */
        for (i = 0; i < NINST; i++)
            for (j = 0; j < NINST; j++)
                if (i != j)
                    wtie[i][j] = 0.6f;
        break;
    }
}

static float w_mass(int mass) { return 0.35f * fm_exp2f((float)mass * (3.32f / 127.0f)); }

/* instrument i struck at vel: its kick and its neighbours' shove */
static void web_push(int i, float vel, int mass, int chaos)
{
    float m = w_mass(mass), a = (45.0f + 1.6f * (float)chaos) * vel / m;
    int j, k;
    web_topology(eng_topology(chaos));
    for (k = 0; k < 4; k++)
        wv[i][k] += a * 0.8f * white(&web_noise) * fm_minf(g_lk[i], 1.0f);
    for (j = 0; j < NINST; j++)
        if (wtie[i][j] > 0.0f)
            for (k = 0; k < 4; k++)
                wv[j][k] += a * wtie[i][j] * g_lk[i] * g_lk[j] * tie_sign(i, j, k);
}

/* everything shoved at once, hard (the intro's collapse) */
static void web_slam(void)
{
    int i, k;
    for (i = 0; i < NINST; i++)
        for (k = 0; k < 4; k++)
            wv[i][k] += 260.0f * white(&web_noise);
}

static void web_step(float dt, int mass, int chaos)
{
    float m = w_mass(mass), K = 40.0f * g_tens, ch = (float)chaos / 127.0f;
    float zeta = (0.12f + 0.38f * (1.0f - ch)) * g_damp, c = 2.0f * zeta * fm_sqrtf(K * m), kc = 12.0f + 40.0f * ch;
    float acc[NINST][4];
    int i, j, k;
    web_topology(eng_topology(chaos));
    for (i = 0; i < NINST; i++)
        for (k = 0; k < 4; k++) {
            float f = -K * wd[i][k] - c * wv[i][k];
            for (j = 0; j < NINST; j++)
                if (wtie[i][j] > 0.0f)
                    f += kc * wtie[i][j] * g_lk[i] * g_lk[j] * (wd[j][k] - wd[i][k]);
            acc[i][k] = f / m;
        }
    for (i = 0; i < NINST; i++)
        for (k = 0; k < 4; k++) {
            wv[i][k] += acc[i][k] * dt;
            wd[i][k] = fm_clampf(wd[i][k] + wv[i][k] * dt, -64.0f, 64.0f);
            if (fm_fabsf(wd[i][k]) >= 64.0f)
                wv[i][k] *= -0.3f;       /* the wall: it bounces back */
        }
}

/* the knob each instrument's MASS moves: its pitch, its length (and every one's dirt) */
static const uint8_t W_TUNE[NINST] = {0, 0, 0, 1, 0, 255};
static const uint8_t W_DECAY[NINST] = {2, 2, 1, 2, 1, 1};

/* instrument t's four knobs for a hit now: as set, pushed by the web, moved by MASS */
static void web_knobs(int t, const uint8_t *set, int mass, float *out)
{
    float mm = (float)(mass - 64) / 63.0f;
    int k;
    for (k = 0; k < 4; k++)
        out[k] = (float)set[k] + wd[t][k] * g_reach;
    if (W_TUNE[t] != 255u)
        out[W_TUNE[t]] -= mm * (t == I_GLITCH ? 30.0f : 22.0f);
    out[W_DECAY[t]] += mm * 30.0f;
    if (mm > 0.0f)
        out[3] += mm * 35.0f;
    for (k = 0; k < 4; k++)
        out[k] = fm_clampf(out[k], 0.0f, 127.0f);
}

static float web_energy(int t)
{
    float e = 0.0f;
    int k;
    for (k = 0; k < 4; k++)
        e += fm_fabsf(wv[t][k]) * 0.004f + fm_fabsf(wd[t][k]) * 0.012f;
    return fm_clampf(e, 0.0f, 1.0f);
}
