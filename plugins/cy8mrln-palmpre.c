/*
 *  tslib/plugins/cy8mrln-palmpre.c
 *
 *  Copyright (C) 2010 Frederik Sdun <frederik.sdun@googlemail.com>
 *		     Thomas Zimmermann <ml@vdm-design.de>
 *		     Simon Busch <morphis@gravedo.de>
 *
 * This file is placed under the LGPL.  Please see the file
 * COPYING for more details.
 *
 * Plugin for the cy8mrln touchscreen with the firmware used on the Palm Pre (Plus).
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "cy8mrln.h"
#include "config.h"
#include "tslib-private.h"
#include "tslib-filter.h"

#define SCREEN_WIDTH   319
#define SCREEN_HEIGHT  527
#define H_FIELDS       7
#define V_FIELDS       11
#define DEFAULT_SCANRATE 60
#define DEFAULT_VERBOSE 0
#define DEFAULT_WOT_THRESHOLD 22
#define DEFAULT_SLEEPMODE CY8MRLN_ON_STATE
#define DEFAULT_WOT_SCANRATE WOT_SCANRATE_512HZ
#define DEFAULT_TIMESTAMP_MODE 1
#define DEFAULT_TS_PRESSURE 255
#define DEFAULT_NOISE 25

#define container_of(ptr, type, member) ({ \
	const typeof( ((type*)0)->member ) *__mptr = (ptr); \
	(type *)( (char *)__mptr - offsetof(type, member)); })

struct cy8mrln_palmpre_input
{  
	uint16_t	n_r;
	uint16_t	field[H_FIELDS * V_FIELDS];
	uint16_t	ffff;			/* always 0xffff */
	uint8_t		seq_nr1;		/* incremented if seq_nr0 == scanrate */
	uint16_t	seq_nr2;		/* incremeted if seq_nr1 == 255 */
	uint8_t		unknown[4]; 
	uint8_t		seq_nr0;		/* incremented [0:scanrate] */
	uint8_t		null;		   /* NULL byte */
}__attribute__((packed));

struct tslib_cy8mrln_palmpre 
{
	struct tslib_module_info	module;
	uint16_t			references[H_FIELDS * V_FIELDS];
	int				scanrate;
	int				verbose;
	int				wot_threshold;
	int				sleepmode;
	int				wot_scanrate;
	int				timestamp_mode;
	int				ts_pressure;
	int				noise;
	int				last_n_valid_samples;
	struct ts_sample*		last_valid_samples;
};

static int cy8mrln_palmpre_set_scanrate(struct tslib_cy8mrln_palmpre* info, int rate);
static int cy8mrln_palmpre_set_verbose(struct tslib_cy8mrln_palmpre* info, int v);
static int cy8mrln_palmpre_set_sleepmode(struct tslib_cy8mrln_palmpre* info, int mode);
static int cy8mrln_palmpre_set_wot_scanrate(struct tslib_cy8mrln_palmpre* info, int rate);
static int cy8mrln_palmpre_set_wot_threshold(struct tslib_cy8mrln_palmpre* info, int v);
static int cy8mrln_palmpre_set_timestamp_mode(struct tslib_cy8mrln_palmpre* info, int v);
static int cy8mrln_palmpre_set_noise (struct tslib_cy8mrln_palmpre* info, int n);
static int cy8mrln_palmpre_set_ts_pressure(struct tslib_cy8mrln_palmpre* info, int p);
static int parse_scanrate(struct tslib_module_info *info, char *str, void *data);
static int parse_verbose(struct tslib_module_info *info, char *str, void *data);
static int parse_wot_scanrate(struct tslib_module_info *info, char *str, void *data);
static int parse_wot_threshold(struct tslib_module_info *info, char *str, void *data);
static int parse_sleepmode(struct tslib_module_info *info, char *str, void *data);
static int parse_timestamp_mode(struct tslib_module_info *info, char *str, void *data);
static int parse_noise(struct tslib_module_info *info, char *str, void *data);
static int parse_ts_pressure(struct tslib_module_info *info, char *str, void *data);
static void cy8mrln_palmpre_update_references(uint16_t references[H_FIELDS * V_FIELDS], uint16_t field[H_FIELDS * V_FIELDS]);
static void cy8mrln_palmpre_interpolate(uint16_t field[H_FIELDS * V_FIELDS], int x, int y, struct ts_sample *out);
static int cy8mrln_palmpre_fini(struct tslib_module_info *info);
static int cy8mrln_palmpre_read(struct tslib_module_info *info, struct ts_sample *samp, int nr);
TSAPI struct tslib_module_info *cy8mrln_palmpre_mod_init(struct tsdev *dev, const char *params);


