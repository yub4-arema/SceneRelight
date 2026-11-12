/*
 * obs-harmonizer: 前景（立ち絵・アバター）を背景になじませるフィルター
 * Copyright (C) 2026 obs-harmonizer contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 1 フレームの流れ（GPU のみ。CPU への読み戻しなし）
 *   背景ソース -> 256x144 -> 32x18 ブロック平均 -> 統計 4x1（時間平滑）
 *                                       └-> ぼかし -> 環境光マップ 32x18（時間平滑）
 *   前景（このフィルターの入力）-> 32x32 -> 統計 1x1（時間平滑）
 *   本体: harmonizer.effect
 */
#include "hz-util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/dstr.h>

#define BG_W 256
#define BG_H 144
#define GRID_W 32
#define GRID_H 18
#define FG_SMALL 32

#define S_BG "bg_source"
#define S_AUTO_PLACE "auto_place"
#define S_STRENGTH "strength"
#define S_TINT "tint"
#define S_EXPOSURE "exposure"
#define S_CONTRAST "contrast"
#define S_SHADOW_TINT "shadow_tint"
#define S_SMOOTH "smoothing"
#define S_SHADING "shading"
#define S_SPREAD "shading_spread"
#define S_RIM "rim"
#define S_RIM_WIDTH "rim_width"
#define S_RIM_DIR "rim_dir"

struct light_params {
	float kind;
	struct vec2 pos;
	float angle;
	float size;
	float soft;
	float amount;
	struct vec3 color;
	float autoc;
};

struct light_eparams {
	gs_eparam_t *kind, *pos, *angle, *size, *soft, *amount, *color, *autoc;
};

struct harmonizer {
	obs_source_t *context;

	gs_effect_t *prep;
	gs_effect_t *main;

	/* prep のパラメーター */
	gs_eparam_t *p_image, *p_image2, *p_prev, *p_texel, *p_blend, *p_sigma;

	/* main のパラメーター */
	gs_eparam_t *m_image, *m_env, *m_stats, *m_fgstats, *m_texel, *m_map_u, *m_map_v, *m_has_bg;
	gs_eparam_t *m_strength, *m_tint, *m_exposure, *m_contrast, *m_shadow_tint, *m_shading;
	gs_eparam_t *m_rim, *m_rim_width, *m_rim_dir, *m_aspect;
	struct light_eparams m_l[2];

	gs_texrender_t *input;
	gs_texrender_t *bg_small;
	gs_texrender_t *blocks;
	gs_texrender_t *blocks_sq;
	gs_texrender_t *blur_h;
	gs_texrender_t *fg_small;
	struct hz_pingpong stats;
	struct hz_pingpong env;
	struct hz_pingpong fgstats;

	obs_weak_source_t *bg;
	bool auto_place;

	float strength, tint, exposure, contrast, shadow_tint, smoothing;
	float shading, spread, rim, rim_width, rim_dir;
	struct light_params light[2];

	/* フレームごとの状態 */
	float dt;
	bool processed; /* このフレームの前処理を済ませたか */
	bool rendering_bg; /* 背景の描画中（背景に自分が含まれる場合の再帰よけ） */
	bool has_bg;
	bool input_ok;
	uint32_t in_w, in_h;
	struct vec4 map_u, map_v;
};

/* ------------------------------------------------------------------ 設定 */

static const char *light_key(int idx, const char *name, struct dstr *buf)
{
	dstr_printf(buf, "light%d_%s", idx + 1, name);
	return buf->array;
}

static void load_light(obs_data_t *s, int idx, struct light_params *lp)
{
	struct dstr k = {0};
	lp->kind = (float)obs_data_get_int(s, light_key(idx, "kind", &k));
	lp->pos.x = (float)obs_data_get_double(s, light_key(idx, "x", &k));
	lp->pos.y = (float)obs_data_get_double(s, light_key(idx, "y", &k));
	lp->angle = (float)(obs_data_get_double(s, light_key(idx, "angle", &k)) * M_PI / 180.0);
	lp->size = (float)obs_data_get_double(s, light_key(idx, "size", &k));
	lp->soft = (float)obs_data_get_double(s, light_key(idx, "soft", &k));
	lp->amount = (float)obs_data_get_double(s, light_key(idx, "amount", &k));
	lp->autoc = obs_data_get_bool(s, light_key(idx, "auto_color", &k)) ? 1.0f : 0.0f;
	uint32_t c = (uint32_t)obs_data_get_int(s, light_key(idx, "color", &k));
	/* OBS の色は 0xAABBGGRR。sRGB として扱い線形へ */
	float r = (float)(c & 0xff) / 255.0f;
	float g = (float)((c >> 8) & 0xff) / 255.0f;
	float b = (float)((c >> 16) & 0xff) / 255.0f;
	vec3_set(&lp->color, powf(r, 2.2f), powf(g, 2.2f), powf(b, 2.2f));
	dstr_free(&k);
}

