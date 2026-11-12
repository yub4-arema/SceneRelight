/*
 * obs-harmonizer
 * Copyright (C) 2026 obs-harmonizer contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <graphics/matrix4.h>

#define HZ_LOG(level, fmt, ...) blog(level, "[obs-harmonizer] " fmt, ##__VA_ARGS__)

/* 前フレームの結果を読みながら新しい結果を書くための 2 枚組 */
struct hz_pingpong {
	gs_texrender_t *tr[2];
	int cur;     /* 最後に書いた方 */
	bool valid;  /* 一度でも書いたか */
};

void hz_pingpong_init(struct hz_pingpong *pp, enum gs_color_format fmt);
void hz_pingpong_free(struct hz_pingpong *pp);
/* 書き込み先を返し、cur を進める。prev_out には直前の結果（無ければ NULL） */
gs_texrender_t *hz_pingpong_next(struct hz_pingpong *pp, gs_texture_t **prev_out);
gs_texture_t *hz_pingpong_tex(const struct hz_pingpong *pp);

/* effect の technique を w x h の texrender に全面描画する（ブレンドなし） */
bool hz_draw_pass(gs_texrender_t *tr, uint32_t w, uint32_t h, gs_effect_t *effect, const char *tech);

/* フィルターの入力（target の描画結果）を texrender に取り込む。premultiplied のまま */
bool hz_capture_input(obs_source_t *filter, gs_texrender_t *tr, uint32_t w, uint32_t h);

/* 任意のソースを w x h に縮めて描く */
bool hz_render_source_small(obs_source_t *src, gs_texrender_t *tr, uint32_t w, uint32_t h);

/* 時間平滑の重み。tau 秒、dt 秒 */
float hz_smooth_blend(float tau, float dt);

/*
 * 前景 uv -> 背景 uv のアフィン変換を、表示中のシーンから探す。
 *   bu = u_row.x * u + u_row.y * v + u_row.z
 *   bv = v_row.x * u + v_row.y * v + v_row.z
 * 見つからない場合は「前景も背景もキャンバス全面」とみなした恒等変換を返し false。
 */
bool hz_find_mapping(obs_source_t *fg, obs_source_t *bg, struct vec4 *u_row, struct vec4 *v_row);

/* 映像を持つソースとシーンを list プロパティに並べる。exclude は除外 */
void hz_fill_source_list(obs_property_t *list, obs_source_t *exclude);

/* 弱参照から強参照を得る（呼び出し側で release） */
obs_source_t *hz_weak_get(obs_weak_source_t *weak);
