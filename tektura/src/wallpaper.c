/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * wallpaper.c - Manager stanu tapet per-output.
 */

#include "wallpaper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WALLPAPER_MAX_ENTRIES 32

typedef struct {
	char output[128];
	char path[512];
	bool used;
} wallpaper_entry;

struct tektura_wallpaper_manager {
	struct tektura_server *server;
	wallpaper_entry entries[WALLPAPER_MAX_ENTRIES];
};

static wallpaper_entry *find_entry(tektura_wallpaper_manager *mgr, const char *output) {
	for (int i = 0; i < WALLPAPER_MAX_ENTRIES; i++) {
		if (mgr->entries[i].used && strcmp(mgr->entries[i].output, output) == 0) {
			return &mgr->entries[i];
		}
	}
	return NULL;
}

static wallpaper_entry *find_free(tektura_wallpaper_manager *mgr) {
	for (int i = 0; i < WALLPAPER_MAX_ENTRIES; i++) {
		if (!mgr->entries[i].used) return &mgr->entries[i];
	}
	return NULL;
}

tektura_wallpaper_manager *wallpaper_manager_init(struct tektura_server *server) {
	tektura_wallpaper_manager *mgr = calloc(1, sizeof(*mgr));
	if (!mgr) return NULL;
	mgr->server = server;
	return mgr;
}

void wallpaper_manager_destroy(tektura_wallpaper_manager *mgr) {
	free(mgr);
}

bool wallpaper_set(tektura_wallpaper_manager *mgr,
		const char *output_name, const char *path) {
	if (!mgr || !output_name || !output_name[0] || !path) return false;

	wallpaper_entry *e = find_entry(mgr, output_name);
	if (!e) e = find_free(mgr);
	if (!e) return false;

	e->used = true;
	snprintf(e->output, sizeof(e->output), "%s", output_name);
	snprintf(e->path, sizeof(e->path), "%s", path);
	return true;
}

const char *wallpaper_get(tektura_wallpaper_manager *mgr, const char *output_name) {
	if (!mgr || !output_name) return NULL;
	wallpaper_entry *e = find_entry(mgr, output_name);
	if (e) return e->path;
	e = find_entry(mgr, "*");
	return e ? e->path : NULL;
}

void wallpaper_on_output_added(tektura_wallpaper_manager *mgr, const char *output_name) {
	if (!mgr || !output_name || !output_name[0]) return;
	if (find_entry(mgr, output_name)) return;
	const char *def = wallpaper_get(mgr, "*");
	if (def) {
		wallpaper_set(mgr, output_name, def);
	}
}

void wallpaper_serialize(tektura_wallpaper_manager *mgr, char *buf, size_t buf_size) {
	if (!buf || buf_size == 0) return;
	buf[0] = '\0';
	if (!mgr) return;

	size_t off = 0;
	for (int i = 0; i < WALLPAPER_MAX_ENTRIES; i++) {
		if (!mgr->entries[i].used) continue;
		int n = snprintf(buf + off, buf_size - off, "%s%s=%s",
			off ? ";" : "", mgr->entries[i].output, mgr->entries[i].path);
		if (n < 0) return;
		if ((size_t)n >= buf_size - off) {
			off = buf_size - 1;
			break;
		}
		off += (size_t)n;
	}
}
