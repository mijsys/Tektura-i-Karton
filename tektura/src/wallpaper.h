/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * wallpaper.h - Manager konfiguracji tapet per-output.
 *
 * Uwaga: ten moduł zarządza stanem i IPC (ścieżki tapet),
 * a renderowanie realizuje klient shell/background przez layer-shell.
 */

#ifndef TEKTURA_WALLPAPER_H
#define TEKTURA_WALLPAPER_H

#include <stdbool.h>
#include <stddef.h>

struct tektura_server;

typedef struct tektura_wallpaper_manager tektura_wallpaper_manager;

tektura_wallpaper_manager *wallpaper_manager_init(struct tektura_server *server);
void wallpaper_manager_destroy(tektura_wallpaper_manager *mgr);

/* Rejestracja nowego outputu; jeśli jest ustawienie domyślne, przypisuje je outputowi */
void wallpaper_on_output_added(tektura_wallpaper_manager *mgr, const char *output_name);

/*
 * Ustaw tapetę:
 * output_name == "*" oznacza domyślną dla wszystkich outputów.
 */
bool wallpaper_set(tektura_wallpaper_manager *mgr,
	const char *output_name, const char *path);

/* Zwraca ścieżkę tapety dla outputu; fallback do domyślnej "*" */
const char *wallpaper_get(tektura_wallpaper_manager *mgr, const char *output_name);

/* Serializacja do formatu przyjaznego IPC: output=path;output2=path2 */
void wallpaper_serialize(tektura_wallpaper_manager *mgr, char *buf, size_t buf_size);

#endif /* TEKTURA_WALLPAPER_H */
