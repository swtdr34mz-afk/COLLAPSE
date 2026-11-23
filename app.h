/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 swtdr34mz (COLLAPSE) */
/* COLLAPSE by swtdr34mz: a physics drum machine for the M-VAVE FM-1 (on fm1-base). Six instruments tied together by
 * springs: every hit pushes the one struck and its neighbours to new sounds. Glitch, dubstep, DnB, LXR.
 * This header is the app's half of the contract with the platform (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "COLLAPSE"
#define APP_SPLASH_RGB RGB(93, 232, 224)
#define APP_SPLASH                       /* app_splash: the chrome logo, not the platform's page */
#define APP_TICK                         /* app_tick: MIDI in (clock, Start, Stop), at 1 kHz */
void app_tick(void);
#define APP_BLOCK 128u                   /* frames per audio block: 2.9 ms at 44.1 kHz (a drum machine: tight) */

/* the store: the app's settings (the eight patterns, what is picked) and the panel calibration (the platform's) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* backup and restore over USB: the settings */
#define APP_BACKUP_OBJS {OBJ_SETTINGS}
#define APP_BACKUP_MAGIC 0x4C4C4F43u            /* "COLL" */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces the eight patterns"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_splash(void);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