static int cy8mrln_palmpre_set_scanrate(struct tslib_cy8mrln_palmpre* info, int rate)
{
	if (info == NULL || info->module.dev == NULL || ioctl(info->module.dev->fd,CY8MRLN_IOCTL_SET_SCANRATE,&rate) < 0)
		goto error;
		
	info->scanrate = rate;
	return 0;
	
error:
	printf("TSLIB: cy8mrln_palmpre: ERROR: could not set scanrate value\n");
	return -1;
}

static int cy8mrln_palmpre_set_verbose(struct tslib_cy8mrln_palmpre* info, int v)
{
	if (info == NULL || info->module.dev == NULL || ioctl(info->module.dev->fd,CY8MRLN_IOCTL_SET_VERBOSE_MODE,&v) < 0)
		goto error;
	
	info->verbose = v;
	return 0;

error:
	printf("TSLIB: cy8mrln_palmpre: ERROR: could not set verbose value\n");
	return -1;
}

static int cy8mrln_palmpre_set_sleepmode(struct tslib_cy8mrln_palmpre* info, int mode)
{
	if (info == NULL || info->module.dev == NULL || ioctl(info->module.dev->fd,CY8MRLN_IOCTL_SET_SLEEPMODE,&mode) < 0)
		goto error;

	info->sleepmode = mode;
	return 0;
	
error:
	printf("TSLIB: cy8mrln_palmpre: ERROR: could not set sleepmode value\n");
	return -1;
}

static int cy8mrln_palmpre_set_wot_scanrate(struct tslib_cy8mrln_palmpre* info, int rate)
{
	if (info == NULL || info->module.dev == NULL || ioctl(info->module.dev->fd,CY8MRLN_IOCTL_SET_WOT_SCANRATE,&rate) < 0)
		goto error;

	info->wot_scanrate = rate;
	return 0;

error:
	printf("TSLIB: cy8mrln_palmpre: ERROR: could not set scanrate value\n");
	return -1;
}

static int cy8mrln_palmpre_set_wot_threshold(struct tslib_cy8mrln_palmpre* info, int v)
{
	if (info == NULL || info->module.dev == NULL) 
		goto error;
	if(v < WOT_THRESHOLD_MIN || v > WOT_THRESHOLD_MAX)
		goto error;
	if(ioctl(info->module.dev->fd,CY8MRLN_IOCTL_SET_WOT_THRESHOLD,&v) < 0)
		goto error;
		
	info->wot_threshold = v;
	return 0;
	
error:
	printf("TSLIB: cy8mrln_palmpre: ERROR: could not set wot treshhold value\n");
	return -1;
}

static int cy8mrln_palmpre_set_timestamp_mode(struct tslib_cy8mrln_palmpre* info, int v)
{
	v = v ? 1 : 0;
	if(info == NULL || info->module.dev == NULL || ioctl(info->module.dev->fd,CY8MRLN_IOCTL_SET_TIMESTAMP_MODE,&v) < 0)
	     goto error;
	info->timestamp_mode = v;
	return 0;
	
error:
	printf("TSLIB: cy8mrln_palmpre: ERROR: could not set timestamp value\n");
	return -1;
}

static int cy8mrln_palmpre_set_noise (struct tslib_cy8mrln_palmpre* info, int n)
{
	if (info == NULL) {
		printf("TSLIB: cy8mrln_palmpre: ERROR: could not set noise value\n");
		return -1;
	}
	info->noise = n;
	return 0;
}

static int cy8mrln_palmpre_set_ts_pressure(struct tslib_cy8mrln_palmpre* info, int p)
{
	if (info == NULL) {
		printf("TSLIB: cy8mrln_palmpre: ERROR: could not set ts_pressure value\n");
		return -1;
	}
	info->ts_pressure = p;
	return 0;
}

