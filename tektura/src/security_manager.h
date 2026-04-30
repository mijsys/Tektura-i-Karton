/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * security_manager.h - System uprawnień aplikacji.
 *
 * Architektura bezpieczeństwa:
 *   - Baza uprawnień w formacie binarnym (nie JSON/XML — trudniejsza do ręcznej edycji).
 *   - Podpis HMAC-SHA256 całego pliku — jeśli ktoś zmodyfikuje bazę z zewnątrz,
 *     kompozytor wykryje naruszenie i zresetuje bazę do bezpiecznych ustawień.
 *   - Blokada wyłączna flock() przez cały czas działania kompozytora.
 *   - Tylko kompozytor komunikuje się z bazą przez API tego modułu.
 *   - Zewnętrzne aplikacje (np. panel ustawień) mogą prosić o zmianę uprawnień
 *     wyłącznie przez dedykowany Unix socket — kompozytor weryfikuje PID nadawcy.
 *
 * Chronione protokoły Wayland:
 *   - wlr-screencopy-v1        (zrzuty ekranu)
 *   - wlr-export-dmabuf-v1    (nagrywanie wideo, OBS)
 *   - wlr-virtual-keyboard-v1 (emulacja klawiatury)
 *   - wlr-input-inhibitor-v1  (przechwytywanie całego wejścia)
 */

#ifndef TEKTURA_SECURITY_MANAGER_H
#define TEKTURA_SECURITY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

/* Maksymalna długość ścieżki do pliku wykonywalnego aplikacji */
#define SECMGR_MAX_PATH 512

/* Wersja formatu bazy binarnej — zmień, gdy zmienisz strukturę rekordu */
#define SECMGR_DB_VERSION 1

/* Plik bazy uprawnień — w /run/user/<uid>/ (tmpfs, niedostępny dla innych userów) */
#define SECMGR_DB_SUBPATH "tektura/permissions.db"

/* Unix socket dla zewnętrznych żądań zmiany uprawnień (np. panel ustawień) */
#define SECMGR_SOCKET_SUBPATH "tektura/security.sock"

/* ------------------------------------------------------------------ */
/* Typy uprawnień                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
	PERM_UNSET    = 0, /* brak wpisu — pytaj użytkownika           */
	PERM_ALLOWED  = 1, /* zgoda trwała (zapisana w bazie)           */
	PERM_ONCE     = 2, /* zgoda jednorazowa (tylko w RAM, nie w db)  */
	PERM_DENIED   = 3, /* odmowa trwała (zapisana w bazie)           */
} tektura_permission_state;

/* Rodzaj uprawnień, o które pyta aplikacja */
typedef enum {
	CAP_SCREENCOPY      = (1 << 0), /* wlr-screencopy-v1          */
	CAP_EXPORT_DMABUF   = (1 << 1), /* wlr-export-dmabuf-v1       */
	CAP_VIRTUAL_KEYBOARD= (1 << 2), /* wlr-virtual-keyboard-v1    */
	CAP_INPUT_INHIBIT   = (1 << 3), /* wlr-input-inhibitor-v1     */
} tektura_capability;

/* ------------------------------------------------------------------ */
/* Rekord w bazie (format binarny)                                      */
/* ------------------------------------------------------------------ */

/*
 * Jeden rekord opisuje uprawnienia jednej aplikacji.
 * Pole `app_path` to pełna ścieżka do binarki (wyciągana z /proc/PID/exe).
 * Pola są wyrównane — nie używaj #pragma pack, żeby uniknąć problemów
 * z przenośnością; zamiast tego baza ma pole `version` do walidacji.
 */
typedef struct {
	char     app_path[SECMGR_MAX_PATH]; /* ścieżka do binarki             */
	uint32_t capabilities;              /* maska bitowa tektura_capability */
	uint8_t  state;                     /* tektura_permission_state        */
	uint8_t  _pad[3];                   /* wyrównanie do 4 bajtów          */
	uint64_t granted_at;                /* Unix timestamp przyznania zgody */
} tektura_perm_record;

