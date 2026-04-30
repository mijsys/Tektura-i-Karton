/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * protocols.c - Inicjalizacja i obsługa protokołów Wayland.
 */

#include "protocols.h"
#include "server.h"
#include "view.h"
#include "ipc.h"

#include <stdbool.h>
#include <sys/socket.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/log.h>

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
struct tektura_popup *popup = wl_container_of(listener, popup, commit);
if (popup->xdg_popup->base->initial_commit) {
wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
(void)data;
struct tektura_popup *popup = wl_container_of(listener, popup, destroy);
wl_list_remove(&popup->commit.link);
wl_list_remove(&popup->destroy.link);
free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
(void)listener;
struct wlr_xdg_popup *xdg_popup = data;

struct tektura_popup *popup = calloc(1, sizeof(*popup));
if (!popup) return;
popup->xdg_popup = xdg_popup;

struct wlr_xdg_surface *parent =
wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
assert(parent != NULL);
struct wlr_scene_tree *parent_tree = parent->data;
xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

popup->commit.notify = xdg_popup_commit;
wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

popup->destroy.notify = xdg_popup_destroy;
wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

bool protocols_check_privileged(struct tektura_server *server,
struct wl_client *client, tektura_capability cap) {
if (!server->security_manager) {
return false;
}

pid_t pid;
uid_t uid;
gid_t gid;
wl_client_get_credentials(client, &pid, &uid, &gid);

tektura_perm_result result = secmgr_check(server->security_manager, pid, cap);

if (result.state == PERM_ALLOWED || result.state == PERM_ONCE) {
return true;
}
if (result.state == PERM_DENIED) {
return false;
}

/* Powiadom shell/panel o konieczności promptu — tylko jeśli jeszcze nie czekamy */
if (server->ipc && result.state == PERM_UNSET) {
char app_path[SECMGR_MAX_PATH] = {0};
if (!secmgr_pid_to_path(pid, app_path, sizeof(app_path))) {
snprintf(app_path, sizeof(app_path), "pid:%d", (int)pid);
}
if (!secmgr_is_pending(server->security_manager, app_path, cap)) {
/* Pierwsze żądanie — oznacz jako oczekujące i powiadom shella */
secmgr_mark_pending(server->security_manager, app_path, cap);
char payload[IPC_MAX_MSG_SIZE];
snprintf(payload, sizeof(payload), "%d %d %s", (int)pid, (int)cap, app_path);
ipc_broadcast_event(server->ipc, IPC_EVENT_PERMISSION_REQUEST, payload);
wlr_log(WLR_INFO,
"protocols: wysłano żądanie zgody dla PID=%d cap='%s', czekam na odpowiedź shella.",
(int)pid, secmgr_capability_name(cap));
} else {
wlr_log(WLR_DEBUG,
"protocols: PID=%d cap='%s' — prompt już wysłany, blokuję cicho.",
(int)pid, secmgr_capability_name(cap));
}
}

wlr_log(WLR_INFO,
"protocols: aplikacja PID=%d chce dostępu do '%s' — brakuje zgody, blokuję.",
(int)pid, secmgr_capability_name(cap));
return false;
}

/* ------------------------------------------------------------------ */
/* xdg-decoration                                                      */
/* ------------------------------------------------------------------ */

static bool decoration_policy_prefers_csd(struct tektura_server *server, const char *app_id) {
if (!server || !server->config || !app_id) return false;

bool prefer_csd = false;
const char *mode = server->config->decorations.default_mode;
if (mode && strcmp(mode, "csd") == 0) prefer_csd = true;

char list[256];
snprintf(list, sizeof(list), "%s", server->config->decorations.force_csd_apps);
for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
while (*tok == ' ') tok++;
if (*tok && strcmp(tok, app_id) == 0) prefer_csd = true;
}

snprintf(list, sizeof(list), "%s", server->config->decorations.force_ssd_apps);
for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
while (*tok == ' ') tok++;
if (*tok && strcmp(tok, app_id) == 0) prefer_csd = false;
}

return prefer_csd;
}

