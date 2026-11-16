/*
 * obs-scene-relight
 * Copyright (C) 2026 obs-scene-relight contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "hz-util.h"
#include "plugin-support.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-scene-relight", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Relight a foreground (avatar / character art) with the light of the background scene.";
}

extern struct obs_source_info harmonizer_filter;
extern struct obs_source_info finish_filter;

bool obs_module_load(void)
{
	obs_register_source(&harmonizer_filter);
	obs_register_source(&finish_filter);
	HZ_LOG(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}