static int parse_scanrate(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = container_of(info, struct tslib_cy8mrln_palmpre, module);
	unsigned long rate = strtoul(str, NULL, 0);

	if(rate == ULONG_MAX && errno == ERANGE)
		return -1;

	return cy8mrln_palmpre_set_scanrate(i, rate);
}

static int parse_verbose(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = container_of(info, struct tslib_cy8mrln_palmpre, module);
	unsigned long v = strtoul(str, NULL, 0);

	if(v == ULONG_MAX && errno == ERANGE)
		return -1;

	return cy8mrln_palmpre_set_verbose(i, v);
}

static int parse_wot_scanrate(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = container_of(info, struct tslib_cy8mrln_palmpre, module);
	unsigned long rate = strtoul(str, NULL, 0);

	return cy8mrln_palmpre_set_wot_scanrate(i, rate);
}

static int parse_wot_threshold(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = container_of(info, struct tslib_cy8mrln_palmpre, module);
	unsigned long threshold = strtoul(str, NULL, 0);

	return cy8mrln_palmpre_set_wot_threshold(i, threshold);
}

static int parse_sleepmode(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = container_of(info, struct tslib_cy8mrln_palmpre, module);
	unsigned long sleep = strtoul(str, NULL, 0);

	return cy8mrln_palmpre_set_sleepmode(i, sleep);
}

static int parse_timestamp_mode(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = (struct tslib_cy8mrln_palmpre*) info;
	unsigned long sleep = strtoul(str, NULL, 0);

	if(sleep == ULONG_MAX && errno == ERANGE)
		return -1;


	return cy8mrln_palmpre_set_sleepmode(i, sleep);
}
static int parse_noise(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = (struct tslib_cy8mrln_palmpre*) info;
	unsigned long noise = strtoul (str, NULL, 0);

	if(noise == ULONG_MAX && errno == ERANGE)
		return -1;


	return cy8mrln_palmpre_set_noise (i, noise);
}

static int parse_ts_pressure(struct tslib_module_info *info, char *str, void *data)
{
	(void)data;
	struct tslib_cy8mrln_palmpre *i = (struct tslib_cy8mrln_palmpre*) info;
	unsigned long tp = strtoul (str, NULL, 0);

	if(tp == ULONG_MAX && errno == ERANGE)
		return -1;

	return cy8mrln_palmpre_set_ts_pressure (i, tp);
}

#define NR_VARS (sizeof(cy8mrln_palmpre_vars) / sizeof(cy8mrln_palmpre_vars[0]))
/*
 *      f12
 * f21 (x/y) f23
 *      f32
 */

static void cy8mrln_palmpre_interpolate(uint16_t field[H_FIELDS * V_FIELDS], int x, int y, struct ts_sample *out) {
	float f12, f21, f23, f32;
	int posx = SCREEN_WIDTH - SCREEN_WIDTH / H_FIELDS * x;
	int posy = SCREEN_HEIGHT / V_FIELDS * y;
	static const int dx = SCREEN_WIDTH / H_FIELDS;
	static const int dy = SCREEN_HEIGHT / V_FIELDS;
	
	/* caluculate corrections for top, bottom, left and right fields */
	f12 = (y == 0) ? 0.0f : 0.5 * ((float)field[(y - 1) * H_FIELDS + x] / field[y * H_FIELDS + x]);
	f32 = (y == (V_FIELDS - 1)) ? 0.0f : 0.5 * (float)field[(y + 1) * H_FIELDS + x] / field[y * H_FIELDS + x];
	f21 = (x == (H_FIELDS - 1)) ? 0.0f : 0.5 * (float)field[y * H_FIELDS + x + 1] / field[y * H_FIELDS + x];
	f23 = (x == 0) ? 0.0f : 0.5 * (float) field[y * H_FIELDS + x - 1] / field[y * H_FIELDS + x];

	/* correct values for the edges, shift the mesuarment point by half a 
	 * field diminsion to the outside */
	if (x == 0) {
		posx = posx + dx / 2.0;
		f21 = f21 * 2.0;
	} else if (x == (H_FIELDS - 1)) {
		posx = posx - dx / 2.0;
		f23 = f23 * 2.0;
	}

	if (y == 0) {
		posy = posy - dy / 2.0;
		f32 = f32 * 2.0;
	} else if (y == (V_FIELDS - 1)) {
		posy = posy + dy / 2.0;
		f12 = f12 * 2.0;
	}

	out->x = posx // + (f13 + f33 - f11 - f31) * dx /* use corners too?*/
		 + (f23 - f21) * dx
//		 + (f21 == 0.0) ? ((f23 * 2 + (dx / 2)) * dx) : (f23 * dx)
//		 - (f23 == 0.0) ? ((f21 * 2 + (dx / 2)) * dx) : (f21 * dx)
		 - (dx / 2);
	out->y = posy // + (f31 + f33 - f11 - f13) * dy /* use corners too?*/
		 + (f32 - f12) * dy + (dy / 2);

#ifdef DEBUG
	fprintf(stderr, "RAW---------------------------> (%i/%i) f12: %f f21: %f, f23: %f, f32: %f\n", x, y, f12, f21, f23, f32);
#endif /*DEBUG*/
}