/* ------------------------------------------------------------------ */
/* Wynik zapytania o uprawnienie                                        */
/* ------------------------------------------------------------------ */

typedef struct {
	tektura_permission_state state;
	bool prompt_pending; /* true = trwa oczekiwanie na odpowiedź użytkownika */
} tektura_perm_result;

/* ------------------------------------------------------------------ */
/* Kontekst security managera (opaque — twórz przez secmgr_init)       */
/* ------------------------------------------------------------------ */

struct tektura_server;

typedef struct tektura_security_manager tektura_security_manager;

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

/*
 * Inicjalizuje security managera:
 *  - tworzy/otwiera plik bazy binarnej z wyłączną blokadą flock
 *  - weryfikuje podpis HMAC-SHA256
 *  - ładuje rekordy do pamięci RAM
 *  - uruchamia Unix socket dla panelu ustawień
 * Zwraca NULL w przypadku błędu krytycznego (np. naruszenia integralności).
 */
tektura_security_manager *secmgr_init(struct tektura_server *server);

/*
 * Sprawdza uprawnienie danej aplikacji (identyfikowanej przez PID klienta
 * Wayland) do żądanej możliwości.
 *
 * Jeśli brak wpisu w bazie (PERM_UNSET), ustawia prompt_pending = true
 * i wysyła żądanie do shella/panelu, by pokazał okienko autoryzacji.
 * Wywołujący powinien wtedy ZATRZYMAĆ bind protokołu i czekać na callback
 * `on_prompt_response`.
 */
tektura_perm_result secmgr_check(tektura_security_manager *mgr,
	pid_t client_pid, tektura_capability cap);

/*
 * Zapisuje decyzję użytkownika dla danej aplikacji i możliwości.
 * Jeśli state == PERM_ONCE, wpis trafia tylko do RAM (nie do pliku bazy).
 * Wywołaj po otrzymaniu odpowiedzi z okienka autoryzacji.
 */
void secmgr_set_permission(tektura_security_manager *mgr,
	const char *app_path, tektura_capability cap,
	tektura_permission_state state);

/*
 * Odwołuje uprawnienie dla aplikacji (np. z panelu ustawień).
 * Weryfikuje PID nadawcy żądania przed wykonaniem zmiany.
 */
void secmgr_revoke_permission(tektura_security_manager *mgr,
	const char *app_path, tektura_capability cap);

/*
 * Zwraca czytelną nazwę możliwości (do wyświetlenia w okienku).
 * Np. CAP_SCREENCOPY -> "robienie zrzutów ekranu"
 */
const char *secmgr_capability_name(tektura_capability cap);

/*
 * Odczytuje ścieżkę binarki procesu na podstawie jego PID.
 * Wynik zapisuje do buf (rozmiar buf_size). Zwraca false przy błędzie.
 */
bool secmgr_pid_to_path(pid_t pid, char *buf, size_t buf_size);

/*
 * Oznacza parę (app_path, cap) jako "oczekującą na odpowiedź shella".
 * Wywołaj po wysłaniu IPC_EVENT_PERMISSION_REQUEST — zapobiega duplikatom.
 */
void secmgr_mark_pending(tektura_security_manager *mgr,
	const char *app_path, tektura_capability cap);

/*
 * Usuwa parę (app_path, cap) z tabeli oczekujących.
 * Wywołaj po otrzymaniu odpowiedzi od shella przez on_permission_response.
 */
void secmgr_clear_pending(tektura_security_manager *mgr,
	const char *app_path, tektura_capability cap);

/*
 * Zwraca true, jeśli żądanie dla (app_path, cap) jest już oczekujące,
 * tzn. shell został już powiadomiony i czekamy na odpowiedź.
 */
bool secmgr_is_pending(tektura_security_manager *mgr,
	const char *app_path, tektura_capability cap);

/*
 * Niszczy security managera: zapisuje bazę (z nowym podpisem HMAC),
 * zwalnia blokadę flock, zamyka socket.
 */
void secmgr_destroy(tektura_security_manager *mgr);

#endif /* TEKTURA_SECURITY_MANAGER_H */
