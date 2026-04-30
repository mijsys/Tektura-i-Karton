/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * ipc.c - Implementacja bezpiecznej komunikacji IPC.
 *
 * Każdy klient po połączeniu musi:
 *   1. Wysłać "HELLO <token_hex>\n" w ciągu 2 sekund.
 *   2. Podpisać każdą kolejną wiadomość (opcionalnie — TODO faza 2).
 * Token sesji jest generowany przy starcie kompozytora z /dev/urandom
 * i przekazywany zaufanym procesom przez zmienną środowiskową.
 */

#include "ipc.h"
#include "server.h"
#include "i18n.h"
#include "view.h"
#include "workspace.h"
#include "output.h"
#include "wallpaper.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* Struktury wewnętrzne                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
	int      fd;
	bool     authenticated;       /* czy przeszedł HELLO?          */
	uint32_t subscriptions;       /* maska bitowa tektura_ipc_event */
	char     read_buf[IPC_MAX_MSG_SIZE];
	size_t   read_len;
	struct wl_event_source *event_source;
	struct tektura_ipc    *ipc;   /* wskaźnik w górę               */
} ipc_client;

struct tektura_ipc {
	struct tektura_server     *server;
	tektura_ipc_callbacks      callbacks;
	uint8_t                    session_token[IPC_SESSION_TOKEN_SIZE];
	char                       session_token_hex[IPC_SESSION_TOKEN_SIZE * 2 + 1];
	char                       sock_path[512];
	int                        sock_fd;
	struct wl_event_source    *sock_event;
	ipc_client                 clients[IPC_MAX_CLIENTS];
};

/* ------------------------------------------------------------------ */
/* Pomocnicze                                                           */
/* ------------------------------------------------------------------ */

static void bytes_to_hex(const uint8_t *in, size_t len, char *out) {
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < len; i++) {
		out[i * 2]     = hex[in[i] >> 4];
		out[i * 2 + 1] = hex[in[i] & 0xf];
	}
	out[len * 2] = '\0';
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t len) {
	for (size_t i = 0; i < len; i++) {
		unsigned int hi, lo;
		if (sscanf(hex + i * 2, "%1x", &hi) != 1) return false;
		if (sscanf(hex + i * 2 + 1, "%1x", &lo) != 1) return false;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

static ipc_client *find_free_client_slot(struct tektura_ipc *ipc) {
	for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
		if (ipc->clients[i].fd < 0) {
			return &ipc->clients[i];
		}
	}
	return NULL;
}

static void client_send(ipc_client *c, const char *msg) {
	if (c->fd < 0) return;
	size_t len = strlen(msg);
	if (write(c->fd, msg, len) < 0) {
		wlr_log(WLR_DEBUG, "ipc: write fd=%d błąd: %s", c->fd, strerror(errno));
	}
}

static void client_disconnect(ipc_client *c) {
	if (c->fd < 0) return;
	wlr_log(WLR_DEBUG, "ipc: rozłączono klienta fd=%d", c->fd);
	if (c->event_source) {
		wl_event_source_remove(c->event_source);
		c->event_source = NULL;
	}
	close(c->fd);
	c->fd = -1;
	c->authenticated = false;
	c->subscriptions = 0;
	c->read_len = 0;
}

static struct tektura_toplevel *get_toplevel_by_index(
		struct tektura_server *server, int idx) {
	if (idx < 0) return NULL;
	int i = 0;
	struct tektura_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (i == idx) return t;
		i++;
	}
	return NULL;
}

static void sanitize_field(char *s) {
	if (!s) return;
	for (char *p = s; *p; p++) {
		if (*p == '\n' || *p == '\r' || *p == ';' || *p == '|') {
			*p = ' ';
		}
	}
}

/* ------------------------------------------------------------------ */
/* Przetwarzanie wiadomości od klienta                                  */
/* ------------------------------------------------------------------ */