static void hz_update(void *data, obs_data_t *s)
{
	struct harmonizer *f = data;

	const char *name = obs_data_get_string(s, S_BG);
	obs_weak_source_release(f->bg);
	f->bg = NULL;
	if (name && *name) {
		obs_source_t *src = obs_get_source_by_name(name);
		if (src) {
			f->bg = obs_source_get_weak_source(src);
			obs_source_release(src);
		}
	}

	f->auto_place = obs_data_get_bool(s, S_AUTO_PLACE);
	f->strength = (float)obs_data_get_double(s, S_STRENGTH);
	f->tint = (float)obs_data_get_double(s, S_TINT);
	f->exposure = (float)obs_data_get_double(s, S_EXPOSURE);
	f->contrast = (float)obs_data_get_double(s, S_CONTRAST);
	f->shadow_tint = (float)obs_data_get_double(s, S_SHADOW_TINT);
	f->smoothing = (float)obs_data_get_double(s, S_SMOOTH);
	f->shading = (float)obs_data_get_double(s, S_SHADING);
	f->spread = (float)obs_data_get_double(s, S_SPREAD);
	f->rim = (float)obs_data_get_double(s, S_RIM);
	f->rim_width = (float)obs_data_get_double(s, S_RIM_WIDTH);
	f->rim_dir = (float)obs_data_get_double(s, S_RIM_DIR);
	load_light(s, 0, &f->light[0]);
	load_light(s, 1, &f->light[1]);
}

static void defaults_light(obs_data_t *s, int idx)
{
	struct dstr k = {0};
	obs_data_set_default_int(s, light_key(idx, "kind", &k), 0);
	obs_data_set_default_double(s, light_key(idx, "x", &k), idx == 0 ? 0.0 : 0.7);
	obs_data_set_default_double(s, light_key(idx, "y", &k), idx == 0 ? 0.5 : 0.25);
	obs_data_set_default_double(s, light_key(idx, "angle", &k), 0.0);
	obs_data_set_default_double(s, light_key(idx, "size", &k), 0.6);
	obs_data_set_default_double(s, light_key(idx, "soft", &k), 0.6);
	obs_data_set_default_double(s, light_key(idx, "amount", &k), idx == 0 ? -0.5 : 0.4);
	obs_data_set_default_bool(s, light_key(idx, "auto_color", &k), true);
	obs_data_set_default_int(s, light_key(idx, "color", &k), idx == 0 ? 0xff4a3a3a : 0xffb0e0ff);
	dstr_free(&k);
}

static void hz_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, S_BG, "");
	obs_data_set_default_bool(s, S_AUTO_PLACE, true);
	obs_data_set_default_double(s, S_STRENGTH, 1.0);
	obs_data_set_default_double(s, S_TINT, 0.7);
	obs_data_set_default_double(s, S_EXPOSURE, 0.35);
	obs_data_set_default_double(s, S_CONTRAST, 0.4);
	obs_data_set_default_double(s, S_SHADOW_TINT, 0.35);
	obs_data_set_default_double(s, S_SMOOTH, 0.5);
	obs_data_set_default_double(s, S_SHADING, 1.0);
	obs_data_set_default_double(s, S_SPREAD, 4.0);
	obs_data_set_default_double(s, S_RIM, 0.6);
	obs_data_set_default_double(s, S_RIM_WIDTH, 4.0);
	obs_data_set_default_double(s, S_RIM_DIR, 0.85);
	defaults_light(s, 0);
	defaults_light(s, 1);
}

static bool light_kind_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	const char *key = obs_property_name(p); /* lightN_kind */
	int idx = key[5] - '1';
	bool on = obs_data_get_int(s, key) != 0;
	bool linear = obs_data_get_int(s, key) == 1;
	struct dstr k = {0};
	const char *names[] = {"x", "y", "angle", "size", "soft", "amount", "auto_color", "color"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		obs_property_t *q = obs_properties_get(props, light_key(idx, names[i], &k));
		if (!q)
			continue;
		bool vis = on;
		if (strcmp(names[i], "angle") == 0)
			vis = on && linear;
		if (strcmp(names[i], "size") == 0)
			vis = on && !linear;
		if (strcmp(names[i], "color") == 0)
			vis = on && !obs_data_get_bool(s, light_key(idx, "auto_color", &k));
		obs_property_set_visible(q, vis);
	}
	dstr_free(&k);
	return true;
}

