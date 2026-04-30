/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * server.c - Inicjalizacja i główna pętla serwera kompozytora.
 */

#include "server.h"
#include "output.h"
#include "input.h"
#include "view.h"
#include "protocols.h"
#include "layer.h"
#include "i18n.h"
#include "ipc.h"
#include "lock.h"
#include "workspace.h"
#include "config.h"
#include "idle.h"
#include "decoration.h"
#include "xwayland.h"
#include "wallpaper.h"

#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

/* --- Callbacki IPC --- */

static void on_permission_response(struct tektura_server *server,
		pid_t app_pid, int cap_id, int state) {
	if (!server->security_manager) return;
	char app_path[512];
	if (!secmgr_pid_to_path(app_pid, app_path, sizeof(app_path))) return;
	secmgr_set_permission(server->security_manager, app_path,
		(tektura_capability)cap_id,
		(tektura_permission_state)state);
	/* Usuń z tabeli oczekujących — shell odpowiedział */
	secmgr_clear_pending(server->security_manager, app_path,
		(tektura_capability)cap_id);
	wlr_log(WLR_INFO, "server: IPC permission_response pid=%d cap=%d state=%d",
		(int)app_pid, cap_id, state);
}

static void on_set_locale(struct tektura_server *server, const char *lang_code) {
	(void)server;
	i18n_set_locale(lang_code);
	wlr_log(WLR_INFO, "server: IPC set_locale=%s", lang_code);
}

static void on_lock_screen(struct tektura_server *server) {
	wlr_log(WLR_INFO, "server: IPC lock_screen");
	if (server->lock_manager) {
		lock_request(server->lock_manager);
	} else if (server->ipc) {
		ipc_broadcast_event(server->ipc, IPC_EVENT_LOCK_SCREEN, NULL);
	}
}

void server_init(struct tektura_server *server) {
	server->wl_display = wl_display_create();

	/* Konfiguracja — wczytaj najwcześniej (reszta korzysta z wartości) */
	server->config = config_load();
	if (!server->config) {
		wlr_log(WLR_ERROR, "nie udało się wczytać konfiguracji");
		exit(1);
	}

	server->backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server->wl_display), NULL);
	if (server->backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		exit(1);
	}

	server->renderer = wlr_renderer_autocreate(server->backend);
	if (server->renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		exit(1);
	}
	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (server->allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		exit(1);
	}

	server->compositor = wlr_compositor_create(server->wl_display, 5, server->renderer);
	wlr_subcompositor_create(server->wl_display);
	wlr_data_device_manager_create(server->wl_display);

	server->output_layout = wlr_output_layout_create(server->wl_display);

	wl_list_init(&server->outputs);
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	server->scene = wlr_scene_create();
	server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);

	protocols_init(server);

	/* layer-shell musi być po protocols_init (potrzebuje sceny) */
	layer_shell_init(server);

	/* SSB dekoracje okien */
	decoration_manager_init(server);

	/* Tapety (stan per-output, render robi shell background) */
	server->wallpaper_manager = wallpaper_manager_init(server);
	if (!server->wallpaper_manager) {
		wlr_log(WLR_ERROR, "wallpaper manager nie startuje — przerywam");
		exit(1);
	}

	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server->cursor_mode = TEKTURA_CURSOR_PASSTHROUGH;
	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	/* System wielojęzyczności — inicjalizuj przed UI */
	i18n_init();
	if (server->config->general.locale[0])
		i18n_set_locale(server->config->general.locale);

	/* Uruchom security manager przed resztą — musi być gotowy przed bindami */
	server->security_manager = secmgr_init(server);
	if (!server->security_manager) {
		wlr_log(WLR_ERROR, "security manager nie startuje — przerywam");
		exit(1);
	}

	/* Uruchom serwer IPC dla shella, panelu, ustawień */
	const tektura_ipc_callbacks ipc_cb = {
		.on_permission_response = on_permission_response,
		.on_set_locale          = on_set_locale,
		.on_lock_screen         = on_lock_screen,
	};
	server->ipc = ipc_init(server, &ipc_cb);
	if (!server->ipc) {
		wlr_log(WLR_ERROR, "IPC nie startuje — przerywam");
		exit(1);
	}
	/* Przekaż token sesji przez zmienną środowiskową — zaufane dzieci procesu
	 * (np. shell, panel) mogą się uwierzytelnić */
	const uint8_t *token = ipc_get_session_token(server->ipc);
	char token_hex[IPC_SESSION_TOKEN_SIZE * 2 + 1];
	for (int i = 0; i < IPC_SESSION_TOKEN_SIZE; i++) {
		snprintf(token_hex + i * 2, 3, "%02x", token[i]);
	}
	setenv("TEKTURA_IPC_TOKEN", token_hex, 1);
	setenv("TEKTURA_IPC_SOCKET", getenv("XDG_RUNTIME_DIR") ?
		"" : "/tmp", 0); /* placeholder — ipc.c ustawia pełną ścieżkę */

	/* Blokada ekranu */
	server->lock_manager = lock_manager_init(server);
	if (!server->lock_manager) {
		wlr_log(WLR_ERROR, "lock manager nie startuje — przerywam");
		exit(1);
	}

	/* Wirtualne pulpity */
	uint32_t ws_count = server->config->general.workspaces;
	if (ws_count < 1) ws_count = 1;
	if (ws_count > WORKSPACE_MAX) ws_count = WORKSPACE_MAX;
	server->workspace_manager = workspace_manager_init(server, ws_count);
	if (!server->workspace_manager) {
		wlr_log(WLR_ERROR, "workspace manager nie startuje — przerywam");
		exit(1);
	}

	wl_list_init(&server->keyboards);
	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->seat = wlr_seat_create(server->wl_display, "seat0");
	server->request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	server->pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server->seat->pointer_state.events.focus_change, &server->pointer_focus_change);
	server->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);

	/* Xwayland (X11 apps) */
	if (!xwayland_init(server)) {
		wlr_log(WLR_ERROR, "xwayland init failed");
	}
}

