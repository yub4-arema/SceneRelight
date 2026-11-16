/*
 * obs-scene-relight
 * Copyright (C) 2026 obs-scene-relight contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hz-util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void hz_pingpong_init(struct hz_pingpong *pp, enum gs_color_format fmt)
{
	pp->tr[0] = gs_texrender_create(fmt, GS_ZS_NONE);
	pp->tr[1] = gs_texrender_create(fmt, GS_ZS_NONE);
	pp->cur = 0;
	pp->valid = false;
}

void hz_pingpong_free(struct hz_pingpong *pp)
{
	gs_texrender_destroy(pp->tr[0]);
	gs_texrender_destroy(pp->tr[1]);
	pp->tr[0] = pp->tr[1] = NULL;
	pp->valid = false;
}

gs_texrender_t *hz_pingpong_next(struct hz_pingpong *pp, gs_texture_t **prev_out)
{
	gs_texture_t *prev = pp->valid ? gs_texrender_get_texture(pp->tr[pp->cur]) : NULL;
	if (prev_out)
		*prev_out = prev;
	pp->cur ^= 1;
	pp->valid = true;
	return pp->tr[pp->cur];
}

gs_texture_t *hz_pingpong_tex(const struct hz_pingpong *pp)
{
	return pp->valid ? gs_texrender_get_texture(pp->tr[pp->cur]) : NULL;
}

bool hz_draw_pass(gs_texrender_t *tr, uint32_t w, uint32_t h, gs_effect_t *effect, const char *tech)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_enable_blending(false);
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(NULL, 0, w, h);
	gs_blend_state_pop();

	gs_texrender_end(tr);
	return true;
}

bool hz_capture_input(obs_source_t *filter, gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	bool ok = false;
	if (obs_source_process_filter_begin(filter, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		/* 入力はすでに premultiplied。そのまま写す */
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		obs_source_process_filter_tech_end(filter, def, w, h, "Draw");
		gs_blend_state_pop();
		ok = true;
	}

	gs_texrender_end(tr);
	return ok;
}

bool hz_render_source_small(obs_source_t *src, gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	uint32_t sw = obs_source_get_width(src);
	uint32_t sh = obs_source_get_height(src);
	if (!sw || !sh)
		return false;

	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(src);
	gs_blend_state_pop();

	gs_texrender_end(tr);
	return true;
}

float hz_smooth_blend(float tau, float dt)
{
	if (tau <= 0.0001f || dt <= 0.0f)
		return 1.0f;
	float b = 1.0f - expf(-dt / tau);
	return b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
}

/* ------------------------------------------------------------------ シーン探索 */

struct find_items {
	obs_source_t *fg;
	obs_source_t *bg;
	obs_sceneitem_t *fg_item;
	obs_sceneitem_t *bg_item;
};

static bool find_items_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct find_items *fi = param;
	obs_source_t *s = obs_sceneitem_get_source(item);
	if (!obs_sceneitem_visible(item))
		return true;
	if (!fi->fg_item && s == fi->fg)
		fi->fg_item = item;
	if (fi->bg && !fi->bg_item && s == fi->bg)
		fi->bg_item = item;
	return true;
}

struct find_ctx {
	obs_source_t *fg;
	obs_source_t *bg;
	bool want_active;
	bool found;
	bool found_bg;
	struct matrix4 fg_m;
	struct matrix4 bg_m;
};

static bool find_scene_cb(void *param, obs_source_t *scene_src)
{
	struct find_ctx *ctx = param;
	if (ctx->want_active ? !obs_source_active(scene_src) : !obs_source_showing(scene_src))
		return true;

	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene)
		return true;

	struct find_items fi = {ctx->fg, ctx->bg, NULL, NULL};
	obs_scene_enum_items(scene, find_items_cb, &fi);
	if (!fi.fg_item)
		return true;

	obs_sceneitem_get_box_transform(fi.fg_item, &ctx->fg_m);
	ctx->found = true;
	if (fi.bg_item) {
		obs_sceneitem_get_box_transform(fi.bg_item, &ctx->bg_m);
		ctx->found_bg = true;
	}
	return false; /* 見つかったので終了 */
}