static void handle_action(ipc_client *c, const char *action_str, const char *args) {
	struct tektura_ipc *ipc = c->ipc;
	struct tektura_server *srv = ipc->server;

	if (strcmp(action_str, "FOCUS_WINDOW") == 0) {
		int id = -1;
		if (sscanf(args, "%d", &id) == 1) {
			struct tektura_toplevel *top = get_toplevel_by_index(srv, id);
			if (top) {
				focus_toplevel(top);
				client_send(c, "OK\n");
			} else {
				client_send(c, "ERR 404 window not found\n");
			}
		} else {
			client_send(c, "ERR 400 missing id\n");
		}

	} else if (strcmp(action_str, "CLOSE_WINDOW") == 0) {
		int id = -1;
		if (sscanf(args, "%d", &id) == 1) {
			struct tektura_toplevel *top = get_toplevel_by_index(srv, id);
			if (top) {
				wlr_xdg_toplevel_send_close(top->xdg_toplevel);
				client_send(c, "OK\n");
			} else {
				client_send(c, "ERR 404 window not found\n");
			}
		} else {
			client_send(c, "ERR 400 missing id\n");
		}

	} else if (strcmp(action_str, "PIN_WINDOW") == 0) {
		int id = -1, pin = 0;
		if (sscanf(args, "%d %d", &id, &pin) == 2) {
			struct tektura_toplevel *top = get_toplevel_by_index(srv, id);
			if (top && srv->workspace_manager) {
				workspace_set_pinned(srv->workspace_manager, top, pin != 0);
				client_send(c, "OK\n");
			} else {
				client_send(c, "ERR 404 window/workspace not found\n");
			}
		} else {
			client_send(c, "ERR 400 missing args\n");
		}

	} else if (strcmp(action_str, "SET_WALLPAPER") == 0) {
		char output[128] = {0};
		char path[512] = {0};
		if (sscanf(args, "%127s %511[^\n]", output, path) >= 2) {
			if (srv->wallpaper_manager && wallpaper_set(srv->wallpaper_manager, output, path)) {
				char payload[IPC_MAX_MSG_SIZE];
				snprintf(payload, sizeof(payload), "%s %s", output, path);
				ipc_broadcast_event(ipc, IPC_EVENT_WALLPAPER_CHANGED, payload);
				client_send(c, "OK\n");
			} else {
				client_send(c, "ERR 500 wallpaper set failed\n");
			}
		} else {
			client_send(c, "ERR 400 missing output/path\n");
		}

	} else if (strcmp(action_str, "PERMISSION_RESPONSE") == 0) {
		int app_pid, cap_id, state;
		if (sscanf(args, "%d %d %d", &app_pid, &cap_id, &state) == 3) {
			if (ipc->callbacks.on_permission_response) {
				ipc->callbacks.on_permission_response(srv,
					(pid_t)app_pid, cap_id, state);
			}
			client_send(c, "OK\n");
		} else {
			client_send(c, "ERR 400 bad args\n");
		}

	} else if (strcmp(action_str, "SET_LOCALE") == 0) {
		if (args && *args) {
			/* Usuń '\n' jeśli jest */
			char lang[32];
			snprintf(lang, sizeof(lang), "%s", args);
			lang[strcspn(lang, "\n\r")] = '\0';
			if (i18n_set_locale(lang)) {
				if (ipc->callbacks.on_set_locale) {
					ipc->callbacks.on_set_locale(srv, lang);
				}
				client_send(c, "OK\n");
			} else {
				client_send(c, "ERR 404 unknown locale\n");
			}
		} else {
			client_send(c, "ERR 400 missing lang\n");
		}

	} else if (strcmp(action_str, "LOCK_SCREEN") == 0) {
		if (ipc->callbacks.on_lock_screen) {
			ipc->callbacks.on_lock_screen(srv);
		}
		client_send(c, "OK\n");

	} else {
		client_send(c, "ERR 404 unknown action\n");
	}
}

