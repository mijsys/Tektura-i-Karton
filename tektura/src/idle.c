/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * idle.c - Zarządzanie czasem bezczynności.
 *
 * Mechanizm:
 *   - Jeden wlr_timer (Wayland event loop) odlicza co sekundę.
 *   - Licznik idle_seconds jest inkrementowany; resetuje się przy
 *     każdym wejściu użytkownika (idle_notify_activity).
 *   - Po przekroczeniu progów: lock → dpms_off → systemctl suspend.
 *   - Obsługa ext-idle-notify-v1 (wlroots 0.18): protokół Wayland
 *     pozwalający klientom subskrybować zdarzenia bezczynności.
 *   - Inhibicja: gdy inhibited == true, licznik jest zatrzymany.
 */

#include "idle.h"
#include "lock.h"
#include "server.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#define IDLE_TICK_MS 1000 /* sprawdzaj co sekundę */

struct tektura_idle_manager {
	struct tektura_server        *server;
	tektura_idle_thresholds       thresholds;

	struct wl_event_source       *timer;
	uint32_t                      idle_seconds;

	/* Stany (zapobiegają wielokrotnemu wywołaniu akcji) */
	bool dpms_off;
	bool lock_done;
	bool suspend_done;

	bool inhibited;

	/* ext-idle-notify-v1 (wlroots) */
	struct wlr_idle_notifier_v1  *notifier;
};

/* ------------------------------------------------------------------ */
/* Pomocnicze                                                          */
/* ------------------------------------------------------------------ */

static void dpms_set_all_outputs(struct tektura_server *server, bool on) {
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &server->output_layout->outputs, link) {
		struct wlr_output *output = lo->output;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, on);
		wlr_output_commit_state(output, &state);
		wlr_output_state_finish(&state);
	}
	wlr_log(WLR_INFO, "idle: DPMS %s", on ? "ON" : "OFF");
}

static void do_suspend(void) {
	wlr_log(WLR_INFO, "idle: zawieszanie systemu (systemctl suspend)");
	/* execl nie wraca; jeśli zawiedzie — logujemy i wznawiamy */
	if (fork() == 0) {
		execlp("systemctl", "systemctl", "suspend", NULL);
		_exit(1);
	}
}

/* ------------------------------------------------------------------ */
/* Timer callback                                                      */
/* ------------------------------------------------------------------ */

static int idle_tick(void *data) {
	tektura_idle_manager *mgr = data;

	if (mgr->inhibited) goto reschedule;

	mgr->idle_seconds++;

	/* --- Blokada ekranu --- */
	uint32_t tl = mgr->thresholds.timeout_lock;
	if (tl > 0 && !mgr->lock_done && mgr->idle_seconds >= tl) {
		mgr->lock_done = true;
		if (mgr->server->lock_manager) {
			lock_request(mgr->server->lock_manager);
			wlr_log(WLR_INFO, "idle: blokowanie ekranu (idle=%us)", mgr->idle_seconds);
		}
	}

	/* --- DPMS off --- */
	uint32_t td = mgr->thresholds.timeout_dpms;
	if (td > 0 && !mgr->dpms_off && mgr->idle_seconds >= td) {
		mgr->dpms_off = true;
		dpms_set_all_outputs(mgr->server, false);
	}

	/* --- Suspend --- */
	uint32_t ts = mgr->thresholds.timeout_suspend;
	if (ts > 0 && !mgr->suspend_done && mgr->idle_seconds >= ts) {
		mgr->suspend_done = true;
		do_suspend();
	}

reschedule:
	wl_event_source_timer_update(mgr->timer, IDLE_TICK_MS);
	return 0;
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

tektura_idle_manager *idle_manager_init(struct tektura_server *server,
		const tektura_idle_thresholds *thresholds) {
	tektura_idle_manager *mgr = calloc(1, sizeof(*mgr));
	if (!mgr) return NULL;

	mgr->server = server;
	memcpy(&mgr->thresholds, thresholds, sizeof(*thresholds));

	/* Rejestruj ext-idle-notify-v1 (powiadomienia dla klientów Wayland) */
	mgr->notifier = wlr_idle_notifier_v1_create(server->wl_display);
	if (!mgr->notifier) {
		wlr_log(WLR_ERROR, "idle: nie udało się utworzyć wlr_idle_notifier_v1");
		free(mgr);
		return NULL;
	}

	struct wl_event_loop *loop =
		wl_display_get_event_loop(server->wl_display);
	mgr->timer = wl_event_loop_add_timer(loop, idle_tick, mgr);
	if (!mgr->timer) {
		wlr_log(WLR_ERROR, "idle: nie udało się utworzyć timera");
		free(mgr);
		return NULL;
	}

	wl_event_source_timer_update(mgr->timer, IDLE_TICK_MS);
	wlr_log(WLR_INFO, "idle: init (lock=%us dpms=%us suspend=%us)",
		thresholds->timeout_lock,
		thresholds->timeout_dpms,
		thresholds->timeout_suspend);
	return mgr;
}

void idle_notify_activity(tektura_idle_manager *mgr) {
	if (!mgr) return;

	bool was_idle = mgr->idle_seconds > 0;
	mgr->idle_seconds = 0;

	if (mgr->dpms_off) {
		mgr->dpms_off = false;
		dpms_set_all_outputs(mgr->server, true);
	}

	mgr->lock_done    = false;
	mgr->suspend_done = false;

	/* Poinformuj klientów ext-idle-notify-v1 o wznowieniu */
	if (was_idle && mgr->notifier) {
		wlr_idle_notifier_v1_notify_activity(mgr->notifier, mgr->server->seat);
	}
}

void idle_update_thresholds(tektura_idle_manager *mgr,
		const tektura_idle_thresholds *thresholds) {
	if (!mgr) return;
	memcpy(&mgr->thresholds, thresholds, sizeof(*thresholds));
	idle_notify_activity(mgr); /* reset liczników */
	wlr_log(WLR_INFO, "idle: zaktualizowano progi");
}

void idle_set_inhibit(tektura_idle_manager *mgr, bool inhibited) {
	if (!mgr || mgr->inhibited == inhibited) return;
	mgr->inhibited = inhibited;
	wlr_log(WLR_DEBUG, "idle: inhibicja %s", inhibited ? "ON" : "OFF");
	if (!inhibited) {
		/* Wznów odliczanie od zera */
		idle_notify_activity(mgr);
	}
}

void idle_manager_destroy(tektura_idle_manager *mgr) {
	if (!mgr) return;
	if (mgr->timer) wl_event_source_remove(mgr->timer);
	free(mgr);
}
