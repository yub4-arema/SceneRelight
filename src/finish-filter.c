/*
 * obs-scene-relight Finish: 網点・グレイン・光漏れ・周辺減光
 * シーンやグループに掛けて、前景と背景を同じ「カメラ」で撮ったように見せる
 * Copyright (C) 2026 obs-scene-relight contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hz-util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>

#define S_HALFTONE "halftone"
#define S_HT_CELL "halftone_cell"
#define S_HT_ANGLE "halftone_angle"
#define S_HT_EDGE "halftone_edge"
#define S_GRAIN "grain"
#define S_GRAIN_SIZE "grain_size"
#define S_GRAIN_ANIM "grain_animate"
#define S_LEAK "leak"
#define S_LEAK_SIZE "leak_size"
#define S_LEAK_AUTO "leak_auto"
#define S_LEAK_X "leak_x"
#define S_LEAK_Y "leak_y"
#define S_LEAK_AUTOCOLOR "leak_auto_color"
#define S_LEAK_COLOR "leak_color"
#define S_LEAK_SMOOTH "leak_smoothing"
#define S_VIGNETTE "vignette"

struct finish {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *e_image, *e_prev, *e_leakstats, *e_texel, *e_size, *e_blend;
	gs_eparam_t *e_halftone, *e_ht_cell, *e_ht_angle, *e_ht_edge;
	gs_eparam_t *e_grain, *e_grain_size, *e_seed;
	gs_eparam_t *e_leak, *e_leak_size, *e_leak_auto, *e_leak_pos, *e_leak_color, *e_leak_autocolor;
	gs_eparam_t *e_vignette;

	gs_texrender_t *input;
	gs_texrender_t *small;
	struct hz_pingpong leakstats;

	float halftone, ht_cell, ht_angle, ht_edge;
	float grain, grain_size;
	bool grain_anim;
	float leak, leak_size, leak_auto, leak_smooth;
	struct vec2 leak_pos;
	struct vec3 leak_color;
	bool leak_autocolor;
	float vignette;

	float dt;
	float time;
	bool processed;
	bool input_ok;
	uint32_t in_w, in_h;
};

static const char *fin_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Finish");
}

static void fin_update(void *data, obs_data_t *s)
{
	struct finish *f = data;
	f->halftone = (float)obs_data_get_double(s, S_HALFTONE);
	f->ht_cell = (float)obs_data_get_double(s, S_HT_CELL);
	f->ht_angle = (float)(obs_data_get_double(s, S_HT_ANGLE) * M_PI / 180.0);
	f->ht_edge = (float)obs_data_get_double(s, S_HT_EDGE);
	f->grain = (float)obs_data_get_double(s, S_GRAIN);
	f->grain_size = (float)obs_data_get_double(s, S_GRAIN_SIZE);
	f->grain_anim = obs_data_get_bool(s, S_GRAIN_ANIM);
	f->leak = (float)obs_data_get_double(s, S_LEAK);
	f->leak_size = (float)obs_data_get_double(s, S_LEAK_SIZE);
	f->leak_auto = (float)obs_data_get_double(s, S_LEAK_AUTO);
	f->leak_smooth = (float)obs_data_get_double(s, S_LEAK_SMOOTH);
	vec2_set(&f->leak_pos, (float)obs_data_get_double(s, S_LEAK_X), (float)obs_data_get_double(s, S_LEAK_Y));
	f->leak_autocolor = obs_data_get_bool(s, S_LEAK_AUTOCOLOR);
	uint32_t c = (uint32_t)obs_data_get_int(s, S_LEAK_COLOR);
	vec3_set(&f->leak_color, (float)(c & 0xff) / 255.0f, (float)((c >> 8) & 0xff) / 255.0f,
		 (float)((c >> 16) & 0xff) / 255.0f);
	f->vignette = (float)obs_data_get_double(s, S_VIGNETTE);
}

static void fin_defaults(obs_data_t *s)
{
	obs_data_set_default_double(s, S_HALFTONE, 0.35);
	obs_data_set_default_double(s, S_HT_CELL, 7.0);
	obs_data_set_default_double(s, S_HT_ANGLE, 45.0);
	obs_data_set_default_double(s, S_HT_EDGE, 0.7);
	obs_data_set_default_double(s, S_GRAIN, 0.06);
	obs_data_set_default_double(s, S_GRAIN_SIZE, 1.4);
	obs_data_set_default_bool(s, S_GRAIN_ANIM, true);
	obs_data_set_default_double(s, S_LEAK, 0.3);
	obs_data_set_default_double(s, S_LEAK_SIZE, 0.45);
	obs_data_set_default_double(s, S_LEAK_AUTO, 1.0);
	obs_data_set_default_double(s, S_LEAK_X, 0.9);
	obs_data_set_default_double(s, S_LEAK_Y, 0.1);
	obs_data_set_default_bool(s, S_LEAK_AUTOCOLOR, true);
	obs_data_set_default_int(s, S_LEAK_COLOR, 0xff80c8ff);
	obs_data_set_default_double(s, S_LEAK_SMOOTH, 1.5);
	obs_data_set_default_double(s, S_VIGNETTE, 0.25);
}

static bool leak_autocolor_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	obs_property_set_visible(obs_properties_get(props, S_LEAK_COLOR), !obs_data_get_bool(s, S_LEAK_AUTOCOLOR));
	return true;
}

static obs_properties_t *fin_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *ht = obs_properties_create();
	obs_properties_add_float_slider(ht, S_HALFTONE, obs_module_text("Finish.Halftone"), 0.0, 1.0, 0.01);
	obs_property_t *cell =
		obs_properties_add_float_slider(ht, S_HT_CELL, obs_module_text("Finish.HalftoneCell"), 2.0, 32.0, 0.5);
	obs_property_float_set_suffix(cell, " px");
	obs_properties_add_float_slider(ht, S_HT_ANGLE, obs_module_text("Finish.HalftoneAngle"), 0.0, 90.0, 1.0);
	obs_properties_add_float_slider(ht, S_HT_EDGE, obs_module_text("Finish.HalftoneEdge"), 0.0, 1.0, 0.01);
	obs_properties_add_group(props, "halftone_group", obs_module_text("Finish.HalftoneGroup"), OBS_GROUP_NORMAL,
				 ht);

	obs_properties_t *gr = obs_properties_create();
	obs_properties_add_float_slider(gr, S_GRAIN, obs_module_text("Finish.Grain"), 0.0, 0.5, 0.005);
	obs_properties_add_float_slider(gr, S_GRAIN_SIZE, obs_module_text("Finish.GrainSize"), 1.0, 6.0, 0.1);
	obs_properties_add_bool(gr, S_GRAIN_ANIM, obs_module_text("Finish.GrainAnimate"));
	obs_properties_add_group(props, "grain_group", obs_module_text("Finish.GrainGroup"), OBS_GROUP_NORMAL, gr);

	obs_properties_t *lk = obs_properties_create();
	obs_properties_add_float_slider(lk, S_LEAK, obs_module_text("Finish.Leak"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(lk, S_LEAK_SIZE, obs_module_text("Finish.LeakSize"), 0.05, 1.5, 0.01);
	obs_property_t *la =
		obs_properties_add_float_slider(lk, S_LEAK_AUTO, obs_module_text("Finish.LeakAuto"), 0.0, 1.0, 0.01);
	obs_property_set_long_description(la, obs_module_text("Finish.LeakAuto.Desc"));
	obs_properties_add_float_slider(lk, S_LEAK_X, obs_module_text("Finish.LeakX"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(lk, S_LEAK_Y, obs_module_text("Finish.LeakY"), 0.0, 1.0, 0.01);
	obs_property_t *lac = obs_properties_add_bool(lk, S_LEAK_AUTOCOLOR, obs_module_text("Finish.LeakAutoColor"));
	obs_property_set_modified_callback(lac, leak_autocolor_changed);
	obs_properties_add_color(lk, S_LEAK_COLOR, obs_module_text("Finish.LeakColor"));
	obs_property_t *ls = obs_properties_add_float_slider(lk, S_LEAK_SMOOTH, obs_module_text("Finish.LeakSmoothing"),
							     0.0, 10.0, 0.1);
	obs_property_float_set_suffix(ls, " s");
	obs_properties_add_group(props, "leak_group", obs_module_text("Finish.LeakGroup"), OBS_GROUP_NORMAL, lk);

	obs_properties_add_float_slider(props, S_VIGNETTE, obs_module_text("Finish.Vignette"), 0.0, 1.0, 0.01);
	return props;
}

static void fin_destroy(void *data)
{
	struct finish *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texrender_destroy(f->input);
	gs_texrender_destroy(f->small);
	hz_pingpong_free(&f->leakstats);
	obs_leave_graphics();
	bfree(f);
}

static void *fin_create(obs_data_t *settings, obs_source_t *context)
{
	struct finish *f = bzalloc(sizeof(*f));
	f->context = context;

	char *path = obs_module_file("effects/finish.effect");
	char *err = NULL;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &err);
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->small = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	hz_pingpong_init(&f->leakstats, GS_RGBA32F);
	obs_leave_graphics();
	if (!f->effect)
		HZ_LOG(LOG_ERROR, "failed to load %s: %s", path ? path : "finish.effect", err ? err : "(unknown)");
	bfree(err);
	bfree(path);
	if (!f->effect) {
		fin_destroy(f);
		return NULL;
	}

	gs_effect_t *e = f->effect;
	f->e_image = gs_effect_get_param_by_name(e, "image");
	f->e_prev = gs_effect_get_param_by_name(e, "prev");
	f->e_leakstats = gs_effect_get_param_by_name(e, "leakstats");
	f->e_texel = gs_effect_get_param_by_name(e, "texel");
	f->e_size = gs_effect_get_param_by_name(e, "size");
	f->e_blend = gs_effect_get_param_by_name(e, "blend");
	f->e_halftone = gs_effect_get_param_by_name(e, "halftone");
	f->e_ht_cell = gs_effect_get_param_by_name(e, "halftone_cell");
	f->e_ht_angle = gs_effect_get_param_by_name(e, "halftone_angle");
	f->e_ht_edge = gs_effect_get_param_by_name(e, "halftone_edge");
	f->e_grain = gs_effect_get_param_by_name(e, "grain");
	f->e_grain_size = gs_effect_get_param_by_name(e, "grain_size");
	f->e_seed = gs_effect_get_param_by_name(e, "seed");
	f->e_leak = gs_effect_get_param_by_name(e, "leak");
	f->e_leak_size = gs_effect_get_param_by_name(e, "leak_size");
	f->e_leak_auto = gs_effect_get_param_by_name(e, "leak_auto");
	f->e_leak_pos = gs_effect_get_param_by_name(e, "leak_pos");
	f->e_leak_color = gs_effect_get_param_by_name(e, "leak_color");
	f->e_leak_autocolor = gs_effect_get_param_by_name(e, "leak_auto_color");
	f->e_vignette = gs_effect_get_param_by_name(e, "vignette");

	fin_update(f, settings);
	return f;
}

static void fin_tick(void *data, float seconds)
{
	struct finish *f = data;
	f->dt = seconds;
	f->time += seconds;
	if (f->time > 1000.0f)
		f->time -= 1000.0f;
	f->processed = false;
}

static void fin_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct finish *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!w || !h) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!f->processed || w != f->in_w || h != f->in_h) {
		f->processed = true;
		f->in_w = w;
		f->in_h = h;
		f->input_ok = hz_capture_input(f->context, f->input, w, h);
		if (f->input_ok && f->leak > 0.0001f) {
			gs_effect_set_texture(f->e_image, gs_texrender_get_texture(f->input));
			hz_draw_pass(f->small, 32, 18, f->effect, "Down");

			gs_texture_t *prev = NULL;
			gs_texrender_t *dst = hz_pingpong_next(&f->leakstats, &prev);
			gs_effect_set_texture(f->e_image, gs_texrender_get_texture(f->small));
			gs_effect_set_texture(f->e_prev, prev);
			gs_effect_set_float(f->e_blend, prev ? hz_smooth_blend(f->leak_smooth, f->dt) : 1.0f);
			hz_draw_pass(dst, 2, 1, f->effect, "LeakStats");
		}
	}
	if (!f->input_ok) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	struct vec2 v;
	gs_effect_set_texture(f->e_image, gs_texrender_get_texture(f->input));
	gs_effect_set_texture(f->e_leakstats, hz_pingpong_tex(&f->leakstats));
	vec2_set(&v, 1.0f / (float)w, 1.0f / (float)h);
	gs_effect_set_vec2(f->e_texel, &v);
	vec2_set(&v, (float)w, (float)h);
	gs_effect_set_vec2(f->e_size, &v);
	gs_effect_set_float(f->e_halftone, f->halftone);
	gs_effect_set_float(f->e_ht_cell, f->ht_cell);
	gs_effect_set_float(f->e_ht_angle, f->ht_angle);
	gs_effect_set_float(f->e_ht_edge, f->ht_edge);
	gs_effect_set_float(f->e_grain, f->grain);
	gs_effect_set_float(f->e_grain_size, f->grain_size);
	gs_effect_set_float(f->e_seed, f->grain_anim ? floorf(f->time * 24.0f) : 1.0f);
	gs_effect_set_float(f->e_leak, hz_pingpong_tex(&f->leakstats) ? f->leak : 0.0f);
	gs_effect_set_float(f->e_leak_size, f->leak_size);
	gs_effect_set_float(f->e_leak_auto, f->leak_auto);
	gs_effect_set_vec2(f->e_leak_pos, &f->leak_pos);
	gs_effect_set_vec3(f->e_leak_color, &f->leak_color);
	gs_effect_set_float(f->e_leak_autocolor, f->leak_autocolor ? 1.0f : 0.0f);
	gs_effect_set_float(f->e_vignette, f->vignette);

	while (gs_effect_loop(f->effect, "Draw"))
		gs_draw_sprite(gs_texrender_get_texture(f->input), 0, w, h);
}

struct obs_source_info finish_filter = {
	.id = "obs_harmonizer_finish_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fin_get_name,
	.create = fin_create,
	.destroy = fin_destroy,
	.update = fin_update,
	.get_defaults = fin_defaults,
	.get_properties = fin_properties,
	.video_tick = fin_tick,
	.video_render = fin_render,
};