static void handle_query(ipc_client *c, const char *query_str) {
	if (strcmp(query_str, "LOCALE") == 0) {
		char buf[64];
		snprintf(buf, sizeof(buf), "OK %s\n", i18n_current_locale());
		client_send(c, buf);
	} else if (strcmp(query_str, "WINDOWS") == 0) {
		char payload[IPC_MAX_MSG_SIZE] = {0};
		size_t off = 0;
		struct tektura_toplevel *t;
		int id = 0;
		wl_list_for_each(t, &c->ipc->server->toplevels, link) {
			char app[128] = {0};
			char title[128] = {0};
			snprintf(app, sizeof(app), "%s",
				t->xdg_toplevel->app_id ? t->xdg_toplevel->app_id : "");
			snprintf(title, sizeof(title), "%s",
				t->xdg_toplevel->title ? t->xdg_toplevel->title : "");
			sanitize_field(app);
			sanitize_field(title);
			int n = snprintf(payload + off, sizeof(payload) - off,
				"%s{id:%d,app:%s,title:%s,ws:%u,pin:%d}",
				off ? ";" : "", id, app, title,
				(unsigned)t->workspace_id, t->pinned ? 1 : 0);
			if (n < 0 || (size_t)n >= sizeof(payload) - off) break;
			off += (size_t)n;
			id++;
		}
		char out[IPC_MAX_MSG_SIZE];
		snprintf(out, sizeof(out), "OK %s\n", off ? payload : "[]");
		client_send(c, out);
	} else if (strcmp(query_str, "WORKSPACES") == 0) {
		if (!c->ipc->server->workspace_manager) {
			client_send(c, "OK []\n");
			return;
		}
		uint32_t count = workspace_get_count(c->ipc->server->workspace_manager);
		uint32_t active = workspace_get_active_id(c->ipc->server->workspace_manager);
		char payload[IPC_MAX_MSG_SIZE];
		size_t off = (size_t)snprintf(payload, sizeof(payload),
			"active:%u;count:%u", active, count);
		for (uint32_t i = 0; i < count; i++) {
			tektura_workspace *ws = workspace_get(c->ipc->server->workspace_manager, i);
			if (!ws) continue;
			int n = snprintf(payload + off, sizeof(payload) - off,
				";%u:%s:%d", ws->id, ws->name, ws->visible ? 1 : 0);
			if (n < 0 || (size_t)n >= sizeof(payload) - off) break;
			off += (size_t)n;
		}
		char out[IPC_MAX_MSG_SIZE];
		snprintf(out, sizeof(out), "OK %s\n", payload);
		client_send(c, out);
	} else if (strcmp(query_str, "OUTPUTS") == 0) {
		char payload[IPC_MAX_MSG_SIZE] = {0};
		size_t off = 0;
		struct tektura_output *o;
		wl_list_for_each(o, &c->ipc->server->outputs, link) {
			int n = snprintf(payload + off, sizeof(payload) - off,
				"%s{name:%s,enabled:%d}",
				off ? ";" : "", o->wlr_output->name,
				o->wlr_output->enabled ? 1 : 0);
			if (n < 0 || (size_t)n >= sizeof(payload) - off) break;
			off += (size_t)n;
		}
		char out[IPC_MAX_MSG_SIZE];
		snprintf(out, sizeof(out), "OK %s\n", off ? payload : "[]");
		client_send(c, out);
	} else if (strcmp(query_str, "WALLPAPERS") == 0) {
		char payload[IPC_MAX_MSG_SIZE] = {0};
		if (c->ipc->server->wallpaper_manager) {
			wallpaper_serialize(c->ipc->server->wallpaper_manager, payload, sizeof(payload));
		}
		char out[IPC_MAX_MSG_SIZE];
		snprintf(out, sizeof(out), "OK %s\n", payload[0] ? payload : "[]");
		client_send(c, out);
	} else {
		client_send(c, "ERR 404 unknown query\n");
	}
}

