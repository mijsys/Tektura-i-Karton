/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * config.h - System konfiguracji kompozytora.
 *
 * Format pliku: INI-like, UTF-8, komentarze '#' i ';'
 *
 * Lokalizacja:
 *   1. $TEKTURA_CONFIG (zmienna środowiskowa)
 *   2. $XDG_CONFIG_HOME/karton/tektura.ini
 *   3. ~/.config/karton/tektura.ini
 *   4. /etc/karton/tektura.ini (systemowe)
 *
 * Przykład tektura.ini:
 *   [general]
 *   locale = pl
 *   workspaces = 4
 *   startup = alacritty
 *
 *   [input]
 *   keyboard_layout = pl
 *   keyboard_variant = basic
 *   repeat_rate = 25
 *   repeat_delay = 600
 *   natural_scroll = false
 *   cursor_theme = default
 *   cursor_size = 24
 *
 *   [idle]
 *   timeout_lock = 300       # sekundy do blokady (0 = wyłączone)
 *   timeout_dpms = 600       # sekundy do wygas. monitora (0 = wyłączone)
 *   timeout_suspend = 900    # sekundy do uśpienia systemu (0 = wyłączone)
 *
 *   [security]
 *   require_auth_on_lock = true
 *   lock_on_sleep = true
 *
 *   [keybindings]
 *   # Format: <mod>+<key> = <action>
 *   super+t = spawn alacritty
 *   super+q = close_window
 *   super+l = lock_screen
 *   super+1 = workspace 1
 *   super+right = workspace_next
 *
 *   [decorations]
 *   titlebar_height = 30     # wysokość belki w px
 *   border_width    = 1      # grubość ramki (0 = brak)
 *   button_size     = 14     # rozmiar przycisku (px)
 *   color_active    = #2e2e2e
 *   color_inactive  = #4d4d4d
 *   color_border    = #6699e6
 *   color_btn_close    = #e64545
 *   color_btn_minimize = #f5c021
 *   color_btn_maximize = #45c145
 *   default_mode = auto
 *   force_csd_apps = firefox,code
 *   force_ssd_apps = foot,alacritty
 */

#ifndef TEKTURA_CONFIG_H
#define TEKTURA_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Sekcja [general]                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
	char    locale[16];       /* kod języka np. "pl"                   */
	uint32_t workspaces;      /* liczba wirtualnych pulpitów           */
	char    startup_cmd[256]; /* komenda uruchamiana po starcie         */
} config_general;

/* ------------------------------------------------------------------ */
/* Sekcja [input]                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
	char    keyboard_layout[16];  /* "pl", "us", "de" ...              */
	char    keyboard_variant[32]; /* "basic", "nodeadkeys" ...         */
	int     repeat_rate;          /* znak/s (25)                        */
	int     repeat_delay;         /* ms do pierwszego powtórzenia (600)*/
	bool    natural_scroll;       /* odwrócony scroll touchpada         */
	char    cursor_theme[64];     /* nazwa motywu kursora               */
	int     cursor_size;          /* rozmiar kursora w px               */
} config_input;

/* ------------------------------------------------------------------ */
/* Sekcja [idle]                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t timeout_lock;    /* s do blokady ekranu (0=wył)           */
	uint32_t timeout_dpms;    /* s do wygas. monitora (0=wył)          */
	uint32_t timeout_suspend; /* s do uśpienia (0=wył)                 */
} config_idle;

/* ------------------------------------------------------------------ */
/* Sekcja [security]                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
	bool require_auth_on_lock; /* wymagaj hasła przy odblokowywaniu     */
	bool lock_on_sleep;        /* blokuj ekran przy uśpieniu systemu    */
} config_security;

/* ------------------------------------------------------------------ */
/* Wiązanie klawisza (keybinding)                                      */
/* ------------------------------------------------------------------ */

#define CONFIG_MAX_KEYBINDINGS 64

typedef struct {
	uint32_t modifiers;    /* WLR_MODIFIER_*: CTRL, ALT, SUPER, SHIFT  */
	uint32_t keysym;       /* xkb_keysym_t                              */
	char     action[128];  /* np. "spawn alacritty" "workspace 2"       */
} config_keybinding;

/* ------------------------------------------------------------------ */
/* Sekcja [decorations]                                                */
/* ------------------------------------------------------------------ */

typedef struct {
	int  titlebar_height;     /* wysokość belki w px (domyślnie 30)       */
	int  border_width;        /* grubość ramki w px (0 = brak)            */
	int  button_size;         /* rozmiar przycisku w px (domyślnie 14)    */
	char color_active[10];    /* hex np. "#2e2e2e" aktywna belka          */
	char color_inactive[10];  /* hex szara belka nieaktywna               */
	char color_border[10];    /* hex kolor ramki aktywnej                 */
	char color_btn_close[10]; /* hex czerwony przycisk zamknij             */
	char color_btn_min[10];   /* hex żółty minimalizuj                     */
	char color_btn_max[10];   /* hex zielony maksymalizuj                  */
	char default_mode[16];    /* auto|csd|ssd                              */
	char force_csd_apps[256]; /* CSV app_id preferujące CSD                */
	char force_ssd_apps[256]; /* CSV app_id wymuszające SSB                */
} config_decorations;

/* ------------------------------------------------------------------ */
/* Pełna konfiguracja                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
	config_general    general;
	config_input      input;
	config_idle       idle;
	config_security   security;
	config_decorations decorations;
	config_keybinding keybindings[CONFIG_MAX_KEYBINDINGS];
	int               keybinding_count;
} tektura_config;

/* ------------------------------------------------------------------ */
/* API publiczne                                                        */
/* ------------------------------------------------------------------ */

/*
 * Wczytuje konfigurację z pliku.
 * Jeśli plik nie istnieje — wypełnia domyślnymi wartościami.
 * Zwraca zaalokowaną strukturę (zwolnij przez config_destroy).
 */
tektura_config *config_load(void);

/*
 * Przeładowuje konfigurację w locie (np. po SIGHUP).
 * Wypełnia *cfg na nowo.
 */
void config_reload(tektura_config *cfg);

/* Zwolnienie zasobów */
void config_destroy(tektura_config *cfg);

/*
 * Zwraca ścieżkę do aktualnie używanego pliku konfiguracji.
 */
const char *config_get_path(void);

#endif /* TEKTURA_CONFIG_H */
