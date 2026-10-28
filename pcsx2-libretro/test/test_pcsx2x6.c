/*
 * Created by Claude for the ARMSX2 shader-optimisation branch, 2026-08-31.
 * Adapted for pcsx2x6 (readback present, savestate loading), 2026-09-11.
 *
 * As part of the RECALBOX Project
 * http://www.recalbox.com
 *
 * ARMSX2 (PS2) libretro core - headless test harness.
 *
 * Copyright (C) 2025-2026 Bkg2k / Recalbox <https://www.recalbox.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation. See the LICENSE file in the project
 * root for the full licence text.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * IMPORTANT: when modifying or redistributing this file, you MUST preserve
 * the copyright notices above and the GPLv2 licence reference intact.
 *
 * ---------------------------------------------------------------------------
 *
 * dlopen() the core, run N frames with no window, inject a scripted RetroPad
 * sequence, optionally load a RetroArch savestate at frame K, and report fps.
 *
 * Unlike ARMSX2, pcsx2x6 does not need a hardware context: SET_HW_RENDER is
 * refused here, the core falls back to its readback present (XRGB8888 pixels
 * through video_refresh, NULL for a dup), so this is a plain software frontend.
 * ponytail: readback only. pcsx2_hw_render=enabled profiles are measured on the
 * readback path (~13 % dearer on SDBZ); add an EGL/GLES3 frontend if an A/B ever
 * needs the zero-copy path itself.
 *
 * retro_run is blocking in this core (token handshake, 2 s timeout), so the loop
 * needs no sleep and the core's own limiter caps it at the nominal rate, exactly
 * as under RetroArch.
 *
 * Usage:
 *   test_pcsx2x6 <core.so> <content> <system_dir> <frames> [bseq] [outdir] [shots]
 *     bseq   "frame:BUTTON,..." e.g. "600:START,900:CROSS". Names: A B X Y L R L2 R2
 *            L3 R3 START SELECT UP DOWN LEFT RIGHT, or CROSS CIRCLE SQUARE TRIANGLE.
 *     shots  "300,600" writes <outdir>/frame-NNNNNN.png
 * Environment:
 *   PCSX2X6_TEST_OPTIONS    "key=value;key=value" core option overrides
 *   PCSX2X6_TEST_OPT_FILE   RetroArch .opt file (key = "value"), applied before OPTIONS
 *   PCSX2X6_TEST_STATE      RetroArch .state (#RZIPv1# and/or RASTATE, or raw)
 *   PCSX2X6_TEST_STATE_FRAME frame at which it is loaded (default 300)
 *   PCSX2X6_TEST_SAVE       "frame:path" writes the core's raw state there (loadable as TEST_STATE)
 *   PCSX2X6_TEST_WINDOW     "a:b" frames of the STEADY window (default: 600 frames
 *                           after the state load to the end, else frames/4 to the end)
 *
 * Build (buildroot toolchain, so the binary matches the target glibc):
 *   $CC -O2 -Wall -o test_pcsx2x6 test_pcsx2x6.c -I.. -ldl -lz -lstdc++ -lpthread
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

#include "libretro.h"

#define BUTTON_HOLD_FRAMES 12
#define MAX_BUTTON_EVENTS 64
#define MAX_JOYPAD_ID 16
#define MAX_OPTIONS 128
#define MAX_SHOTS 1024

struct ButtonEvent { int frame; unsigned id; };
struct Option { char key[128]; char value[128]; };

static const char *gSystemDir, *gOutDir;
static struct ButtonEvent gEvents[MAX_BUTTON_EVENTS];
static int gEventCount;
static bool gPressed[MAX_JOYPAD_ID];
static struct Option gOptions[MAX_OPTIONS];
static int gOptionCount;
/* Defaults the core declares through SET_VARIABLES ("Label; first|second", first =
 * default), answered for every option the run does not override, the way RetroArch
 * does. Without this the core keeps whatever its ini held from the previous run. */
static struct Option gDefaults[MAX_OPTIONS];
static int gDefaultCount;
static int gShots[MAX_SHOTS], gShotCount;

static unsigned long gFrames, gVideoRefreshes;
static unsigned gLastW, gLastH;

