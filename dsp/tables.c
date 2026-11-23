/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE's sound engine: the instruments, their knobs and sweetspots, the macros. Part of dsp/engine.c (included
 * there: the device builds the engine as one unit). */
/* ================================================================ the tables === */
const inst_t INST[NINST] = {
    {"KICK", "KCK", RGB565(93, 232, 224),
     {{"TUNE", 4, {16, 48, 80, 112}, {"SUB", "DEEP", "PUNCH", "TIGHT"}},
      {"PUNCH", 4, {10, 50, 85, 120}, {"SOFT", "CLICK", "ZAP", "LASER"}},
      {"DECAY", 4, {10, 45, 80, 120}, {"SHORT", "TIGHT", "808", "BOOM"}},
      {"DIRT", 4, {0, 45, 90, 127}, {"CLEAN", "WARM", "DISTORT", "SHRED"}}}},
    {"SNARE", "SNR", RGB565(120, 190, 255),
     {{"TUNE", 4, {16, 50, 85, 118}, {"FAT", "DNB", "TIGHT", "PICCOLO"}},
      {"SNAP", 4, {10, 50, 85, 120}, {"BODY", "CRACK", "NOISE", "WHITE"}},
      {"DECAY", 4, {10, 45, 80, 120}, {"GHOST", "TIGHT", "ROOM", "GATED"}},
      {"DIRT", 4, {0, 45, 90, 127}, {"CLEAN", "GRIT", "CRUSH", "SHRED"}}}},
    {"HATS", "HAT", RGB565(200, 240, 255),
     {{"TUNE", 4, {16, 50, 85, 118}, {"DARK", "808", "BRIGHT", "TINY"}},
      {"DECAY", 4, {8, 40, 80, 120}, {"TICK", "CLOSED", "HALF", "OPEN"}},
      {"METAL", 4, {10, 50, 85, 120}, {"NOISE", "MIX", "METAL", "RING"}},
      {"DIRT", 4, {0, 45, 90, 127}, {"CLEAN", "GRIT", "8 BIT", "4 BIT"}}}},
    {"GLITCH", "GLT", RGB565(255, 110, 200),
     {{"RATE", 4, {10, 45, 85, 120}, {"SLOW", "STUTTER", "BUZZ", "SWARM"}},
      {"PITCH", 4, {16, 50, 85, 118}, {"LOW", "MID", "HIGH", "DATA"}},
      {"LENGTH", 4, {8, 45, 85, 120}, {"BLIP", "SHORT", "BURST", "SMEAR"}},
      {"CRUSH", 4, {0, 45, 90, 127}, {"CLEAN", "BITS", "BROKEN", "DEAD"}}}},
    {"PERC", "PRC", RGB565(255, 190, 90),
     {{"TUNE", 4, {16, 50, 85, 118}, {"LOW", "TOM", "BELL", "TICK"}},
      {"DECAY", 4, {8, 45, 85, 120}, {"KNOCK", "SHORT", "RING", "DRONE"}},
      {"RESO", 4, {10, 50, 85, 120}, {"WOOD", "SKIN", "METAL", "GLASS"}},
      {"DIRT", 4, {0, 45, 90, 127}, {"CLEAN", "GRIT", "CRUSH", "SHRED"}}}},
    {"BASS", "BAS", RGB565(150, 255, 120),
     {{"NOTE", 5, {0, 32, 64, 96, 127}, {"E1", "B1", "E2", "B2", "E3"}},
      {"DECAY", 4, {8, 45, 85, 120}, {"PLUCK", "SHORT", "LONG", "HOLD"}},
      {"WOBBLE", 5, {0, 32, 64, 96, 127}, {"NONE", "1/4", "1/8", "1/16", "SCREAM"}},
      {"DIRT", 4, {0, 45, 90, 127}, {"CLEAN", "REESE", "GROWL", "SHRED"}}}},
};

const macro_t MASS_KNOB = {"MASS", 5, {0, 32, 64, 96, 127}, {"FEATHER", "LIGHT", "NEUTRAL", "HEAVY", "BLACK HOLE"}};
const macro_t CHAOS_KNOB = {"CHAOS", 4, {16, 48, 80, 112}, {"CHAIN", "RING", "STAR", "MESH"}};
const char *const WEB_NAME[4] = {"CHAIN", "RING", "STAR", "MESH"};
int eng_topology(int chaos) { return chaos < 32 ? 0 : chaos < 64 ? 1 : chaos < 96 ? 2 : 3; }

/* the FX page: BPM's zones are tempos (the knob's value is the BPM minus 60, 0..127 = 60..187; 200 past it) */
const macro_t MASTER_KNOB[4] = {
    {"BPM", 5, {50, 60, 80, 110, 114}, {"110", "120", "140", "170", "174"}},
    {"SWING", 3, {0, 30, 60}, {"STRAIGHT", "SHUFFLE", "DRUNK"}},
    {"DRIVE", 4, {0, 40, 85, 127}, {"CLEAN", "GLUE", "HOT", "MELT"}},
    {"SPACE", 4, {0, 35, 75, 120}, {"DRY", "ROOM", "HALL", "VOID"}},
};
