/*
 * obs-scene-relight Lens Blur: 背景ぼかし（レンズのぼけ・玉ボケ）
 * 背景のソースに掛けて、本物のカメラの被写界深度のように背景だけをぼかす。
 * 前景は別のソースなので、背景ソースに掛ければ背景だけがぼける
 * Copyright (C) 2026 obs-scene-relight contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hz-util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>

#define S_RADIUS "radius"
#define S_BOKEH "bokeh"
#define S_RAMP "ramp"

#define MAX_LEVELS 6     /* 半径が大きいときに 1/2 ずつ縮める最大の回数（1/32 まで） */
#define DISC_TARGET 4.0f /* 縮めた後の半径を、これ以下の画素数に収める */

struct lensblur {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *e_image, *e_texel, *e_radius, *e_bokeh, *e_ramp, *e_cubic;

	gs_texrender_t *input;
	gs_texrender_t *lv[MAX_LEVELS + 1]; /* lv[k] は 1/2^k の線形光（k = 0 は縮めないときだけ使う） */
	gs_texrender_t *disc;
	gs_texture_t *dummy;

	float radius_pct, bokeh, ramp;
	bool disc_ok;
};

static const char *lb_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LensBlur");
}

static void lb_update(void *data, obs_data_t *s)
{
	struct lensblur *f = data;
	f->radius_pct = (float)obs_data_get_double(s, S_RADIUS);
	f->bokeh = (float)obs_data_get_double(s, S_BOKEH);
	f->ramp = (float)obs_data_get_double(s, S_RAMP);
}

static void lb_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, S_RADIUS, 2.0);
	obs_data_set_default_double(s, S_BOKEH, 0.5);
	obs_data_set_default_double(s, S_RAMP, 0.0);
}

static obs_properties_t *lb_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *r =
		obs_properties_add_float_slider(props, S_RADIUS, obs_module_text("LensBlur.Radius"), 0.0, 10.0, 0.05);
	obs_property_float_set_suffix(r, " %");
	obs_property_set_long_description(r, obs_module_text("LensBlur.Radius.Desc"));
	obs_properties_add_float_slider(props, S_BOKEH, obs_module_text("LensBlur.Bokeh"), 0.0, 1.0, 0.01);
	obs_property_t *rp =
		obs_properties_add_float_slider(props, S_RAMP, obs_module_text("LensBlur.Ramp"), 0.0, 1.0, 0.01);
	obs_property_set_long_description(rp, obs_module_text("LensBlur.Ramp.Desc"));
	return props;
}

static void lb_destroy(void *data)
{
	struct lensblur *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->input);
	for (int i = 0; i <= MAX_LEVELS; i++)
		gs_texrender_destroy(f->lv[i]);
	gs_texrender_destroy(f->disc);
	gs_texture_destroy(f->dummy);
	obs_leave_graphics();
	bfree(f);
}

static void *lb_create(obs_data_t *settings, obs_source_t *context)
{
	struct lensblur *f = bzalloc(sizeof(*f));
	f->context = context;

	char *path = obs_module_file("effects/lensblur.effect");
	char *err = NULL;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &err);
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (int i = 0; i <= MAX_LEVELS; i++)
		f->lv[i] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->disc = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	static const uint8_t dummy_px[4] = {0, 0, 0, 0};
	const uint8_t *dummy_data[1] = {dummy_px};
	f->dummy = gs_texture_create(1, 1, GS_RGBA, 1, dummy_data, 0);
	obs_leave_graphics();
	if (!f->effect)
		HZ_LOG(LOG_ERROR, "failed to load %s: %s", path ? path : "lensblur.effect", err ? err : "(unknown)");
	bfree(err);
	bfree(path);
	if (!f->effect) {
		lb_destroy(f);
		return NULL;
	}

	gs_effect_t *e = f->effect;
	f->e_image = gs_effect_get_param_by_name(e, "image");
	f->e_texel = gs_effect_get_param_by_name(e, "texel");
	f->e_radius = gs_effect_get_param_by_name(e, "radius");
	f->e_bokeh = gs_effect_get_param_by_name(e, "bokeh");
	f->e_ramp = gs_effect_get_param_by_name(e, "ramp");
	f->e_cubic = gs_effect_get_param_by_name(e, "cubic");

	lb_update(f, settings);
	return f;
}

static void set_texel(struct lensblur *f, uint32_t w, uint32_t h)
{
	struct vec2 t;
	vec2_set(&t, 1.0f / (float)w, 1.0f / (float)h);
	gs_effect_set_vec2(f->e_texel, &t);
}

/* 縮める回数。半径（画素）が DISC_TARGET 以下になるまで 1/2 ずつ縮める */
static int shrink_count(float radius_px)
{
	int n = 0;
	while (n < MAX_LEVELS && radius_px / (float)(1 << n) > DISC_TARGET)
		n++;
	return n;
}

static void lb_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct lensblur *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	float radius_px = f->radius_pct * 0.01f * (float)h;
	if (!w || !h || radius_px < 0.5f) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!hz_capture_input(f->context, f->input, w, h)) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	int n = shrink_count(radius_px);
	uint32_t dw = w, dh = h;
	gs_texrender_t *last;

	/* 線形光に直して縮める */
	if (n == 0) {
		gs_effect_set_texture(f->e_image, gs_texrender_get_texture(f->input));
		f->disc_ok = hz_draw_pass(f->lv[0], w, h, f->effect, "Lin");
		last = f->lv[0];
	} else {
		f->disc_ok = true;
		for (int k = 1; k <= n && f->disc_ok; k++) {
			dw = (w >> k) ? (w >> k) : 1;
			dh = (h >> k) ? (h >> k) : 1;
			gs_effect_set_texture(f->e_image, gs_texrender_get_texture(k == 1 ? f->input : f->lv[k - 1]));
			set_texel(f, dw, dh);
			f->disc_ok = hz_draw_pass(f->lv[k], dw, dh, f->effect, k == 1 ? "DownLin" : "Down2");
		}
		last = f->lv[n];
	}

	/* 円盤状にぼかす */
	if (f->disc_ok) {
		gs_effect_set_texture(f->e_image, gs_texrender_get_texture(last));
		set_texel(f, dw, dh);
		gs_effect_set_float(f->e_radius, radius_px / (float)(1 << n));
		gs_effect_set_float(f->e_bokeh, f->bokeh);
		gs_effect_set_float(f->e_ramp, f->ramp);
		f->disc_ok = hz_draw_pass(f->disc, dw, dh, f->effect, "Disc");
	}
	if (!f->disc_ok) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	gs_texture_t *tex = gs_texrender_get_texture(f->disc);
	gs_effect_set_texture(f->e_image, tex ? tex : f->dummy);
	set_texel(f, dw, dh); /* 拡大元（ぼかした画像）の 1 テクセル */
	gs_effect_set_float(f->e_cubic, n > 0 ? 1.0f : 0.0f);
	while (gs_effect_loop(f->effect, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
}

struct obs_source_info lensblur_filter = {
	.id = "obs_scene_relight_lensblur_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = lb_get_name,
	.create = lb_create,
	.destroy = lb_destroy,
	.update = lb_update,
	.get_defaults = lb_defaults,
	.get_properties = lb_properties,
	.video_render = lb_render,
};