static void (*core_init)(void);
static void (*core_deinit)(void);
static void (*core_set_environment)(retro_environment_t);
static void (*core_set_video_refresh)(retro_video_refresh_t);
static void (*core_set_audio_sample)(retro_audio_sample_t);
static void (*core_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*core_set_input_poll)(retro_input_poll_t);
static void (*core_set_input_state)(retro_input_state_t);
static bool (*core_load_game)(const struct retro_game_info *);
static void (*core_unload_game)(void);
static void (*core_run)(void);
static void (*core_get_system_av_info)(struct retro_system_av_info *);
static bool (*core_unserialize)(const void *, size_t);
static size_t (*core_serialize_size)(void);
static bool (*core_serialize)(void *, size_t);
static void (*core_set_controller_port_device)(unsigned, unsigned);

/* ------------------------------------------------------------------ capture */

static void Be32(FILE *f, uint32_t v)
{
	const unsigned char b[4] = {v >> 24, v >> 16, v >> 8, v};
	fwrite(b, 1, 4, f);
}

static void PngChunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
	uLong crc = crc32(0, (const Bytef *)type, 4);
	/* Only fold data in when there is some: zlib's crc32(crc, Z_NULL, 0) returns
	 * the INITIAL value, not crc -- the bug that broke ARMSX2's first IEND. */
	if (len)
		crc = crc32(crc, data, len);
	Be32(f, len);
	fwrite(type, 1, 4, f);
	if (len)
		fwrite(data, 1, len, f);
	Be32(f, (uint32_t)crc);
}

/* XRGB8888 little-endian in memory: B, G, R, X. */
static bool WritePng(const char *path, const unsigned char *px, unsigned w, unsigned h, size_t pitch)
{
	const size_t row = 1 + (size_t)w * 3;
	unsigned char *raw = malloc(row * h);
	uLongf zlen = compressBound(row * h);
	unsigned char *z = malloc(zlen);
	FILE *f = NULL;
	bool ok = false;
	if (!raw || !z)
		goto out;
	for (unsigned y = 0; y < h; y++)
	{
		unsigned char *d = raw + y * row;
		const unsigned char *s = px + y * pitch;
		*d++ = 0; /* filter: none */
		for (unsigned x = 0; x < w; x++, s += 4)
		{
			*d++ = s[2];
			*d++ = s[1];
			*d++ = s[0];
		}
	}
	if (compress2(z, &zlen, raw, row * h, 6) != Z_OK || !(f = fopen(path, "wb")))
		goto out;
	{
		static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
		const unsigned char ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h,
			8, 2, 0, 0, 0}; /* 8-bit RGB */
		fwrite(sig, 1, 8, f);
		PngChunk(f, "IHDR", ihdr, 13);
		PngChunk(f, "IDAT", z, (uint32_t)zlen);
		PngChunk(f, "IEND", NULL, 0);
	}
	ok = fclose(f) == 0;
out:
	free(raw);
	free(z);
	return ok;
}

/* --------------------------------------------------------------- savestate */

static uint32_t Le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t Le64(const unsigned char *p) { return Le32(p) | (uint64_t)Le32(p + 4) << 32; }

