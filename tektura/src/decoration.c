/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * decoration.c - Server-Side Decorations (SSB).
 *
 * Renderowanie przez wlr_scene_rect — kolorowe prostokąty bez zewnętrznych
 * zależności. Każde okno xdg-toplevel dostaje belkę + ramkę niezależnie od
 * tego czy aplikacja obsługuje xdg-decoration-v1 (foot, gtk4, qt6, electron —
 * wszystkie dostaną jednolity wygląd Kartonu).
 *
 * Układ w przestrzeni sceny (pozycje relative do deco_tree):
 *
 *   deco_tree ustawiony na (0, TH+BW) w scene_tree toplevela
 *   → "lokalne" (0,0) deco_tree = górny-lewy róg zawartości okna
 *
 *   titlebar      @ (0, -(TH+BW))  relative do deco_tree
 *   border_top    @ (0, -(TH+2BW))
 *   border_left   @ (-BW, -(TH+2BW))
 *   border_right  @ (W, -(TH+2BW))
 *   border_bottom @ (0, H-BW)      — dzięki offsetowi deco_tree
 */

#include "decoration.h"
#include "server.h"
#include "view.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* Globalny config dekoracji                                           */
/* ------------------------------------------------------------------ */

static tektura_decoration_config g_deco_cfg;

static void set_default_config(tektura_decoration_config *cfg) {
cfg->titlebar_height  = 30;
cfg->border_radius    = 0;
cfg->border_width     = 1;
cfg->button_size      = 14;
cfg->button_margin    = 6;

/* Aktywna belka */
cfg->color_active[0]   = 0.18f; cfg->color_active[1]   = 0.18f;
cfg->color_active[2]   = 0.18f; cfg->color_active[3]   = 1.00f;
/* Nieaktywna belka */
cfg->color_inactive[0] = 0.30f; cfg->color_inactive[1] = 0.30f;
cfg->color_inactive[2] = 0.30f; cfg->color_inactive[3] = 1.00f;
/* Tekst */
cfg->color_text[0] = cfg->color_text[1] = cfg->color_text[2] = cfg->color_text[3] = 1.0f;
/* Przyciski */
cfg->color_button_close[0]    = 0.90f; cfg->color_button_close[1]    = 0.27f;
cfg->color_button_close[2]    = 0.27f; cfg->color_button_close[3]    = 1.0f;
cfg->color_button_minimize[0] = 0.96f; cfg->color_button_minimize[1] = 0.75f;
cfg->color_button_minimize[2] = 0.13f; cfg->color_button_minimize[3] = 1.0f;
cfg->color_button_maximize[0] = 0.27f; cfg->color_button_maximize[1] = 0.70f;
cfg->color_button_maximize[2] = 0.27f; cfg->color_button_maximize[3] = 1.0f;
/* Ramka aktywna — niebieska */
cfg->color_border_active[0]   = 0.40f; cfg->color_border_active[1]   = 0.55f;
cfg->color_border_active[2]   = 0.90f; cfg->color_border_active[3]   = 1.0f;
/* Ramka nieaktywna — szara */
cfg->color_border_inactive[0] = 0.30f; cfg->color_border_inactive[1] = 0.30f;
cfg->color_border_inactive[2] = 0.30f; cfg->color_border_inactive[3] = 1.0f;
}

static bool parse_color(const char *hex, float out[4]) {
if (!hex || hex[0] != '#') return false;
unsigned r = 0, g = 0, b = 0, a = 255;
int n = sscanf(hex + 1, "%02x%02x%02x%02x", &r, &g, &b, &a);
if (n < 3) return false;
out[0] = r / 255.0f; out[1] = g / 255.0f;
out[2] = b / 255.0f; out[3] = a / 255.0f;
return true;
}

/* ------------------------------------------------------------------ */
/* Struktura dekoracji                                                  */
/* ------------------------------------------------------------------ */

struct tektura_decoration {
struct tektura_server   *server;
struct tektura_toplevel *toplevel;

struct wlr_scene_tree   *deco_tree;   /* podpięty do scene_tree toplevela */

struct wlr_scene_rect   *titlebar;
struct wlr_scene_rect   *btn_close;
struct wlr_scene_rect   *btn_minimize;
struct wlr_scene_rect   *btn_maximize;

struct wlr_scene_rect   *border_top;
struct wlr_scene_rect   *border_left;
struct wlr_scene_rect   *border_right;
struct wlr_scene_rect   *border_bottom;

bool focused;
int  win_w;
int  win_h;
};

/* ------------------------------------------------------------------ */
/* Pomocnicze                                                           */
/* ------------------------------------------------------------------ */