static bool light_autocolor_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	const char *key = obs_property_name(p); /* lightN_auto_color */
	struct dstr k = {0};
	dstr_printf(&k, "light%c_kind", key[5]);
	obs_property_t *kp = obs_properties_get(props, k.array);
	dstr_free(&k);
	return kp ? light_kind_changed(props, kp, s) : false;
}

static void add_light_group(obs_properties_t *props, int idx)
{
	struct dstr k = {0};
	struct dstr t = {0};
	obs_properties_t *g = obs_properties_create();

	obs_property_t *kind = obs_properties_add_list(g, light_key(idx, "kind", &k), obs_module_text("Light.Kind"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(kind, obs_module_text("Light.Kind.None"), 0);
	obs_property_list_add_int(kind, obs_module_text("Light.Kind.Linear"), 1);
	obs_property_list_add_int(kind, obs_module_text("Light.Kind.Radial"), 2);
	obs_property_set_modified_callback(kind, light_kind_changed);

	obs_properties_add_float_slider(g, light_key(idx, "amount", &k), obs_module_text("Light.Amount"), -1.0, 1.0,
					0.01);
	obs_properties_add_float_slider(g, light_key(idx, "x", &k), obs_module_text("Light.X"), -0.5, 1.5, 0.01);
	obs_properties_add_float_slider(g, light_key(idx, "y", &k), obs_module_text("Light.Y"), -0.5, 1.5, 0.01);
	obs_properties_add_float_slider(g, light_key(idx, "angle", &k), obs_module_text("Light.Angle"), -180.0, 180.0,
					1.0);
	obs_properties_add_float_slider(g, light_key(idx, "size", &k), obs_module_text("Light.Size"), 0.05, 3.0, 0.01);
	obs_properties_add_float_slider(g, light_key(idx, "soft", &k), obs_module_text("Light.Soft"), 0.0, 1.0, 0.01);
	obs_property_t *ac =
		obs_properties_add_bool(g, light_key(idx, "auto_color", &k), obs_module_text("Light.AutoColor"));
	obs_property_set_modified_callback(ac, light_autocolor_changed);
	obs_properties_add_color(g, light_key(idx, "color", &k), obs_module_text("Light.Color"));

	dstr_printf(&t, "%s %d", obs_module_text("Light.Group"), idx + 1);
	dstr_printf(&k, "light%d_group", idx + 1);
	obs_properties_add_group(props, k.array, t.array, OBS_GROUP_NORMAL, g);
	dstr_free(&k);
	dstr_free(&t);
}

static obs_properties_t *hz_properties(void *data)
{
	struct harmonizer *f = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_BG, obs_module_text("Harmonizer.Background"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	hz_fill_source_list(list, f ? obs_filter_get_parent(f->context) : NULL);
	obs_property_set_long_description(list, obs_module_text("Harmonizer.Background.Desc"));

	obs_property_t *ap = obs_properties_add_bool(props, S_AUTO_PLACE, obs_module_text("Harmonizer.AutoPlace"));
	obs_property_set_long_description(ap, obs_module_text("Harmonizer.AutoPlace.Desc"));

	obs_properties_add_float_slider(props, S_STRENGTH, obs_module_text("Harmonizer.Strength"), 0.0, 1.0, 0.01);

	obs_properties_t *col = obs_properties_create();
	obs_properties_add_float_slider(col, S_TINT, obs_module_text("Harmonizer.Tint"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(col, S_EXPOSURE, obs_module_text("Harmonizer.Exposure"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(col, S_CONTRAST, obs_module_text("Harmonizer.Contrast"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(col, S_SHADOW_TINT, obs_module_text("Harmonizer.ShadowTint"), 0.0, 1.0, 0.01);
	obs_property_t *sm =
		obs_properties_add_float_slider(col, S_SMOOTH, obs_module_text("Harmonizer.Smoothing"), 0.0, 5.0, 0.05);
	obs_property_float_set_suffix(sm, " s");
	obs_properties_add_group(props, "color_group", obs_module_text("Harmonizer.ColorGroup"), OBS_GROUP_NORMAL, col);

	obs_properties_t *sh = obs_properties_create();
	obs_properties_add_float_slider(sh, S_SHADING, obs_module_text("Harmonizer.Shading"), 0.0, 2.0, 0.01);
	obs_properties_add_float_slider(sh, S_SPREAD, obs_module_text("Harmonizer.Spread"), 0.0, 12.0, 0.1);
	obs_properties_add_float_slider(sh, S_RIM, obs_module_text("Harmonizer.Rim"), 0.0, 2.0, 0.01);
	obs_property_t *rw = obs_properties_add_float_slider(sh, S_RIM_WIDTH, obs_module_text("Harmonizer.RimWidth"),
							      1.0, 24.0, 0.5);
	obs_property_float_set_suffix(rw, " px");
	obs_properties_add_float_slider(sh, S_RIM_DIR, obs_module_text("Harmonizer.RimDir"), 0.0, 1.0, 0.01);
	obs_properties_add_group(props, "shade_group", obs_module_text("Harmonizer.ShadeGroup"), OBS_GROUP_NORMAL, sh);

	add_light_group(props, 0);
	add_light_group(props, 1);
	return props;
}

/* ------------------------------------------------------------------ 生成・破棄 */

static const char *hz_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Harmonizer");
}

static void get_light_eparams(gs_effect_t *e, int idx, struct light_eparams *lp)
{
	char buf[32];
#define LP(field, nm)                                          \
	snprintf(buf, sizeof(buf), "l%d_%s", idx + 1, nm);     \
	lp->field = gs_effect_get_param_by_name(e, buf);
	LP(kind, "kind");
	LP(pos, "pos");
	LP(angle, "angle");
	LP(size, "size");
	LP(soft, "soft");
	LP(amount, "amount");
	LP(color, "color");
	LP(autoc, "auto");
#undef LP
}

static gs_effect_t *load_effect(const char *file)
{
	char *path = obs_module_file(file);
	char *err = NULL;
	gs_effect_t *e = gs_effect_create_from_file(path, &err);
	if (!e)
		HZ_LOG(LOG_ERROR, "failed to load %s: %s", path ? path : file, err ? err : "(unknown)");
	bfree(err);
	bfree(path);
	return e;
}

static void hz_destroy(void *data)
{
	struct harmonizer *f = data;
	obs_enter_graphics();
	gs_effect_destroy(f->prep);
	gs_effect_destroy(f->main);
	gs_texrender_destroy(f->input);
	gs_texrender_destroy(f->bg_small);
	gs_texrender_destroy(f->blocks);
	gs_texrender_destroy(f->blocks_sq);
	gs_texrender_destroy(f->blur_h);
	gs_texrender_destroy(f->fg_small);
	hz_pingpong_free(&f->stats);
	hz_pingpong_free(&f->env);
	hz_pingpong_free(&f->fgstats);
	obs_leave_graphics();
	obs_weak_source_release(f->bg);
	bfree(f);
}

static void *hz_create(obs_data_t *settings, obs_source_t *context)
{
	struct harmonizer *f = bzalloc(sizeof(*f));
	f->context = context;

	obs_enter_graphics();
	f->prep = load_effect("effects/harmonizer-prep.effect");
	f->main = load_effect("effects/harmonizer.effect");
	f->input = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->bg_small = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->blocks = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->blocks_sq = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->blur_h = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->fg_small = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	hz_pingpong_init(&f->stats, GS_RGBA32F);
	hz_pingpong_init(&f->env, GS_RGBA16F);
	hz_pingpong_init(&f->fgstats, GS_RGBA32F);
	obs_leave_graphics();

	if (!f->prep || !f->main) {
		hz_destroy(f);
		return NULL;
	}

	gs_effect_t *p = f->prep;
	f->p_image = gs_effect_get_param_by_name(p, "image");
	f->p_image2 = gs_effect_get_param_by_name(p, "image2");
	f->p_prev = gs_effect_get_param_by_name(p, "prev");
	f->p_texel = gs_effect_get_param_by_name(p, "texel");
	f->p_blend = gs_effect_get_param_by_name(p, "blend");
	f->p_sigma = gs_effect_get_param_by_name(p, "sigma");

	gs_effect_t *m = f->main;
	f->m_image = gs_effect_get_param_by_name(m, "image");
	f->m_env = gs_effect_get_param_by_name(m, "env");
	f->m_stats = gs_effect_get_param_by_name(m, "stats");
	f->m_fgstats = gs_effect_get_param_by_name(m, "fgstats");
	f->m_texel = gs_effect_get_param_by_name(m, "texel");
	f->m_map_u = gs_effect_get_param_by_name(m, "map_u");
	f->m_map_v = gs_effect_get_param_by_name(m, "map_v");
	f->m_has_bg = gs_effect_get_param_by_name(m, "has_bg");
	f->m_strength = gs_effect_get_param_by_name(m, "strength");
	f->m_tint = gs_effect_get_param_by_name(m, "tint");
	f->m_exposure = gs_effect_get_param_by_name(m, "exposure");
	f->m_contrast = gs_effect_get_param_by_name(m, "contrast");
	f->m_shadow_tint = gs_effect_get_param_by_name(m, "shadow_tint");
	f->m_shading = gs_effect_get_param_by_name(m, "shading");
	f->m_rim = gs_effect_get_param_by_name(m, "rim");
	f->m_rim_width = gs_effect_get_param_by_name(m, "rim_width");
	f->m_rim_dir = gs_effect_get_param_by_name(m, "rim_dir");
	f->m_aspect = gs_effect_get_param_by_name(m, "aspect");
	get_light_eparams(m, 0, &f->m_l[0]);
	get_light_eparams(m, 1, &f->m_l[1]);

	vec4_set(&f->map_u, 1.0f, 0.0f, 0.0f, 0.0f);
	vec4_set(&f->map_v, 0.0f, 1.0f, 0.0f, 0.0f);

	hz_update(f, settings);
	return f;
}

/* ------------------------------------------------------------------ 毎フレーム */

static void hz_tick(void *data, float seconds)
{
	struct harmonizer *f = data;
	f->dt = seconds;
	f->processed = false;

	obs_source_t *bg = hz_weak_get(f->bg);
	if (f->auto_place) {
		obs_source_t *parent = obs_filter_get_parent(f->context);
		hz_find_mapping(parent, bg, &f->map_u, &f->map_v);
	} else {
		vec4_set(&f->map_u, 1.0f, 0.0f, 0.0f, 0.0f);
		vec4_set(&f->map_v, 0.0f, 1.0f, 0.0f, 0.0f);
	}
	obs_source_release(bg);
}

static void set_texel(gs_eparam_t *p, float w, float h)
{
	struct vec2 t;
	vec2_set(&t, 1.0f / w, 1.0f / h);
	gs_effect_set_vec2(p, &t);
}

/* 背景側の前処理。成功すれば true */
static bool process_background(struct harmonizer *f, float blend)
{
	obs_source_t *bg = hz_weak_get(f->bg);
	if (!bg)
		return false;
	f->rendering_bg = true;
	bool ok = hz_render_source_small(bg, f->bg_small, BG_W, BG_H);
	f->rendering_bg = false;
	obs_source_release(bg);
	if (!ok)
		return false;

	gs_effect_t *p = f->prep;

	/* ブロック平均 */
	gs_effect_set_texture(f->p_image, gs_texrender_get_texture(f->bg_small));
	set_texel(f->p_texel, BG_W, BG_H);
	hz_draw_pass(f->blocks, GRID_W, GRID_H, p, "Blocks");
	gs_effect_set_texture(f->p_image, gs_texrender_get_texture(f->bg_small));
	set_texel(f->p_texel, BG_W, BG_H);
	hz_draw_pass(f->blocks_sq, GRID_W, GRID_H, p, "BlocksSq");

	/* 統計 */
	gs_texture_t *prev = NULL;
	gs_texrender_t *dst = hz_pingpong_next(&f->stats, &prev);
	gs_effect_set_texture(f->p_image, gs_texrender_get_texture(f->blocks));
	gs_effect_set_texture(f->p_image2, gs_texrender_get_texture(f->blocks_sq));
	gs_effect_set_texture(f->p_prev, prev);
	gs_effect_set_float(f->p_blend, prev ? blend : 1.0f);
	hz_draw_pass(dst, 4, 1, p, "Stats");

	/* 環境光マップ */
	gs_effect_set_texture(f->p_image, gs_texrender_get_texture(f->blocks));
	set_texel(f->p_texel, GRID_W, GRID_H);
	gs_effect_set_float(f->p_sigma, f->spread * 0.5f);
	hz_draw_pass(f->blur_h, GRID_W, GRID_H, p, "BlurH");

	dst = hz_pingpong_next(&f->env, &prev);
	gs_effect_set_texture(f->p_image, gs_texrender_get_texture(f->blur_h));
	gs_effect_set_texture(f->p_prev, prev);
	set_texel(f->p_texel, GRID_W, GRID_H);
	gs_effect_set_float(f->p_sigma, f->spread * 0.5f);
	gs_effect_set_float(f->p_blend, prev ? blend : 1.0f);
	hz_draw_pass(dst, GRID_W, GRID_H, p, "BlurV");
	return true;
}

static void process_foreground_stats(struct harmonizer *f, float blend)
{
	gs_texrender_reset(f->fg_small);
	if (gs_texrender_begin(f->fg_small, FG_SMALL, FG_SMALL)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)f->in_w, 0.0f, (float)f->in_h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_enable_blending(false);
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), gs_texrender_get_texture(f->input));
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(gs_texrender_get_texture(f->input), 0, f->in_w, f->in_h);
		gs_blend_state_pop();
		gs_texrender_end(f->fg_small);
	}

	gs_texture_t *prev = NULL;
	gs_texrender_t *dst = hz_pingpong_next(&f->fgstats, &prev);
	gs_effect_set_texture(f->p_image, gs_texrender_get_texture(f->fg_small));
	gs_effect_set_texture(f->p_prev, prev);
	gs_effect_set_float(f->p_blend, prev ? blend : 1.0f);
	hz_draw_pass(dst, 1, 1, f->prep, "FgStats");
}

static void set_light(const struct light_eparams *e, const struct light_params *l)
{
	gs_effect_set_float(e->kind, l->kind);
	gs_effect_set_vec2(e->pos, &l->pos);
	gs_effect_set_float(e->angle, l->angle);
	gs_effect_set_float(e->size, l->size);
	gs_effect_set_float(e->soft, l->soft);
	gs_effect_set_float(e->amount, l->amount);
	gs_effect_set_vec3(e->color, &l->color);
	gs_effect_set_float(e->autoc, l->autoc);
}

static void hz_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct harmonizer *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!w || !h || f->rendering_bg) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	/* プレビューと番組で 2 回呼ばれても前処理は 1 回 */
	if (!f->processed || w != f->in_w || h != f->in_h) {
		f->processed = true;
		f->in_w = w;
		f->in_h = h;
		float blend = hz_smooth_blend(f->smoothing, f->dt);
		f->input_ok = hz_capture_input(f->context, f->input, w, h);
		if (f->input_ok) {
			f->has_bg = process_background(f, blend);
			process_foreground_stats(f, blend);
		}
	}
	if (!f->input_ok) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	gs_effect_set_texture(f->m_image, gs_texrender_get_texture(f->input));
	gs_effect_set_texture(f->m_env, hz_pingpong_tex(&f->env));
	gs_effect_set_texture(f->m_stats, hz_pingpong_tex(&f->stats));
	gs_effect_set_texture(f->m_fgstats, hz_pingpong_tex(&f->fgstats));
	set_texel(f->m_texel, (float)w, (float)h);
	gs_effect_set_vec4(f->m_map_u, &f->map_u);
	gs_effect_set_vec4(f->m_map_v, &f->map_v);
	gs_effect_set_float(f->m_has_bg, f->has_bg ? 1.0f : 0.0f);
	gs_effect_set_float(f->m_strength, f->strength);
	gs_effect_set_float(f->m_tint, f->tint);
	gs_effect_set_float(f->m_exposure, f->exposure);
	gs_effect_set_float(f->m_contrast, f->contrast);
	gs_effect_set_float(f->m_shadow_tint, f->shadow_tint);
	gs_effect_set_float(f->m_shading, f->shading);
	gs_effect_set_float(f->m_rim, f->rim);
	gs_effect_set_float(f->m_rim_width, f->rim_width);
	gs_effect_set_float(f->m_rim_dir, f->rim_dir);
	gs_effect_set_float(f->m_aspect, (float)w / (float)h);
	set_light(&f->m_l[0], &f->light[0]);
	set_light(&f->m_l[1], &f->light[1]);

	/* 出力は premultiplied。呼び出し側のブレンド設定のまま描く */
	while (gs_effect_loop(f->main, "Draw"))
		gs_draw_sprite(gs_texrender_get_texture(f->input), 0, w, h);
}

struct obs_source_info harmonizer_filter = {
	.id = "obs_harmonizer_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = hz_get_name,
	.create = hz_create,
	.destroy = hz_destroy,
	.update = hz_update,
	.get_defaults = hz_defaults,
	.get_properties = hz_properties,
	.video_tick = hz_tick,
	.video_render = hz_render,
};
