/*
 * obs-scene-relight の動作確認用：OBS をヘッドレスで動かして 1 フレームを書き出す
 *
 *   headless-render <plugin.so> <data dir> <bg image> <fg image> <out.rgba> [options]
 *     --x 0.66 --y 1.0 --h 1.0   前景の下端中央の位置と高さ（キャンバス比）
 *     --no-harmonizer            Harmonizer を付けない
 *     --finish                   シーンに Finish を付ける
 *     --set key=value            Harmonizer の設定（数値）。複数可
 *     --fset key=value           Finish の設定（数値）。複数可
 *     --lensblur                 背景に「背景ぼかし」を付ける
 *     --lset key=value           背景ぼかしの設定（数値）。複数可
 *     --frames N                 書き出すまでのフレーム数（既定 20）
 *
 * Linux 専用（X11 + EGL）。DISPLAY が必要（Xvfb で可）。
 * 出力は 1920x1080 の RGBA 生データ。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs.h>
#include <obs-nix-platform.h>
#include <util/threading.h>
#include <util/platform.h>

#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CW 1920
#define CH 1080

struct grab {
	int frames;
	int target;
	FILE *out;
	os_event_t *done;
};

static void raw_cb(void *param, struct video_data *frame)
{
	struct grab *g = param;
	if (g->frames++ != g->target)
		return;
	for (int y = 0; y < CH; y++)
		fwrite(frame->data[0] + (size_t)y * frame->linesize[0], 4, CW, g->out);
	fflush(g->out);
	os_event_signal(g->done);
}

static void apply_kv(obs_data_t *s, const char *kv)
{
	char key[128];
	const char *eq = strchr(kv, '=');
	if (!eq || (size_t)(eq - kv) >= sizeof(key))
		return;
	memcpy(key, kv, eq - kv);
	key[eq - kv] = 0;
	const char *val = eq + 1;
	if (strcmp(val, "true") == 0 || strcmp(val, "false") == 0)
		obs_data_set_bool(s, key, val[0] == 't');
	else if (strchr(val, '.'))
		obs_data_set_double(s, key, atof(val));
	else
		obs_data_set_int(s, key, atoll(val));
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage: %s plugin.so datadir bg fg out.rgba [options]\n", argv[0]);
		return 2;
	}
	const char *plugin = argv[1], *datadir = argv[2], *bgfile = argv[3], *fgfile = argv[4], *outfile = argv[5];
	float px = 0.66f, py = 1.0f, ph = 1.0f;
	bool harm = true, finish = false, lensblur = false;
	int nframes = 20;
	obs_data_t *hset = obs_data_create();
	obs_data_t *fset = obs_data_create();
	obs_data_t *lset = obs_data_create();
	for (int i = 6; i < argc; i++) {
		if (!strcmp(argv[i], "--x") && i + 1 < argc)
			px = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--y") && i + 1 < argc)
			py = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--h") && i + 1 < argc)
			ph = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--no-harmonizer"))
			harm = false;
		else if (!strcmp(argv[i], "--finish"))
			finish = true;
		else if (!strcmp(argv[i], "--set") && i + 1 < argc)
			apply_kv(hset, argv[++i]);
		else if (!strcmp(argv[i], "--fset") && i + 1 < argc)
			apply_kv(fset, argv[++i]);
		else if (!strcmp(argv[i], "--lensblur"))
			lensblur = true;
		else if (!strcmp(argv[i], "--lset") && i + 1 < argc)
			apply_kv(lset, argv[++i]);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			nframes = atoi(argv[++i]);
	}

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "no X display\n");
		return 1;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(dpy);

	if (!obs_startup("en-US", NULL, NULL))
		return 1;

	struct obs_video_info ovi = {0};
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = CW;
	ovi.base_height = ovi.output_height = CH;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.colorspace = VIDEO_CS_SRGB;
	ovi.range = VIDEO_RANGE_FULL;
	ovi.scale_type = OBS_SCALE_BILINEAR;
	ovi.gpu_conversion = true;
	ovi.adapter = 0;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed\n");
		return 1;
	}

	/* 標準の image_source（システムの OBS プラグイン）と、このプラグイン */
	obs_add_module_path("/usr/lib/x86_64-linux-gnu/obs-plugins", "/usr/share/obs/obs-plugins/%module%");
	obs_module_t *img_mod = NULL;
	if (obs_open_module(&img_mod, "/usr/lib/x86_64-linux-gnu/obs-plugins/image-source.so",
			    "/usr/share/obs/obs-plugins/image-source") != MODULE_SUCCESS ||
	    !obs_init_module(img_mod)) {
		fprintf(stderr, "failed to load image-source\n");
		return 1;
	}
	obs_module_t *mod = NULL;
	if (obs_open_module(&mod, plugin, datadir) != MODULE_SUCCESS || !obs_init_module(mod)) {
		fprintf(stderr, "failed to load %s\n", plugin);
		return 1;
	}

	obs_scene_t *scene = obs_scene_create("scene");
	obs_source_t *scene_src = obs_scene_get_source(scene);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "file", bgfile);
	obs_source_t *bg = obs_source_create("image_source", "bg", s, NULL);
	obs_data_set_string(s, "file", fgfile);
	obs_source_t *fg = obs_source_create("image_source", "fg", s, NULL);
	obs_data_release(s);

	/* 画像の読み込みを待つ */
	for (int i = 0; i < 200 && (!obs_source_get_width(bg) || !obs_source_get_width(fg)); i++)
		os_sleep_ms(10);
	uint32_t bw = obs_source_get_width(bg), bh = obs_source_get_height(bg);
	uint32_t fw = obs_source_get_width(fg), fh = obs_source_get_height(fg);
	if (!bw || !fw) {
		fprintf(stderr, "image load failed\n");
		return 1;
	}

	obs_sceneitem_t *bi = obs_scene_add(scene, bg);
	struct vec2 v;
	vec2_set(&v, (float)CW / bw, (float)CH / bh);
	obs_sceneitem_set_scale(bi, &v);

	obs_sceneitem_t *fi = obs_scene_add(scene, fg);
	float sc = ph * CH / fh;
	vec2_set(&v, sc, sc);
	obs_sceneitem_set_scale(fi, &v);
	vec2_set(&v, px * CW - fw * sc / 2, py * CH - fh * sc);
	obs_sceneitem_set_pos(fi, &v);

	if (harm) {
		obs_data_set_string(hset, "bg_source", "bg");
		obs_data_set_double(hset, "smoothing", 0.0);
		obs_source_t *flt = obs_source_create("obs_harmonizer_filter", "harmonizer", hset, NULL);
		if (!flt) {
			fprintf(stderr, "failed to create harmonizer filter\n");
			return 1;
		}
		obs_source_filter_add(fg, flt);
		obs_source_release(flt);
	}
	if (lensblur) {
		obs_source_t *flt = obs_source_create("obs_scene_relight_lensblur_filter", "lensblur", lset, NULL);
		if (!flt) {
			fprintf(stderr, "failed to create lensblur filter\n");
			return 1;
		}
		obs_source_filter_add(bg, flt);
		obs_source_release(flt);
	}
	if (finish) {
		obs_data_set_double(fset, "leak_smoothing", 0.0);
		obs_data_set_bool(fset, "grain_animate", false);
		obs_source_t *flt = obs_source_create("obs_harmonizer_finish_filter", "finish", fset, NULL);
		if (!flt) {
			fprintf(stderr, "failed to create finish filter\n");
			return 1;
		}
		obs_source_filter_add(scene_src, flt);
		obs_source_release(flt);
	}

	obs_set_output_source(0, scene_src);

	struct grab g = {0};
	g.target = nframes;
	g.out = fopen(outfile, "wb");
	if (!g.out)
		return 1;
	os_event_init(&g.done, OS_EVENT_TYPE_MANUAL);
	obs_add_raw_video_callback(NULL, raw_cb, &g);
	int rc = os_event_timedwait(g.done, 60000);
	obs_remove_raw_video_callback(raw_cb, &g);
	fclose(g.out);

	obs_set_output_source(0, NULL);
	obs_source_release(bg);
	obs_source_release(fg);
	obs_scene_release(scene);
	obs_data_release(hset);
	obs_data_release(fset);
	obs_shutdown();
	if (rc != 0) {
		fprintf(stderr, "timed out waiting for frames\n");
		return 1;
	}
	return 0;
}