static struct wlr_scene_rect *make_rect(struct wlr_scene_tree *parent,
int x, int y, int w, int h, const float col[4]) {
if (w < 1) w = 1;
if (h < 1) h = 1;
struct wlr_scene_rect *r = wlr_scene_rect_create(parent, w, h, col);
if (r) wlr_scene_node_set_position(&r->node, x, y);
return r;
}

static void rect_set(struct wlr_scene_rect *r,
int x, int y, int w, int h, const float col[4]) {
if (!r) return;
if (w < 1) w = 1;
if (h < 1) h = 1;
wlr_scene_rect_set_size(r, w, h);
wlr_scene_rect_set_color(r, col);
wlr_scene_node_set_position(&r->node, x, y);
}

/* ------------------------------------------------------------------ */
/* layout_decoration — przelicza wszystkie pozycje                     */
/* ------------------------------------------------------------------ */

static void layout_decoration(struct tektura_decoration *deco) {
const tektura_decoration_config *c = &g_deco_cfg;
const int W  = deco->win_w;
const int H  = deco->win_h;
const int TH = c->titlebar_height;
const int BW = c->border_width;
const int BS = c->button_size;
const int BM = c->button_margin;

const float *bar = deco->focused ? c->color_active       : c->color_inactive;
const float *brd = deco->focused ? c->color_border_active : c->color_border_inactive;

/*
 * deco_tree jest w pozycji (0, TH+BW) w scene_tree toplevela.
 * Podajemy coords relative do deco_tree:
 *   (0,0) deco_tree = punkt (0, TH+BW) w scene_tree
 *   surface okna zaczyna się w (0, TH+BW) → w układzie deco_tree: (0, 0)
 */

/* Belka: nad (0,0) → y = -(TH+BW) do -BW */
rect_set(deco->titlebar, 0, -(TH + BW), W, TH, bar);

/* Przyciski w belce (od prawej) */
int by = -(TH + BW) + (TH - BS) / 2;
int bx = W - BM - BS;
rect_set(deco->btn_close,    bx, by, BS, BS, c->color_button_close);
bx -= BM + BS;
rect_set(deco->btn_maximize, bx, by, BS, BS, c->color_button_maximize);
bx -= BM + BS;
rect_set(deco->btn_minimize, bx, by, BS, BS, c->color_button_minimize);

/* Ramka */
if (BW > 0) {
int full_h = TH + H + 2 * BW;
rect_set(deco->border_top,    0,   -(TH + 2 * BW), W,  BW,     brd);
rect_set(deco->border_left,   -BW, -(TH + 2 * BW), BW, full_h, brd);
rect_set(deco->border_right,  W,   -(TH + 2 * BW), BW, full_h, brd);
rect_set(deco->border_bottom, 0,   H,               W,  BW,     brd);
}
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                        */
/* ------------------------------------------------------------------ */

struct tektura_decoration *decoration_create(
struct tektura_server *server,
struct tektura_toplevel *toplevel) {
struct tektura_decoration *deco = calloc(1, sizeof(*deco));
if (!deco) return NULL;
deco->server   = server;
deco->toplevel = toplevel;
deco->focused  = false;
deco->win_w    = 1;
deco->win_h    = 1;

const tektura_decoration_config *c = &g_deco_cfg;
const int TH = c->titlebar_height;
const int BW = c->border_width;
const int BS = c->button_size;
const float zero[4] = {0};

/* Drzewo dekoracji jako drugie dziecko scene_tree (pierwsze to surface) */
deco->deco_tree = wlr_scene_tree_create(toplevel->scene_tree);
if (!deco->deco_tree) { free(deco); return NULL; }

/*
 * Przesuń deco_tree tak, żeby jego (0,0) pokrywał się z lewym górnym
 * rogiem zawartości okna. Surface okna jest w (0,0) scene_tree,
 * więc przesuwamy deco_tree do (BW, TH+BW) — wtedy dekoracje
 * rysowane z ujemnymi Y wychodzą ponad okno.
 */
wlr_scene_node_set_position(&deco->deco_tree->node, BW, TH + BW);

/* Belka */
deco->titlebar     = make_rect(deco->deco_tree, 0, -(TH+BW), 1,  TH, c->color_active);
/* Przyciski — tymczasowe pozycje (layout_decoration poprawi) */
deco->btn_close    = make_rect(deco->deco_tree, 0, 0, BS, BS, c->color_button_close);
deco->btn_minimize = make_rect(deco->deco_tree, 0, 0, BS, BS, c->color_button_minimize);
deco->btn_maximize = make_rect(deco->deco_tree, 0, 0, BS, BS, c->color_button_maximize);

/* Ramka */
if (BW > 0) {
deco->border_top    = make_rect(deco->deco_tree, 0, 0, 1,  BW, zero);
deco->border_left   = make_rect(deco->deco_tree, 0, 0, BW, 1,  zero);
deco->border_right  = make_rect(deco->deco_tree, 0, 0, BW, 1,  zero);
deco->border_bottom = make_rect(deco->deco_tree, 0, 0, 1,  BW, zero);
}

wlr_log(WLR_DEBUG, "decoration: belka %dpx, ramka %dpx", TH, BW);
return deco;
}

void decoration_update(struct tektura_decoration *deco, bool focused) {
if (!deco) return;
deco->focused = focused;

struct wlr_box geo = {0};
wlr_xdg_surface_get_geometry(deco->toplevel->xdg_toplevel->base, &geo);

/* Przy initial_commit rozmiar może wynosić 0 — poczekaj na kolejny */
if (geo.width  > 0) deco->win_w = geo.width;
if (geo.height > 0) deco->win_h = geo.height;

/* Zaktualizuj pozycję deco_tree gdy rozmiar znany */
const tektura_decoration_config *c = &g_deco_cfg;
wlr_scene_node_set_position(&deco->deco_tree->node,
c->border_width, c->titlebar_height + c->border_width);

layout_decoration(deco);
}

void decoration_destroy(struct tektura_decoration *deco) {
if (!deco) return;
if (deco->deco_tree)
wlr_scene_node_destroy(&deco->deco_tree->node);
free(deco);
}

tektura_deco_hit decoration_hit_test(
struct tektura_decoration *deco, double sx, double sy) {
if (!deco || deco->win_w <= 0) return DECO_HIT_NONE;

const tektura_decoration_config *c = &g_deco_cfg;
const int TH = c->titlebar_height;
const int BS = c->button_size;
const int BM = c->button_margin;
const int W  = deco->win_w;

/*
	/*
	 * sx, sy pochodzi z wlr_scene_node_at → relative do węzła surface.
	 * Surface jest w (BW, TH+BW) w scene_tree, więc kliknięcie w belkę
	 * trafia ze współrzędnymi sy ∈ [-(TH+BW), -BW).
	 */
	const int BW = c->border_width;
	if (sy >= -(double)(TH + BW) && sy < -(double)BW) {
		/* Przelicz sx na układ wnętrza belki (bez lewej ramki) */
		double lsx = sx - (double)BW;
		int bx = W - BM - BS;
		if (lsx >= bx && lsx < bx + BS) return DECO_HIT_BTN_CLOSE;
		bx -= BM + BS;
		if (lsx >= bx && lsx < bx + BS) return DECO_HIT_BTN_MAXIMIZE;
		bx -= BM + BS;
		if (lsx >= bx && lsx < bx + BS) return DECO_HIT_BTN_MINIMIZE;
		return DECO_HIT_TITLEBAR;
	}

return DECO_HIT_NONE;
}

/* ------------------------------------------------------------------ */
/* Wczytywanie konfiguracji                                            */
/* ------------------------------------------------------------------ */

void decoration_load_config(struct tektura_server *server) {
set_default_config(&g_deco_cfg);
if (!server->config) return;

const config_decorations *dc = &server->config->decorations;

if (dc->titlebar_height > 0) g_deco_cfg.titlebar_height = dc->titlebar_height;
if (dc->border_width    >= 0) g_deco_cfg.border_width   = dc->border_width;
if (dc->button_size     > 0) g_deco_cfg.button_size     = dc->button_size;

if (dc->color_active[0])    parse_color(dc->color_active,    g_deco_cfg.color_active);
if (dc->color_inactive[0])  parse_color(dc->color_inactive,  g_deco_cfg.color_inactive);
if (dc->color_border[0])    parse_color(dc->color_border,    g_deco_cfg.color_border_active);
if (dc->color_btn_close[0]) parse_color(dc->color_btn_close, g_deco_cfg.color_button_close);
if (dc->color_btn_min[0])   parse_color(dc->color_btn_min,   g_deco_cfg.color_button_minimize);
if (dc->color_btn_max[0])   parse_color(dc->color_btn_max,   g_deco_cfg.color_button_maximize);

for (int i = 0; i < 3; i++)
g_deco_cfg.color_border_inactive[i] = g_deco_cfg.color_border_active[i] * 0.5f;
g_deco_cfg.color_border_inactive[3] = 1.0f;
}

void decoration_manager_init(struct tektura_server *server) {
set_default_config(&g_deco_cfg);
decoration_load_config(server);
wlr_log(WLR_INFO, "decoration: SSB init (belka=%dpx ramka=%dpx przyciski=%dpx)",
g_deco_cfg.titlebar_height, g_deco_cfg.border_width, g_deco_cfg.button_size);
}
