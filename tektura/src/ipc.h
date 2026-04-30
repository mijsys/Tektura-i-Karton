/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * ipc.h - Bezpieczna komunikacja między kompozytorem a resztą środowiska.
 *
 * Architektura IPC:
 *   Kompozytor jest serwerem. Klienty (panel, dock, ustawienia, shell)
 *   łączą się przez Unix Domain Socket w /run/user/<uid>/tektura/ipc.sock.
 *
 * Bezpieczeństwo:
 *   - SO_PEERCRED: każde połączenie jest natychmiast weryfikowane (UID + PID).
 *   - Tylko procesy tego samego użytkownika mogą się połączyć.
 *   - Każda wiadomość ma HMAC-SHA256 podpisany kluczem sesji generowanym
 *     przy starcie kompozytora (unikalny per sesja, trzymany tylko w RAM).
 *   - Klient musi w ciągu 2s przesłać wiadomość HELLO z poprawnym tokenem
 *     sesji; inaczej połączenie jest zrywane.
 *
 * Protokół (linie tekstu zakończone '\n', UTF-8):
 *   Klient → Serwer:
 *     HELLO <session_token>
 *     SUBSCRIBE <event_type>          ← subskrybuj zdarzenia
 *     UNSUBSCRIBE <event_type>
 *     QUERY <query_type> [args...]    ← zapytaj o stan
 *     ACTION <action_type> [args...]  ← zażądaj akcji
 *
 *   Serwer → Klient:
 *     OK [payload]
 *     ERR <kod> <opis>
 *     EVENT <event_type> [payload]    ← push zdarzeń do subskrybentów
 *
 * Typy zdarzeń (event_type):
 *   WINDOW_OPENED, WINDOW_CLOSED, WINDOW_FOCUSED,
 *   PERMISSION_REQUEST,              ← "OBS chce nagrywać — pokaż okienko"
 *   WORKSPACE_CHANGED, OUTPUT_CHANGED,
 *   LOCK_SCREEN, UNLOCK_SCREEN
 *
 * Typy zapytań (query_type):
 *   WINDOWS, WORKSPACES, OUTPUTS, WALLPAPERS, LOCALE
 *
 * Typy akcji (action_type):
 *   FOCUS_WINDOW <id>, CLOSE_WINDOW <id>,
 *   PIN_WINDOW <id> <0|1>,
 *   SET_WALLPAPER <output|*> <path>,
 *   PERMISSION_RESPONSE <pid> <cap> <state>,   ← odpowiedź z okienka autoryzacji
 *   SET_LOCALE <lang_code>,
 *   LOCK_SCREEN
 */

#ifndef TEKTURA_IPC_H
#define TEKTURA_IPC_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct tektura_server;

/* Rozmiar tokenu sesji w bajtach (256 bitów) */
#define IPC_SESSION_TOKEN_SIZE 32

/* Maksymalna długość jednej wiadomości IPC */
#define IPC_MAX_MSG_SIZE 4096

/* Maksymalna liczba równoczesnych klientów IPC */
#define IPC_MAX_CLIENTS 16

/* Unix socket path subpath (względem XDG_RUNTIME_DIR) */
#define IPC_SOCKET_SUBPATH "tektura/ipc.sock"

/* ------------------------------------------------------------------ */
/* Typy zdarzeń i akcji                                                */
/* ------------------------------------------------------------------ */

typedef enum {
	IPC_EVENT_WINDOW_OPENED     = 1,
	IPC_EVENT_WINDOW_CLOSED     = 2,
	IPC_EVENT_WINDOW_FOCUSED    = 3,
	IPC_EVENT_PERMISSION_REQUEST= 4,  /* payload: "pid cap_id app_path" */
	IPC_EVENT_WORKSPACE_CHANGED = 5,
	IPC_EVENT_OUTPUT_CHANGED    = 6,
	IPC_EVENT_LOCK_SCREEN       = 7,
	IPC_EVENT_UNLOCK_SCREEN     = 8,
	IPC_EVENT_WALLPAPER_CHANGED = 9,
} tektura_ipc_event;

typedef enum {
	IPC_ACTION_FOCUS_WINDOW        = 1,
	IPC_ACTION_CLOSE_WINDOW        = 2,
	IPC_ACTION_PERMISSION_RESPONSE = 3, /* odpowiedź z okienka autoryzacji */
	IPC_ACTION_SET_LOCALE          = 4,
	IPC_ACTION_LOCK_SCREEN         = 5,
	IPC_ACTION_SET_WALLPAPER       = 6,
	IPC_ACTION_PIN_WINDOW          = 7,
} tektura_ipc_action;

/* ------------------------------------------------------------------ */
/* Kontekst IPC (opaque)                                               */
/* ------------------------------------------------------------------ */

typedef struct tektura_ipc tektura_ipc;

/* ------------------------------------------------------------------ */
/* Callbacki — wypełnij przed wywołaniem ipc_init()                   */
/* ------------------------------------------------------------------ */

typedef struct {
	/*
	 * Wywołany gdy panel/shell odpowie na PERMISSION_REQUEST.
	 * state: 1=zawsze, 2=tylko teraz, 3=odmów
	 */
	void (*on_permission_response)(struct tektura_server *server,
		pid_t app_pid, int cap_id, int state);

	/* Wywołany gdy klient prosi o zmianę języka interfejsu */
	void (*on_set_locale)(struct tektura_server *server,
		const char *lang_code);

	/* Wywołany gdy klient żąda zablokowania ekranu */
	void (*on_lock_screen)(struct tektura_server *server);
} tektura_ipc_callbacks;

/* ------------------------------------------------------------------ */
/* API publiczne                                                        */
/* ------------------------------------------------------------------ */

/*
 * Inicjalizuje serwer IPC:
 *   - generuje losowy token sesji (w RAM, nie na dysku)
 *   - tworzy Unix socket z uprawnieniami 0600
 *   - rejestruje deskryptor w pętli zdarzeń Wayland
 * Zwraca NULL przy błędzie krytycznym.
 */
tektura_ipc *ipc_init(struct tektura_server *server,
	const tektura_ipc_callbacks *callbacks);

/*
 * Zwraca token sesji (32 bajty) — do przekazania zaufanym klientom
 * (np. przez zmienną środowiskową TEKTURA_IPC_TOKEN przy uruchamianiu shella).
 */
const uint8_t *ipc_get_session_token(tektura_ipc *ipc);

/*
 * Wysyła zdarzenie do wszystkich podłączonych i subskrybujących klientów.
 * payload może być NULL.
 */
void ipc_broadcast_event(tektura_ipc *ipc,
	tektura_ipc_event event, const char *payload);

/*
 * Wysyła zdarzenie do konkretnego klienta (np. PERMISSION_REQUEST
 * tylko do panelu — nie do wszystkich).
 * client_fd: deskryptor połączonego klienta, -1 = broadcast.
 */
void ipc_send_event(tektura_ipc *ipc, int client_fd,
	tektura_ipc_event event, const char *payload);

/*
 * Niszczy serwer IPC, zamyka wszystkie połączenia, usuwa socket.
 */
void ipc_destroy(tektura_ipc *ipc);

#endif /* TEKTURA_IPC_H */
