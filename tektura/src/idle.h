/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * idle.h - Zarządzanie czasem bezczynności (lock/dpms/suspend).
 */

#ifndef TEKTURA_IDLE_H
#define TEKTURA_IDLE_H

#include <stdint.h>
#include <wayland-server-core.h>

struct tektura_server;

/*
 * Progi czasowe; 0 = wyłączone.
 * Kolejność: lock ≤ dpms ≤ suspend (wymuszane przy inicjalizacji).
 */
typedef struct {
	uint32_t timeout_lock;    /* sekundy → blokada ekranu        */
	uint32_t timeout_dpms;    /* sekundy → wygaszenie monitorów  */
	uint32_t timeout_suspend; /* sekundy → systemctl suspend     */
} tektura_idle_thresholds;

typedef struct tektura_idle_manager tektura_idle_manager;

/**
 * Inicjalizuje menedżera bezczynności.
 *
 * @param server  Główna struktura serwera.
 * @param thresholds  Progi z konfiguracji (kopiowane wewnętrznie).
 * @return Wskaźnik do nowego obiektu lub NULL przy błędzie.
 */
tektura_idle_manager *idle_manager_init(struct tektura_server *server,
	const tektura_idle_thresholds *thresholds);

/**
 * Powiadamia menedżera o aktywności użytkownika
 * (wywołaj przy każdym wejściu z klawiatury / myszy).
 */
void idle_notify_activity(tektura_idle_manager *mgr);

/**
 * Aktualizuje progi (np. po przeładowaniu konfiguracji).
 * Resetuje liczniki bezczynności.
 */
void idle_update_thresholds(tektura_idle_manager *mgr,
	const tektura_idle_thresholds *thresholds);

/**
 * Wymusza inhibicję bezczynności (np. pełnoekranowy odtwarzacz wideo).
 * Wywołaj idle_release_inhibit() aby cofnąć.
 */
void idle_set_inhibit(tektura_idle_manager *mgr, bool inhibited);

/**
 * Niszczy menedżera i zwalnia zasoby.
 */
void idle_manager_destroy(tektura_idle_manager *mgr);

#endif /* TEKTURA_IDLE_H */