static void identity_rows(struct vec4 *u_row, struct vec4 *v_row)
{
	vec4_set(u_row, 1.0f, 0.0f, 0.0f, 0.0f);
	vec4_set(v_row, 0.0f, 1.0f, 0.0f, 0.0f);
}

bool hz_find_mapping(obs_source_t *fg, obs_source_t *bg, struct vec4 *u_row, struct vec4 *v_row)
{
	identity_rows(u_row, v_row);
	if (!fg)
		return false;

	struct find_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.fg = fg;
	ctx.bg = bg;
	ctx.want_active = true;
	obs_enum_scenes(find_scene_cb, &ctx);
	if (!ctx.found) {
		ctx.want_active = false;
		obs_enum_scenes(find_scene_cb, &ctx);
	}
	if (!ctx.found)
		return false;

	/* 前景: キャンバス座標 = u * x + v * y + t */
	float ax = ctx.fg_m.x.x, ay = ctx.fg_m.x.y;
	float bx = ctx.fg_m.y.x, by = ctx.fg_m.y.y;
	float tx = ctx.fg_m.t.x, ty = ctx.fg_m.t.y;

	if (ctx.found_bg) {
		/* 背景の box transform の逆でキャンバス -> 背景 uv */
		float p = ctx.bg_m.x.x, q = ctx.bg_m.y.x; /* canvas.x = p*s + q*t + ox */
		float r = ctx.bg_m.x.y, s = ctx.bg_m.y.y; /* canvas.y = r*s + s*t + oy */
		float ox = ctx.bg_m.t.x, oy = ctx.bg_m.t.y;
		float det = p * s - q * r;
		if (fabsf(det) < 1e-8f)
			return false;
		float i00 = s / det, i01 = -q / det;
		float i10 = -r / det, i11 = p / det;
		/* bg = Inv * (A * [u v] + T - O) */
		float cx = tx - ox, cy = ty - oy;
		vec4_set(u_row, i00 * ax + i01 * ay, i00 * bx + i01 * by, i00 * cx + i01 * cy, 0.0f);
		vec4_set(v_row, i10 * ax + i11 * ay, i10 * bx + i11 * by, i10 * cx + i11 * cy, 0.0f);
	} else {
		/* 背景はキャンバス全面とみなす */
		struct obs_video_info ovi;
		if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
			return false;
		float cw = (float)ovi.base_width, ch = (float)ovi.base_height;
		vec4_set(u_row, ax / cw, bx / cw, tx / cw, 0.0f);
		vec4_set(v_row, ay / ch, by / ch, ty / ch, 0.0f);
	}
	return true;
}

/* ------------------------------------------------------------------ ソース一覧 */

struct list_ctx {
	obs_property_t *list;
	obs_source_t *exclude;
};

static bool add_source_cb(void *param, obs_source_t *src)
{
	struct list_ctx *lc = param;
	if (src == lc->exclude)
		return true;
	uint32_t flags = obs_source_get_output_flags(src);
	if (!(flags & OBS_SOURCE_VIDEO))
		return true;
	const char *name = obs_source_get_name(src);
	if (name && *name)
		obs_property_list_add_string(lc->list, name, name);
	return true;
}

void hz_fill_source_list(obs_property_t *list, obs_source_t *exclude)
{
	struct list_ctx lc = {list, exclude};
	obs_property_list_add_string(list, obs_module_text("Harmonizer.NoSource"), "");
	obs_enum_scenes(add_source_cb, &lc);
	obs_enum_sources(add_source_cb, &lc);
}

obs_source_t *hz_weak_get(obs_weak_source_t *weak)
{
	return weak ? obs_weak_source_get_source(weak) : NULL;
}