static void process_message(ipc_client *c, char *line) {
	/* Usuń '\r' jeśli jest (połączenia z Windows-style CRLF) */
	char *cr = strchr(line, '\r');
	if (cr) *cr = '\0';

	if (!c->authenticated) {
		/* Jedyna dozwolona wiadomość przed autoryzacją: HELLO <token_hex> */
		char token_hex[IPC_SESSION_TOKEN_SIZE * 2 + 1];
		if (sscanf(line, "HELLO %128s", token_hex) == 1) {
			uint8_t received[IPC_SESSION_TOKEN_SIZE];
			if (hex_to_bytes(token_hex, received, IPC_SESSION_TOKEN_SIZE) &&
			    memcmp(received, c->ipc->session_token, IPC_SESSION_TOKEN_SIZE) == 0) {
				c->authenticated = true;
				client_send(c, "OK authenticated\n");
				wlr_log(WLR_DEBUG, "ipc: klient fd=%d uwierzytelniony", c->fd);
			} else {
				wlr_log(WLR_ERROR, "ipc: nieprawidłowy token od fd=%d — rozłączam", c->fd);
				client_send(c, "ERR 401 bad token\n");
				client_disconnect(c);
			}
		} else {
			client_send(c, "ERR 401 unauthenticated\n");
			client_disconnect(c);
		}
		return;
	}

	/* Parsuj wiadomości po autoryzacji */
	char verb[32], rest[IPC_MAX_MSG_SIZE];
	rest[0] = '\0';
	sscanf(line, "%31s %4095[^\n]", verb, rest);

	if (strcmp(verb, "SUBSCRIBE") == 0) {
		int ev = atoi(rest);
		c->subscriptions |= (1u << ev);
		client_send(c, "OK\n");

	} else if (strcmp(verb, "UNSUBSCRIBE") == 0) {
		int ev = atoi(rest);
		c->subscriptions &= ~(1u << ev);
		client_send(c, "OK\n");

	} else if (strcmp(verb, "QUERY") == 0) {
		handle_query(c, rest);

	} else if (strcmp(verb, "ACTION") == 0) {
		char action_str[32], action_args[IPC_MAX_MSG_SIZE];
		action_args[0] = '\0';
		sscanf(rest, "%31s %4063[^\n]", action_str, action_args);
		handle_action(c, action_str, action_args);

	} else {
		client_send(c, "ERR 400 unknown verb\n");
	}
}

/* ------------------------------------------------------------------ */
/* Callback pętli zdarzeń — dane od klienta                           */
/* ------------------------------------------------------------------ */

static int on_client_readable(int fd, uint32_t mask, void *data) {
	ipc_client *c = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		client_disconnect(c);
		return 0;
	}

	ssize_t n = read(fd,
		c->read_buf + c->read_len,
		sizeof(c->read_buf) - c->read_len - 1);

	if (n <= 0) {
		client_disconnect(c);
		return 0;
	}
	c->read_len += n;
	c->read_buf[c->read_len] = '\0';

	/* Przetwarzaj pełne linie */
	char *start = c->read_buf;
	char *newline;
	while ((newline = memchr(start, '\n', c->read_len - (size_t)(start - c->read_buf)))) {
		*newline = '\0';
		process_message(c, start);
		start = newline + 1;
	}

	/* Przesuń niepełną linię na początek bufora */
	size_t remaining = c->read_len - (size_t)(start - c->read_buf);
	if (remaining > 0 && start != c->read_buf) {
		memmove(c->read_buf, start, remaining);
	}
	c->read_len = remaining;

	/* Jeśli bufor pełny bez znaku '\n' — prawdopodobnie atak */
	if (c->read_len >= sizeof(c->read_buf) - 1) {
		wlr_log(WLR_ERROR, "ipc: przepełnienie bufora od fd=%d — rozłączam", fd);
		client_send(c, "ERR 413 message too large\n");
		client_disconnect(c);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Callback pętli zdarzeń — nowe połączenie                           */
/* ------------------------------------------------------------------ */

static int on_new_connection(int fd, uint32_t mask, void *data) {
	struct tektura_ipc *ipc = data;
	(void)mask;

	int client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0) {
		return 0;
	}

	/* Natychmiastowa weryfikacja UID przez SO_PEERCRED */
	struct ucred cred;
	socklen_t len = sizeof(cred);
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
	    cred.uid != getuid()) {
		wlr_log(WLR_ERROR, "ipc: połączenie od obcego UID %u — odrzucam", cred.uid);
		close(client_fd);
		return 0;
	}

	ipc_client *c = find_free_client_slot(ipc);
	if (!c) {
		wlr_log(WLR_ERROR, "ipc: osiągnięto limit %d klientów", IPC_MAX_CLIENTS);
		close(client_fd);
		return 0;
	}

	/* Ustaw non-blocking */
	fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);

	c->fd             = client_fd;
	c->authenticated  = false;
	c->subscriptions  = 0;
	c->read_len       = 0;
	c->ipc            = ipc;

	struct wl_event_loop *loop = wl_display_get_event_loop(ipc->server->wl_display);
	c->event_source = wl_event_loop_add_fd(loop, client_fd,
		WL_EVENT_READABLE, on_client_readable, c);

	wlr_log(WLR_DEBUG, "ipc: nowy klient PID=%d fd=%d", cred.pid, client_fd);

	/* Wyślij wiadomość powitalną — klient musi odpowiedzieć HELLO w 2s */
	client_send(c, "TEKTURA_IPC_HELLO_REQUIRED\n");

	return 0;
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