static void xdg_decoration_request_mode(struct wl_listener *listener,
void *data) {
(void)listener;
struct wlr_xdg_toplevel_decoration_v1 *deco = data;
bool prefer_csd = (bool)(intptr_t)deco->data;
wlr_xdg_toplevel_decoration_v1_set_mode(deco,
prefer_csd
? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void xdg_decoration_destroy(struct wl_listener *listener,
void *data) {
(void)data;
wl_list_remove(&listener->link);
free(listener);
}

void server_new_xdg_decoration(struct wl_listener *listener, void *data) {
struct tektura_server *server = wl_container_of(listener, server, new_xdg_decoration);
struct wlr_xdg_toplevel_decoration_v1 *deco = data;
const char *app_id = deco->toplevel && deco->toplevel->app_id
? deco->toplevel->app_id : "";

bool prefer_csd = decoration_policy_prefers_csd(server, app_id);
deco->data = (void *)(intptr_t)(prefer_csd ? 1 : 0);

wlr_xdg_toplevel_decoration_v1_set_mode(deco,
prefer_csd
? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
: WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

struct wl_listener *req = calloc(1, sizeof(*req));
if (req) {
req->notify = xdg_decoration_request_mode;
wl_signal_add(&deco->events.request_mode, req);
}

struct wl_listener *des = calloc(1, sizeof(*des));
if (des) {
des->notify = xdg_decoration_destroy;
wl_signal_add(&deco->events.destroy, des);
}
}

/* ------------------------------------------------------------------ */
/* wlr-output-management                                               */
/* ------------------------------------------------------------------ */

static void apply_output_config(struct tektura_server *server,
struct wlr_output_configuration_v1 *config, bool test_only) {
bool ok = true;

struct wlr_output_configuration_head_v1 *head;
wl_list_for_each(head, &config->heads, link) {
struct wlr_output *output = head->state.output;
struct wlr_output_state state;
wlr_output_state_init(&state);

wlr_output_state_set_enabled(&state, head->state.enabled);
if (head->state.enabled) {
if (head->state.mode)
wlr_output_state_set_mode(&state, head->state.mode);
wlr_output_state_set_scale(&state, head->state.scale);
wlr_output_state_set_transform(&state, head->state.transform);
}

if (test_only) {
if (!wlr_output_test_state(output, &state)) ok = false;
} else {
if (!wlr_output_commit_state(output, &state)) ok = false;
}
wlr_output_state_finish(&state);
}

if (ok)
wlr_output_configuration_v1_send_succeeded(config);
else
wlr_output_configuration_v1_send_failed(config);
wlr_output_configuration_v1_destroy(config);

if (!test_only) {
struct wlr_output_configuration_v1 *new_config =
wlr_output_configuration_v1_create();
struct wlr_output_layout_output *lo;
wl_list_for_each(lo, &server->output_layout->outputs, link) {
struct wlr_output_configuration_head_v1 *h =
wlr_output_configuration_head_v1_create(new_config, lo->output);
(void)h;
}
wlr_output_manager_v1_set_configuration(server->output_mgr, new_config);
}
}

void server_output_mgr_apply(struct wl_listener *listener, void *data) {
struct tektura_server *server =
wl_container_of(listener, server, output_mgr_apply);
apply_output_config(server, data, false);
}

void server_output_mgr_test(struct wl_listener *listener, void *data) {
struct tektura_server *server =
wl_container_of(listener, server, output_mgr_test);
apply_output_config(server, data, true);
}

void protocols_init(struct tektura_server *server) {
wl_list_init(&server->toplevels);
server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);

server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);

server->new_xdg_popup.notify = server_new_xdg_popup;
wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

server->xdg_decoration_mgr =
wlr_xdg_decoration_manager_v1_create(server->wl_display);
server->new_xdg_decoration.notify = server_new_xdg_decoration;
wl_signal_add(&server->xdg_decoration_mgr->events.new_toplevel_decoration,
&server->new_xdg_decoration);

server->output_mgr = wlr_output_manager_v1_create(server->wl_display);
server->output_mgr_apply.notify = server_output_mgr_apply;
wl_signal_add(&server->output_mgr->events.apply, &server->output_mgr_apply);
server->output_mgr_test.notify = server_output_mgr_test;
wl_signal_add(&server->output_mgr->events.test, &server->output_mgr_test);
}