static int cy8mrln_palmpre_read(struct tslib_module_info *info, struct ts_sample *samp, int nr)
{
	struct tsdev *ts = info->dev;
	//We can only read one input struct at once
	struct cy8mrln_palmpre_input cy8mrln_evt;
	struct tslib_cy8mrln_palmpre *cy8mrln_info;
	int max_x = 0, max_y = 0, max_value = 0, x, y;
	uint16_t tmp_value;
	int ret, valid_samples = 0;
	struct ts_sample *p = samp;
	
	/* initalize all samples with proper values */
	memset(p, '\0', nr * sizeof (*p));
	
	cy8mrln_info = container_of(info, struct tslib_cy8mrln_palmpre, module);
	
	ret = read(ts->fd, &cy8mrln_evt, sizeof(cy8mrln_evt));
	if (ret > 0) {
		cy8mrln_palmpre_update_references (cy8mrln_info->references, cy8mrln_evt.field);
		max_x = 0;
		max_y = 0;
		max_value = 0;
		for (y = 0; y < V_FIELDS; y ++) {
			for (x = 0; x < H_FIELDS; x++) {
				tmp_value = cy8mrln_evt.field[y * H_FIELDS + x];

				/* check for the maximum value */
				if (tmp_value > max_value) {
					max_value = tmp_value;
					max_x = x;
					max_y = y;
				}
			}
		}
		/* only caluclate events that are not noise */
		if (max_value > cy8mrln_info->noise) {
			cy8mrln_palmpre_interpolate(cy8mrln_evt.field, max_x, max_y, &samp[valid_samples]);
			samp->pressure = cy8mrln_info->ts_pressure;
#ifdef DEBUG
			fprintf(stderr,"RAW for (%d/%d): %d-----------> %d %d %d\n",
				max_x, max_y, max_value,samp->x, samp->y, samp->pressure);
#endif /*DEBUG*/
			gettimeofday(&samp->tv,NULL);
			valid_samples++;
			if (cy8mrln_info->last_valid_samples == NULL) {
				cy8mrln_info->last_valid_samples = malloc (sizeof (struct ts_sample) * valid_samples);
			} else if (cy8mrln_info->last_n_valid_samples != valid_samples) {
				cy8mrln_info->last_valid_samples = realloc (cy8mrln_info->last_valid_samples, sizeof (struct ts_sample) * valid_samples);
			}
			memcpy (cy8mrln_info->last_valid_samples, samp, sizeof (struct ts_sample) * valid_samples);
			cy8mrln_info->last_n_valid_samples = valid_samples;
		} else {
			//return last samples with pressure = 0 to show a mouse up
			if (cy8mrln_info->last_valid_samples != NULL) {
				valid_samples = cy8mrln_info->last_n_valid_samples;
				memcpy (samp, cy8mrln_info->last_valid_samples, sizeof (struct ts_sample) * valid_samples);
				for (x = 0; x < valid_samples; x++) {
					samp[x].pressure = 0;
				}
				cy8mrln_info->last_n_valid_samples = 0;
				free (cy8mrln_info->last_valid_samples);
				cy8mrln_info->last_valid_samples = NULL;
#ifdef DEBUG
				fprintf (stderr, "cy8mrln_palmpre: Returning %i old values with 0 pressure\n", valid_samples);
#endif
			}
		}
	} else {
		return -1;
	}

	return valid_samples;
}