tektura_ipc *ipc_init(struct tektura_server *server,
		const tektura_ipc_callbacks *callbacks) {
	struct tektura_ipc *ipc = calloc(1, sizeof(*ipc));
	if (!ipc) return NULL;

	ipc->server = server;
	if (callbacks) {
		ipc->callbacks = *callbacks;
	}

	/* Inicjalizuj sloty klientów */
	for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
		ipc->clients[i].fd = -1;
	}

	/* Generuj token sesji */
	int urandom = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (urandom < 0 ||
	    read(urandom, ipc->session_token, IPC_SESSION_TOKEN_SIZE)
	        != IPC_SESSION_TOKEN_SIZE) {
		wlr_log(WLR_ERROR, "ipc: błąd generowania tokenu sesji");
		if (urandom >= 0) close(urandom);
		free(ipc);
		return NULL;
	}
	close(urandom);
	bytes_to_hex(ipc->session_token, IPC_SESSION_TOKEN_SIZE,
		ipc->session_token_hex);
	wlr_log(WLR_DEBUG, "ipc: token sesji wygenerowany");

	/* Ścieżka socketu */
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) runtime_dir = "/tmp";
	snprintf(ipc->sock_path, sizeof(ipc->sock_path),
		"%s/" IPC_SOCKET_SUBPATH, runtime_dir);

	/* Utwórz katalog rodzica */
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", ipc->sock_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0700);
	}

	unlink(ipc->sock_path);

	ipc->sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (ipc->sock_fd < 0) {
		wlr_log(WLR_ERROR, "ipc: błąd socket: %s", strerror(errno));
		free(ipc);
		return NULL;
	}

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, ipc->sock_path, sizeof(addr.sun_path) - 1);
	if (bind(ipc->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		wlr_log(WLR_ERROR, "ipc: bind %s: %s", ipc->sock_path, strerror(errno));
		close(ipc->sock_fd);
		free(ipc);
		return NULL;
	}

	chmod(ipc->sock_path, 0600);
	listen(ipc->sock_fd, IPC_MAX_CLIENTS);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	ipc->sock_event = wl_event_loop_add_fd(loop, ipc->sock_fd,
		WL_EVENT_READABLE, on_new_connection, ipc);

	wlr_log(WLR_INFO, "ipc: nasłuchuje na %s", ipc->sock_path);
	return ipc;
}

const uint8_t *ipc_get_session_token(tektura_ipc *ipc) {
	return ipc->session_token;
}

void ipc_broadcast_event(tektura_ipc *ipc,
		tektura_ipc_event event, const char *payload) {
	char msg[IPC_MAX_MSG_SIZE];
	snprintf(msg, sizeof(msg), "EVENT %d %s\n",
		(int)event, payload ? payload : "");

	for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
		ipc_client *c = &ipc->clients[i];
		if (c->fd >= 0 && c->authenticated &&
		    (c->subscriptions & (1u << (int)event))) {
			client_send(c, msg);
		}
	}
}

void ipc_send_event(tektura_ipc *ipc, int client_fd,
		tektura_ipc_event event, const char *payload) {
	if (client_fd < 0) {
		ipc_broadcast_event(ipc, event, payload);
		return;
	}
	char msg[IPC_MAX_MSG_SIZE];
	snprintf(msg, sizeof(msg), "EVENT %d %s\n",
		(int)event, payload ? payload : "");
	for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
		if (ipc->clients[i].fd == client_fd) {
			client_send(&ipc->clients[i], msg);
			return;
		}
	}
}

void ipc_destroy(tektura_ipc *ipc) {
	if (!ipc) return;
	for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
		client_disconnect(&ipc->clients[i]);
	}
	if (ipc->sock_event) {
		wl_event_source_remove(ipc->sock_event);
	}
	if (ipc->sock_fd >= 0) {
		close(ipc->sock_fd);
		unlink(ipc->sock_path);
	}
	free(ipc);
}
