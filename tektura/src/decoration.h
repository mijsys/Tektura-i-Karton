/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * decoration.h - Server-Side Decorations (SSB).
 *
 * Każde okno otrzymuje belkę tytułową renderowaną przez kompozytor:
 *   ┌────────────────────────────────────────────┐
 *   │ [●] [●] [●]   Tytuł okna              [×] │  ← belka (titlebar)
 *   ├────────────────────────────────────────────┤
 *   │                                            │
 *   │           zawartość okna (CSD/SSD)         │
 *   │                                            │
 *   └────────────────────────────────────────────┘
 *
 * Renderowanie: wlr_scene_rect (kolorowe prostokąty) + wlr_scene_buffer
 * dla tekstu (FreeType → pixmap → wlr_texture).
 *
 * Styl definiowany w sekcji [decorations] pliku tektura.ini.
 */

#ifndef TEKTURA_DECORATION_H
#define TEKTURA_DECORATION_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct tektura_server;
struct tektura_toplevel;

/* ------------------------------------------------------------------ */
/* Konfiguracja wizualna belki                                         */
/* ------------------------------------------------------------------ */

typedef struct {
	/* Wysokość belki w pikselach (domyślnie 30) */
	int titlebar_height;

	/* Kolory RGBA [0.0–1.0] */
	float color_active[4];     /* belka aktywnego okna   */
	float color_inactive[4];   /* belka nieaktywnego     */
	float color_text[4];       /* kolor tytułu           */
	float color_button_close[4];
	float color_button_minimize[4];
	float color_button_maximize[4];

	/* Promień zaokrąglenia wlr_scene_rect (0 = prostokąt) */
	int border_radius;

	/* Grubość ramki okna (0 = brak) */
	int border_width;
	float color_border_active[4];
	float color_border_inactive[4];

	/* Rozmiar przycisków */
	int button_size;    /* domyślnie 14 */
	int button_margin;  /* odstęp między przyciskami, domyślnie 6 */
} tektura_decoration_config;

/* ------------------------------------------------------------------ */
/* Stan dekoracji jednego okna                                         */
/* ------------------------------------------------------------------ */

struct tektura_decoration;

/**
 * Tworzy dekorację (belkę + ramkę) dla toplevela.
 * Wywołaj przy mapowaniu okna.
 */
struct tektura_decoration *decoration_create(
	struct tektura_server *server,
	struct tektura_toplevel *toplevel);

/**
 * Aktualizuje tytuł, rozmiar i stan aktywne/nieaktywne.
 * Wywołaj przy commit (zmianie rozmiaru/tytułu) i zmianie fokusu.
 */
void decoration_update(struct tektura_decoration *deco, bool focused);

/**
 * Niszczy dekorację (usuwa węzły sceny).
 * Wywołaj przed zniszczeniem toplevela.
 */
void decoration_destroy(struct tektura_decoration *deco);

/**
 * Sprawdza czy punkt (sx, sy) w układzie okna trafia w belkę.
 * Używane do obsługi przeciągania okna i kliknięć w przyciski.
 *
 * @return DECO_HIT_NONE / DECO_HIT_TITLEBAR / DECO_HIT_BTN_CLOSE /
 *         DECO_HIT_BTN_MINIMIZE / DECO_HIT_BTN_MAXIMIZE
 */
typedef enum {
	DECO_HIT_NONE,
	DECO_HIT_TITLEBAR,
	DECO_HIT_BTN_CLOSE,
	DECO_HIT_BTN_MINIMIZE,
	DECO_HIT_BTN_MAXIMIZE,
} tektura_deco_hit;

tektura_deco_hit decoration_hit_test(
	struct tektura_decoration *deco, double sx, double sy);

/**
 * Inicjalizuje menedżera dekoracji — wczytuje config, rejestruje
 * xdg-decoration-v1 w trybie SSB.
 */
void decoration_manager_init(struct tektura_server *server);

/**
 * Wczytuje/aktualizuje konfig dekoracji (po config_reload).
 */
void decoration_load_config(struct tektura_server *server);

#endif /* TEKTURA_DECORATION_H */