static unsigned char *ReadFile(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf = NULL;
	long n;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0 && (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return buf;
}

/* RetroArch rzip: "#RZIPv\1#", u32 chunk size, u64 total size, then per chunk a
 * u32 compressed size and a zlib stream. */
static unsigned char *Unrzip(const unsigned char *in, size_t inSize, size_t *outSize)
{
	const uint64_t total = Le64(in + 12);
	unsigned char *out = malloc(total ? total : 1);
	size_t pos = 20, done = 0;
	if (!out)
		return NULL;
	while (pos + 4 <= inSize && done < total)
	{
		const uint32_t clen = Le32(in + pos);
		uLongf dlen = total - done;
		pos += 4;
		if (pos + clen > inSize || uncompress(out + done, &dlen, in + pos, clen) != Z_OK)
		{
			free(out);
			return NULL;
		}
		pos += clen;
		done += dlen;
	}
	*outSize = done;
	return out;
}

/* Returns the core's own payload: the RASTATE "MEM " block if wrapped, else all of it. */
static const unsigned char *StatePayload(const unsigned char *buf, size_t size, size_t *payloadSize)
{
	size_t pos = 8;
	if (size < 8 || memcmp(buf, "RASTATE", 7) != 0)
	{
		*payloadSize = size;
		return buf;
	}
	while (pos + 8 <= size)
	{
		const uint32_t len = Le32(buf + pos + 4);
		if (memcmp(buf + pos, "MEM ", 4) == 0 && pos + 8 + len <= size)
		{
			*payloadSize = len;
			return buf + pos + 8;
		}
		if (memcmp(buf + pos, "END ", 4) == 0)
			break;
		pos += 8 + ((len + 7) & ~(size_t)7); /* blocks are 8-byte aligned */
	}
	return NULL;
}

/* --------------------------------------------------------------- callbacks */

static void CoreLog(enum retro_log_level level, const char *fmt, ...)
{
	static const char *names[] = {"DBG", "INF", "WRN", "ERR"};
	va_list ap;
	fprintf(stderr, "[core %s] ", names[(unsigned)level > 3 ? 1 : level]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static const char *FindOption(const struct Option *opts, int n, const char *key)
{
	for (int i = n - 1; i >= 0; i--) /* last one wins: OPTIONS override OPT_FILE */
		if (strcmp(key, opts[i].key) == 0)
			return opts[i].value;
	return NULL;
}

static void AddOption(struct Option *opts, int *n, const char *key, const char *value, size_t vlen)
{
	if (*n >= MAX_OPTIONS)
		return;
	snprintf(opts[*n].key, sizeof(opts[*n].key), "%s", key);
	if (vlen >= sizeof(opts[*n].value))
		vlen = sizeof(opts[*n].value) - 1;
	memcpy(opts[*n].value, value, vlen);
	opts[*n].value[vlen] = 0;
	(*n)++;
}

static bool EnvironmentCb(unsigned cmd, void *data)
{
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char **)data = gSystemDir;
			return true;
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback *)data)->log = CoreLog;
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
			return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
		{
			struct retro_variable *var = data;
			var->value = var->key ? FindOption(gOptions, gOptionCount, var->key) : NULL;
			if (!var->value && var->key)
				var->value = FindOption(gDefaults, gDefaultCount, var->key);
			return true;
		}
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool *)data = false;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned *)data = 0; /* forces SET_VARIABLES, the only protocol parsed here */
			return true;
		case RETRO_ENVIRONMENT_SET_VARIABLES:
			gDefaultCount = 0;
			for (const struct retro_variable *v = data; v && v->key; v++)
			{
				const char *semi = v->value ? strstr(v->value, "; ") : NULL;
				if (semi)
					AddOption(gDefaults, &gDefaultCount, v->key, semi + 2, strcspn(semi + 2, "|"));
			}
			return true;
		case RETRO_ENVIRONMENT_SET_HW_RENDER:
			fprintf(stderr, "[TEST] SET_HW_RENDER refused: core falls back to readback\n");
			return false;
		case RETRO_ENVIRONMENT_GET_CAN_DUPE:
			*(bool *)data = true;
			return true;
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
		case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
		case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		case RETRO_ENVIRONMENT_SET_GEOMETRY:
		case RETRO_ENVIRONMENT_SET_MESSAGE:
		case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
			return true;
		default:
			return false;
	}
}

static void VideoCb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	gVideoRefreshes++;
	if (!data)
		return; /* dup: no new frame */
	gFrames++;
	gLastW = w;
	gLastH = h;
	if (data == RETRO_HW_FRAME_BUFFER_VALID || !gOutDir)
		return;
	for (int i = 0; i < gShotCount; i++)
	{
		char path[4096];
		if ((unsigned long)gShots[i] != gFrames)
			continue;
		snprintf(path, sizeof(path), "%s/frame-%06lu.png", gOutDir, gFrames);
		fprintf(stderr, "[TEST] capture %s (%ux%u) %s\n", path, w, h,
			WritePng(path, data, w, h, pitch) ? "ok" : "FAILED");
	}
}

