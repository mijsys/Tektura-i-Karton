/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * xwayland.h - Integracja Xwayland (X11 apps).
 */

#ifndef TEKTURA_XWAYLAND_H
#define TEKTURA_XWAYLAND_H

#include <stdbool.h>

struct tektura_server;

bool xwayland_init(struct tektura_server *server);
void xwayland_destroy(struct tektura_server *server);

#endif /* TEKTURA_XWAYLAND_H */