void server_run(struct tektura_server *server, const char *startup_cmd) {
	const char *socket = wl_display_add_socket_auto(server->wl_display);
	if (!socket) {
		wlr_backend_destroy(server->backend);
		exit(1);
	}

	if (!wlr_backend_start(server->backend)) {
		wlr_backend_destroy(server->backend);
		wl_display_destroy(server->wl_display);
		exit(1);
	}

	/* Idle manager — startuje po backendzie (potrzebuje seat) */
	const tektura_idle_thresholds idle_t = {
		.timeout_lock    = server->config->idle.timeout_lock,
		.timeout_dpms    = server->config->idle.timeout_dpms,
		.timeout_suspend = server->config->idle.timeout_suspend,
	};
	server->idle_manager = idle_manager_init(server, &idle_t);
	if (!server->idle_manager) {
		wlr_log(WLR_ERROR, "idle manager nie startuje — przerywam");
		exit(1);
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd == NULL && server->config->general.startup_cmd[0])
		startup_cmd = server->config->general.startup_cmd;
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}

	wlr_log(WLR_INFO, "Running Tektura on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server->wl_display);
}

void server_destroy(struct tektura_server *server) {
	wl_display_destroy_clients(server->wl_display);

	wl_list_remove(&server->new_xdg_toplevel.link);
	wl_list_remove(&server->new_xdg_popup.link);

	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);

	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->pointer_focus_change.link);
	wl_list_remove(&server->request_set_selection.link);

	wl_list_remove(&server->new_output.link);

	idle_manager_destroy(server->idle_manager);
	workspace_manager_destroy(server->workspace_manager);
	lock_manager_destroy(server->lock_manager);
	wallpaper_manager_destroy(server->wallpaper_manager);
	config_destroy(server->config);
	xwayland_destroy(server);

	ipc_destroy(server->ipc);
	secmgr_destroy(server->security_manager);
	i18n_destroy();

	wlr_scene_node_destroy(&server->scene->tree.node);
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
	wlr_allocator_destroy(server->allocator);
	wlr_renderer_destroy(server->renderer);
	wlr_backend_destroy(server->backend);
	wl_display_destroy(server->wl_display);
}