static void AudioSampleCb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t AudioBatchCb(const int16_t *d, size_t f) { (void)d; return f; }
static void InputPollCb(void) {}

static int16_t InputStateCb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	/* The core polls each id itself (no JOYPAD_MASK). */
	return port == 0 && device == RETRO_DEVICE_JOYPAD && id < MAX_JOYPAD_ID && gPressed[id];
}

/* ----------------------------------------------------------------- parsing */

static unsigned ButtonId(const char *name)
{
	/* CROSS & co: RetroPad names are rotated w.r.t. the DualShock symbols the game
	 * prints (RetroPad B = CROSS, X = TRIANGLE); write the PS2 name. */
	static const struct { const char *n; unsigned id; } map[] = {
		{"B", RETRO_DEVICE_ID_JOYPAD_B}, {"Y", RETRO_DEVICE_ID_JOYPAD_Y},
		{"SELECT", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"START", RETRO_DEVICE_ID_JOYPAD_START},
		{"UP", RETRO_DEVICE_ID_JOYPAD_UP}, {"DOWN", RETRO_DEVICE_ID_JOYPAD_DOWN},
		{"LEFT", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"RIGHT", RETRO_DEVICE_ID_JOYPAD_RIGHT},
		{"A", RETRO_DEVICE_ID_JOYPAD_A}, {"X", RETRO_DEVICE_ID_JOYPAD_X},
		{"L", RETRO_DEVICE_ID_JOYPAD_L}, {"R", RETRO_DEVICE_ID_JOYPAD_R},
		{"L2", RETRO_DEVICE_ID_JOYPAD_L2}, {"R2", RETRO_DEVICE_ID_JOYPAD_R2},
		{"L3", RETRO_DEVICE_ID_JOYPAD_L3}, {"R3", RETRO_DEVICE_ID_JOYPAD_R3},
		{"CROSS", RETRO_DEVICE_ID_JOYPAD_B}, {"CIRCLE", RETRO_DEVICE_ID_JOYPAD_A},
		{"SQUARE", RETRO_DEVICE_ID_JOYPAD_Y}, {"TRIANGLE", RETRO_DEVICE_ID_JOYPAD_X},
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (strcasecmp(name, map[i].n) == 0)
			return map[i].id;
	return MAX_JOYPAD_ID;
}

static void ParseButtons(const char *spec)
{
	char *copy = strdup(spec);
	for (char *t = strtok(copy, ","); t && gEventCount < MAX_BUTTON_EVENTS; t = strtok(NULL, ","))
	{
		char *colon = strchr(t, ':');
		unsigned id;
		if (!colon)
			continue;
		*colon = 0;
		id = ButtonId(colon + 1);
		if (atoi(t) > 0 && id < MAX_JOYPAD_ID)
			gEvents[gEventCount++] = (struct ButtonEvent){atoi(t), id};
		else
			fprintf(stderr, "[TEST] ignored button event '%s:%s'\n", t, colon + 1);
	}
	free(copy);
}

static void ParseOptions(const char *spec)
{
	char *copy = strdup(spec);
	for (char *t = strtok(copy, ";"); t; t = strtok(NULL, ";"))
	{
		char *eq = strchr(t, '=');
		if (!eq)
			continue;
		*eq = 0;
		AddOption(gOptions, &gOptionCount, t, eq + 1, strlen(eq + 1));
	}
	free(copy);
}

/* RetroArch .opt: one `key = "value"` per line. */
static void ParseOptFile(const char *path)
{
	char line[512], key[128];
	FILE *f = fopen(path, "r");
	if (!f)
	{
		fprintf(stderr, "[TEST] cannot open opt file %s\n", path);
		return;
	}
	while (fgets(line, sizeof(line), f))
	{
		char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (q2 && sscanf(line, " %127[^ =]", key) == 1)
			AddOption(gOptions, &gOptionCount, key, q1 + 1, (size_t)(q2 - q1 - 1));
	}
	fclose(f);
}

static double Ms(const struct timespec *a, const struct timespec *b)
{
	return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1.0e6;
}

#define SYM(name) \
	do { \
		*(void **)(&core_##name) = dlsym(handle, "retro_" #name); \
		if (!core_##name) { fprintf(stderr, "[TEST] missing retro_" #name "\n"); return 1; } \
	} while (0)

int main(int argc, char **argv)
{
	void *handle;
	struct retro_game_info game = {0};
	struct retro_system_av_info av;
	struct timespec t0, tNow, tLast, tWin0 = {0}, tWin1 = {0};
	unsigned long lastFrames = 0, lastReport = 0;
	int frames, stateFrame = 300, winStart, winEnd;
	long saveFrame = -1;
	const char *savePath = NULL;
	unsigned char *stateFile = NULL, *stateRaw = NULL;
	const unsigned char *statePayload = NULL;
	size_t stateSize = 0;
	const char *env;

	if (argc < 5)
	{
		fprintf(stderr, "Usage: %s <core.so> <content> <system_dir> <frames> [bseq] [outdir] [shots]\n"
			"  see the file header for the PCSX2X6_TEST_* environment\n", argv[0]);
		return 1;
	}
	gSystemDir = argv[3];
	frames = atoi(argv[4]);
	if (argc > 5 && argv[5][0])
		ParseButtons(argv[5]);
	if (argc > 6)
		gOutDir = argv[6];
	if (argc > 7)
	{
		char *copy = strdup(argv[7]);
		for (char *t = strtok(copy, ","); t && gShotCount < MAX_SHOTS; t = strtok(NULL, ","))
			gShots[gShotCount++] = atoi(t);
		free(copy);
	}
	if ((env = getenv("PCSX2X6_TEST_OPT_FILE")) && *env)
		ParseOptFile(env);
	if ((env = getenv("PCSX2X6_TEST_OPTIONS")) && *env)
		ParseOptions(env);
	for (int i = 0; i < gOptionCount; i++)
		fprintf(stderr, "[TEST] option %s=%s\n", gOptions[i].key, gOptions[i].value);

	if ((env = getenv("PCSX2X6_TEST_STATE")) && *env)
	{
		size_t fileSize = 0;
		if (!(stateFile = ReadFile(env, &fileSize)))
		{
			fprintf(stderr, "[TEST] cannot read state %s\n", env);
			return 1;
		}
		stateRaw = stateFile;
		stateSize = fileSize;
		if (fileSize > 20 && memcmp(stateFile, "#RZIPv", 6) == 0 &&
			!(stateRaw = Unrzip(stateFile, fileSize, &stateSize)))
		{
			fprintf(stderr, "[TEST] rzip decode failed\n");
			return 1;
		}
		if (!(statePayload = StatePayload(stateRaw, stateSize, &stateSize)))
		{
			fprintf(stderr, "[TEST] no MEM block in RASTATE\n");
			return 1;
		}
		if ((env = getenv("PCSX2X6_TEST_STATE_FRAME")) && *env)
			stateFrame = atoi(env);
		fprintf(stderr, "[TEST] state: %zu byte payload, load at frame %d\n", stateSize, stateFrame);
	}
	if ((env = getenv("PCSX2X6_TEST_SAVE")) && *env && strchr(env, ':'))
	{
		saveFrame = atol(env);
		savePath = strchr(env, ':') + 1;
	}
	winStart = statePayload ? stateFrame + 600 : frames / 4;
	winEnd = frames;
	if ((env = getenv("PCSX2X6_TEST_WINDOW")) && *env)
		sscanf(env, "%d:%d", &winStart, &winEnd);

	if (!(handle = dlopen(argv[1], RTLD_NOW)))
	{
		fprintf(stderr, "[TEST] dlopen failed: %s\n", dlerror());
		return 1;
	}
	SYM(init);
	SYM(deinit);
	SYM(set_environment);
	SYM(set_video_refresh);
	SYM(set_audio_sample);
	SYM(set_audio_sample_batch);
	SYM(set_input_poll);
	SYM(set_input_state);
	SYM(load_game);
	SYM(unload_game);
	SYM(run);
	SYM(get_system_av_info);
	SYM(unserialize);
	SYM(serialize_size);
	SYM(serialize);
	SYM(set_controller_port_device);

	core_set_environment(EnvironmentCb);
	core_set_video_refresh(VideoCb);
	core_set_audio_sample(AudioSampleCb);
	core_set_audio_sample_batch(AudioBatchCb);
	core_set_input_poll(InputPollCb);
	core_set_input_state(InputStateCb);
	core_init();

	game.path = argv[2];
	if (!core_load_game(&game))
	{
		fprintf(stderr, "[TEST] load_game failed\n");
		return 1;
	}
	core_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	core_get_system_av_info(&av);
	fprintf(stderr, "[TEST] av: %ux%u @ %.2f Hz, window %d:%d\n", av.geometry.base_width,
		av.geometry.base_height, av.timing.fps, winStart, winEnd);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	tLast = t0;
	while ((long)gFrames < frames)
	{
		clock_gettime(CLOCK_MONOTONIC, &tNow);
		if (Ms(&t0, &tNow) > (double)frames * 1000.0) /* 1 s/frame ceiling: a wedged VM must not hang */
		{
			fprintf(stderr, "[TEST] wall-clock budget exhausted at frame %lu\n", gFrames);
			break;
		}
		for (int k = 0; k < gEventCount; k++)
		{
			if ((unsigned long)gEvents[k].frame == gFrames)
				gPressed[gEvents[k].id] = true;
			else if ((unsigned long)(gEvents[k].frame + BUTTON_HOLD_FRAMES) == gFrames)
				gPressed[gEvents[k].id] = false;
		}
		if (statePayload && (long)gFrames >= stateFrame)
		{
			const bool ok = core_unserialize(statePayload, stateSize);
			fprintf(stderr, "[TEST] state load at frame %lu: %s\n", gFrames, ok ? "ok" : "FAILED");
			statePayload = NULL;
			if (!ok)
				return 1;
		}

		core_run();

		if (saveFrame >= 0 && (long)gFrames >= saveFrame)
		{
			const size_t n = core_serialize_size();
			void *buf = malloc(n);
			FILE *f = fopen(savePath, "wb");
			const bool ok = buf && f && core_serialize(buf, n) && fwrite(buf, 1, n, f) == n;
			if (f)
				fclose(f);
			free(buf);
			fprintf(stderr, "[TEST] state save at frame %lu to %s: %s\n", gFrames, savePath, ok ? "ok" : "FAILED");
			saveFrame = -1;
		}

		if (gFrames == lastFrames)
			continue;
		lastFrames = gFrames;
		clock_gettime(CLOCK_MONOTONIC, &tNow);
		if ((long)gFrames == winStart)
			tWin0 = tNow;
		if ((long)gFrames == winEnd)
			tWin1 = tNow;
		/* One line a second of emulated time: the per-interval fps is what shows a
		 * short combat window (SC3 demo) that an average would dilute. */
		if (gFrames - lastReport >= 60)
		{
			fprintf(stderr, "[TEST] frame %lu t=%.1fms fps=%.2f %ux%u\n", gFrames, Ms(&t0, &tNow),
				(double)(gFrames - lastReport) * 1000.0 / Ms(&tLast, &tNow), gLastW, gLastH);
			lastReport = gFrames;
			tLast = tNow;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &tNow);

	fprintf(stderr, "\n[TEST] ---- summary ----\n");
	fprintf(stderr, "[TEST] frames            : %lu (video refreshes %lu)\n", gFrames, gVideoRefreshes);
	fprintf(stderr, "[TEST] wall time         : %.1f ms\n", Ms(&t0, &tNow));
	if (tWin1.tv_sec || tWin1.tv_nsec)
		fprintf(stderr, "[TEST] STEADY            : %.2f fps  %.3f ms/frame  (frames %d:%d)\n",
			(double)(winEnd - winStart) * 1000.0 / Ms(&tWin0, &tWin1),
			Ms(&tWin0, &tWin1) / (double)(winEnd - winStart), winStart, winEnd);
	else
		fprintf(stderr, "[TEST] STEADY            : window %d:%d not reached\n", winStart, winEnd);

	core_unload_game();
	core_deinit();
	dlclose(handle);
	if (stateRaw != stateFile)
		free(stateRaw);
	free(stateFile);
	return 0;
}
