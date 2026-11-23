/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE's sound engine: six instruments (KICK, SNARE, HATS, GLITCH, PERC, BASS) tied together by springs, the
 * PHYSICS WEB. Every hit is a push: the instrument struck jumps to new parameters, and its neighbours in the web are
 * shoved too; springs pull everything back to the sound that was set, so no two hits sound the same and the groove
 * never falls apart. Two macros rule the web: MASS (light and nervous .. heavy and slow) and CHAOS (which instruments
 * are tied to which, and how hard they push). A sequencer of 16 to 64 steps, an internal clock or MIDI clock in.
 *
 * The app owns the patterns (pattern_t, below) and binds one (eng_bind): the main loop writes it, the render reads it
 * (a byte or a word at a time). eng_render runs in the audio interrupt. */
#pragma once
#include <stdint.h>

#define ENG_SR 44100.0f
#define NINST 6
#define NSTEPS 64
#define NPATTERNS 8
enum { I_KICK, I_SNARE, I_HATS, I_GLITCH, I_PERC, I_BASS };

/* a knob with named sweetspots (centre 0..127, name), left to right */
#define MACRO_ZONES 6
typedef struct {
    const char *name;
    uint8_t nzones;
    uint8_t centre[MACRO_ZONES];
    const char *zone[MACRO_ZONES];
} macro_t;

typedef struct {
    const char *name;                    /* KICK .. */
    const char *tag;                     /* three letters, for the sliders */
    uint16_t rgb;                        /* its colour (RGB565) */
    macro_t knob[4];                     /* its four sound knobs (K1..K4) */
} inst_t;
extern const inst_t INST[NINST];
extern const macro_t MASS_KNOB, CHAOS_KNOB;
extern const macro_t MASTER_KNOB[4];     /* the FX page: BPM, SWING, DRIVE, SPACE */
extern const char *const WEB_NAME[4];    /* CHAOS's topologies: CHAIN, RING, STAR, MESH */
int eng_topology(int chaos);             /* 0..3 */

typedef struct {                         /* an instrument's track in a pattern */
    uint8_t p[4];                        /* its sound: the four knobs, 0..127 */
    uint8_t level;                       /* 0..127 */
    uint8_t rsv[3];
    uint32_t steps[2], acc[2];           /* bit s of the 64: the step plays / is accented */
} track_t;

typedef struct {                         /* a pattern: six tracks and the web */
    track_t tr[NINST];
    uint8_t mass, chaos;                 /* the macros, 0..127 */
    uint8_t bpm;                         /* 60 .. 200 (the internal clock) */
    uint8_t swing;                       /* 0 .. 80 */
    uint8_t length;                      /* 16, 32, 48, 64 */
    uint8_t drive, space;                /* the master: drive, reverb, 0..127 */
    uint8_t rsv;
} pattern_t;

typedef struct {                         /* a factory pattern, as written in patterns.c */
    const char *name, *genre;
    uint8_t bpm, swing, mass, chaos, length, drive, space;
    uint8_t p[NINST][4];
    uint8_t level[NINST];
    const char *steps[NINST];            /* 'X' accent, 'x' hit, '.' rest; as long as the pattern */
} factory_t;
extern const factory_t FACTORY[NPATTERNS];

static inline int step_on(const track_t *t, int s) { return (t->steps[s >> 5] >> (s & 31) & 1u) != 0; }
static inline int step_acc(const track_t *t, int s) { return (t->acc[s >> 5] >> (s & 31) & 1u) != 0; }

void eng_init(void);
void eng_bind(const pattern_t *p);       /* now */
void eng_bind_next(const pattern_t *p);  /* at the top of the next loop (playing), else now */
int eng_next_pending(void);
void eng_mute(int inst, int on);
void eng_play(int on);                   /* the internal clock: from step 1 / stop */
/* MIDI in: Start, Continue, Stop and clock (24 a quarter: a step every 6), from the 1 kHz tick */
void eng_ext_start(int cont);
void eng_ext_stop(void);
void eng_ext_clock(void);
void eng_ext_bpm(float bpm);             /* the tempo heard in the clock (0: none) */
int eng_ext(void);                       /* following a MIDI clock now */
void eng_hit(int inst, float vel);       /* an instrument now (a key) */
void eng_kick_web(void);                 /* every instrument shoved at once (the intro's collapse) */
void eng_panic(void);
/* for the screen */
int eng_step(void);                      /* the step under way (-1 stopped) */
int eng_playing(void);
float eng_env(int inst);                 /* its level now 0..1 */
void eng_disp(int inst, float out[4]);   /* where the web has pushed its knobs now, in knob units */
float eng_energy(int inst);              /* how hard it is moving now 0..1 */
uint32_t eng_hits(void);
void eng_render(int32_t *lr, uint32_t n, float gain);