static void cy8mrln_palmpre_update_references(uint16_t references[H_FIELDS * V_FIELDS], uint16_t field[H_FIELDS * V_FIELDS])
{
	int x, y;
	for (y = 0; y < V_FIELDS; y ++) {
		for (x = 0; x < H_FIELDS; x++) {
			if (field[y * H_FIELDS + x] > references[y * H_FIELDS + x]) {
				references [y * H_FIELDS + x] = field [y * H_FIELDS + x];
				field [y * H_FIELDS + x] = 0;
			} else {
				field [y * H_FIELDS + x] = references [y * H_FIELDS + x] - field [y * H_FIELDS + x];
			}
		}
	}
}

static int cy8mrln_palmpre_fini(struct tslib_module_info *info)
{
	struct tslib_cy8mrln_palmpre *i = container_of(info, struct tslib_cy8mrln_palmpre, module);
	if (i->last_valid_samples != NULL) {
		free (i->last_valid_samples);
	}
	free (i);
#ifdef DEBUG
	fprintf (stderr, "finishing cy8mrln_palmpre");
#endif
	return 0;
}

static const struct tslib_vars cy8mrln_palmpre_vars[] =
{
	{ "scanrate",		NULL, parse_scanrate},
	{ "verbose",		NULL, parse_verbose},
	{ "wot_scanrate",	NULL, parse_wot_scanrate},
	{ "wot_threshold",      NULL, parse_wot_threshold},
	{ "sleepmode",		NULL, parse_sleepmode},
	{ "timestamp_mode",     NULL, parse_timestamp_mode},
	{ "noise",		NULL, parse_noise},
	{ "ts_pressure",	NULL, parse_ts_pressure}
};


static const struct tslib_ops cy8mrln_palmpre_ops = 
{
	.read = cy8mrln_palmpre_read,
	.fini = cy8mrln_palmpre_fini,
};

TSAPI struct tslib_module_info *cy8mrln_palmpre_mod_init(struct tsdev *dev, const char *params)
{
	struct tslib_cy8mrln_palmpre *info;
	struct cy8mrln_palmpre_input input;
	int ret = 0;
	
	info = malloc(sizeof(struct tslib_cy8mrln_palmpre));
	if(info == NULL)
	     return NULL;
	info->module.ops = &cy8mrln_palmpre_ops;
	info->last_valid_samples = NULL;
	info->last_n_valid_samples = 0;

	cy8mrln_palmpre_set_verbose(info, DEFAULT_VERBOSE);
	cy8mrln_palmpre_set_scanrate(info, DEFAULT_SCANRATE);
	cy8mrln_palmpre_set_timestamp_mode(info, DEFAULT_TIMESTAMP_MODE);
	cy8mrln_palmpre_set_sleepmode(info, DEFAULT_SLEEPMODE);
	cy8mrln_palmpre_set_wot_scanrate(info, DEFAULT_WOT_SCANRATE);
	cy8mrln_palmpre_set_wot_threshold(info, DEFAULT_WOT_THRESHOLD);
	cy8mrln_palmpre_set_noise(info, DEFAULT_NOISE);
	cy8mrln_palmpre_set_ts_pressure(info, DEFAULT_TS_PRESSURE);

	if (tslib_parse_vars(&info->module, cy8mrln_palmpre_vars, NR_VARS, params)) {
		free(info);
		return NULL;
	}

	/* We need the intial values the touchscreen repots with no touch input for
	 * later use */
	do {
		ret = read(dev->fd, &input, sizeof(input));
	}
	while (ret <= 0);
	memcpy(info->references, input.field, H_FIELDS * V_FIELDS * sizeof(uint16_t));

	return &(info->module);
}
#ifndef TSLIB_STATIC_CY8MRLN_MODULE
	TSLIB_MODULE_INIT(cy8mrln_palmpre_mod_init);
#endif