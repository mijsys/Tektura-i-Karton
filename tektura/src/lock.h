/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * lock.h - Blokada ekranu (screen lock).
 *
 * Implementacja oparta o protokół ext-session-lock-v1 (wlroots).
 *
 * Jak to działa:
 *   1. Kompozytor otrzymuje żądanie blokady (przez IPC "ACTION LOCK_SCREEN"
 *      lub po upłynięciu czasu bezczynności z idle.c).
 *   2. Kompozytor wysyła zdarzenie EXT_SESSION_LOCK do klienta blokady
 *      (np. karton-lock — osobna aplikacja wayland).
 *   3. Do czasu gdy klient blokady NIE wyśle unlock_and_destroy:
 *      - Wejście (klawiatura, mysz) trafia wyłącznie do klienta blokady.
 *      - Pozostałe aplikacje są "zamrożone" optycznie (nie dostają klatek).
 *   4. Klient blokady weryfikuje hasło i wywołuje unlock_and_destroy.
 *   5. Kompozytor wraca do normalnego trybu.
 *
 * Bezpieczeństwo:
 *   - Ekran jest blokowany natychmiast, BEZ czekania na klienta blokady.
 *     Jeśli klient nie podłączy się w ciągu LOCK_TIMEOUT_MS → ekran pozostaje
 *     czarny z wyłączonym wejściem.
 *   - Jeśli klient blokady "zginie" (crash) → ekran pozostaje czarny.
 *     Tylko kompozytor może odblokować ekran (przez awaryjne TTY).
 */

#ifndef TEKTURA_LOCK_H
#define TEKTURA_LOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct tektura_server;

/* Czas (ms) po którym ekran zostaje zablokowany jeśli klient blokady
 * nie podłączy się po żądaniu blokady. Ekran pozostaje czarny. */
#define LOCK_CLIENT_TIMEOUT_MS 3000

/* ------------------------------------------------------------------ */
/* Stan blokady                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
	LOCK_STATE_UNLOCKED = 0, /* normalny tryb pracy         */
	LOCK_STATE_LOCKING,      /* oczekiwanie na klienta       */
	LOCK_STATE_LOCKED,       /* klient podłączony, ekran zab.*/
	LOCK_STATE_ABANDONED,    /* klient zgubiony — czarny ekr.*/
} tektura_lock_state;

/* ------------------------------------------------------------------ */
/* Kontekst blokady (opaque)                                           */
/* ------------------------------------------------------------------ */

typedef struct tektura_lock_manager tektura_lock_manager;

/* ------------------------------------------------------------------ */
/* API publiczne                                                        */
/* ------------------------------------------------------------------ */

/*
 * Inicjalizuje menedżer blokady i rejestruje protokół
 * ext-session-lock-v1 w wl_display.
 */
tektura_lock_manager *lock_manager_init(struct tektura_server *server);

/*
 * Żąda zablokowania ekranu.
 * Wywołuje IPC_EVENT_LOCK_SCREEN, wyłącza wejście dla innych klientów.
 * Oczekuje na klienta blokady przez LOCK_CLIENT_TIMEOUT_MS.
 */
void lock_request(tektura_lock_manager *mgr);

/*
 * Zwraca aktualny stan blokady.
 */
tektura_lock_state lock_get_state(const tektura_lock_manager *mgr);

/*
 * Zwraca true jeśli ekran jest aktualnie zablokowany
 * (klient blokady aktywny lub tryb ABANDONED).
 */
bool lock_is_locked(const tektura_lock_manager *mgr);

/*
 * Niszczy menedżer blokady.
 */
void lock_manager_destroy(tektura_lock_manager *mgr);

#endif /* TEKTURA_LOCK_H */
