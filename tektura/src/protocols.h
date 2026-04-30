/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * protocols.h - Rejestracja protokołów Wayland: xdg-shell, popupy, dekoracje.
 */

#ifndef TEKTURA_PROTOCOLS_H
#define TEKTURA_PROTOCOLS_H

#include "security_manager.h"

struct tektura_server;

void protocols_init(struct tektura_server *server);

/* Handlery zdarzeń xdg-shell — wywoływane przez server.c */
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);

/*
 * Strażnik protokołów uprzywilejowanych.
 * Wywołaj z callbacku bind() danego globala Wayland.
 * Zwraca true, jeśli dostęp jest dozwolony (lub jednorazowy).
 * Zwraca false, jeśli dostęp jest zablokowany lub oczekuje na prompt.
 */
bool protocols_check_privileged(struct tektura_server *server,
	struct wl_client *client, tektura_capability cap);

/* xdg-decoration */
void server_new_xdg_decoration(struct wl_listener *listener, void *data);

/* output-management */
void server_output_mgr_apply(struct wl_listener *listener, void *data);
void server_output_mgr_test(struct wl_listener *listener, void *data);

#endif /* TEKTURA_PROTOCOLS_H */
