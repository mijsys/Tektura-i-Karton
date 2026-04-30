/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * layer.h - Obsługa wlr-layer-shell-unstable-v1.
 *
 * Layer-shell pozwala klientom Wayland (panel, dock, tapeta, OSD)
 * na przypięcie powierzchni do krawędzi ekranu z wyłuszczeniem
 * (exclusive zone) — tzn. zmuszeniem okien aplikacji do ustępowania
 * miejsca panelowi.
 *
 * Warstwy (od najniższej):
 *   ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND  — tapeta
 *   ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM      — widżety dolne
 *   ZWLR_LAYER_SHELL_V1_LAYER_TOP         — panel, dock
 *   ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY     — OSD, powiadomienia
 */

#ifndef TEKTURA_LAYER_H
#define TEKTURA_LAYER_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>

struct tektura_server;

/**
 * Inicjalizuje obsługę layer-shell w serwerze.
 * Rejestruje protokół i listenery; musi być wywołane po protocols_init().
 */
void layer_shell_init(struct tektura_server *server);

/**
 * Układa powierzchnie layer-shell na danym wyjściu.
 * Wywołaj przy każdej zmianie rozmiaru wyjścia lub zmianie exclusive zone.
 *
 * @param server  Główny serwer.
 * @param output  Wyjście do przeliczenia.
 */
void layer_shell_arrange(struct tektura_server *server,
	struct wlr_output *output);

#endif /* TEKTURA_LAYER_H */
