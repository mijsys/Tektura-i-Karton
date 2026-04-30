/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * config.c - Wczytywanie i parsowanie pliku konfiguracji INI.
 */

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* ------------------------------------------------------------------ */
/* Domyślna konfiguracja                                               */
/* ------------------------------------------------------------------ */

static void config_set_defaults(tektura_config *cfg) {
	/* [general] */
	strncpy(cfg->general.locale, "pl", sizeof(cfg->general.locale) - 1);
	cfg->general.workspaces = 4;
	cfg->general.startup_cmd[0] = '\0';

	/* [input] */
	strncpy(cfg->input.keyboard_layout, "pl",
		sizeof(cfg->input.keyboard_layout) - 1);
	strncpy(cfg->input.keyboard_variant, "basic",
		sizeof(cfg->input.keyboard_variant) - 1);
	cfg->input.repeat_rate    = 25;
	cfg->input.repeat_delay   = 600;
	cfg->input.natural_scroll = false;
	strncpy(cfg->input.cursor_theme, "default",
		sizeof(cfg->input.cursor_theme) - 1);
	cfg->input.cursor_size = 24;

	/* [idle] */
	cfg->idle.timeout_lock    = 300;
	cfg->idle.timeout_dpms    = 600;
	cfg->idle.timeout_suspend = 0; /* domyślnie wyłączone */

	/* [security] */
	cfg->security.require_auth_on_lock = true;
	cfg->security.lock_on_sleep        = true;

	/* [decorations] */
	cfg->decorations.titlebar_height = 30;
	cfg->decorations.border_width    = 1;
	cfg->decorations.button_size     = 14;
	strncpy(cfg->decorations.color_active,   "#2e2e2e", sizeof(cfg->decorations.color_active) - 1);
	strncpy(cfg->decorations.color_inactive, "#4d4d4d", sizeof(cfg->decorations.color_inactive) - 1);
	strncpy(cfg->decorations.color_border,   "#6699e6", sizeof(cfg->decorations.color_border) - 1);
	strncpy(cfg->decorations.color_btn_close,"#e64545", sizeof(cfg->decorations.color_btn_close) - 1);
	strncpy(cfg->decorations.color_btn_min,  "#f5c021", sizeof(cfg->decorations.color_btn_min) - 1);
	strncpy(cfg->decorations.color_btn_max,  "#45c145", sizeof(cfg->decorations.color_btn_max) - 1);
	strncpy(cfg->decorations.default_mode, "auto", sizeof(cfg->decorations.default_mode) - 1);
	cfg->decorations.force_csd_apps[0] = '\0';
	cfg->decorations.force_ssd_apps[0] = '\0';

	/* [keybindings] — domyślne skróty */
	cfg->keybinding_count = 0;

	/* Super+T → alacritty */
	config_keybinding *kb = &cfg->keybindings[cfg->keybinding_count++];
	kb->modifiers = WLR_MODIFIER_LOGO;
	kb->keysym    = XKB_KEY_t;
	strncpy(kb->action, "spawn alacritty", sizeof(kb->action) - 1);

	/* Super+Q → zamknij okno */
	kb = &cfg->keybindings[cfg->keybinding_count++];
	kb->modifiers = WLR_MODIFIER_LOGO;
	kb->keysym    = XKB_KEY_q;
	strncpy(kb->action, "close_window", sizeof(kb->action) - 1);

	/* Super+L → zablokuj ekran */
	kb = &cfg->keybindings[cfg->keybinding_count++];
	kb->modifiers = WLR_MODIFIER_LOGO;
	kb->keysym    = XKB_KEY_l;
	strncpy(kb->action, "lock_screen", sizeof(kb->action) - 1);

	/* Super+→ → następny workspace */
	kb = &cfg->keybindings[cfg->keybinding_count++];
	kb->modifiers = WLR_MODIFIER_LOGO;
	kb->keysym    = XKB_KEY_Right;
	strncpy(kb->action, "workspace_next", sizeof(kb->action) - 1);

	/* Super+← → poprzedni workspace */
	kb = &cfg->keybindings[cfg->keybinding_count++];
	kb->modifiers = WLR_MODIFIER_LOGO;
	kb->keysym    = XKB_KEY_Left;
	strncpy(kb->action, "workspace_prev", sizeof(kb->action) - 1);

	/* Super+1..4 → przełącz na workspace */
	const xkb_keysym_t num_keys[] = {
		XKB_KEY_1, XKB_KEY_2, XKB_KEY_3, XKB_KEY_4
	};
	for (int i = 0; i < 4 && cfg->keybinding_count < CONFIG_MAX_KEYBINDINGS; i++) {
		kb = &cfg->keybindings[cfg->keybinding_count++];
		kb->modifiers = WLR_MODIFIER_LOGO;
		kb->keysym    = num_keys[i];
		snprintf(kb->action, sizeof(kb->action), "workspace %d", i + 1);
	}

	/* Alt+F4 → zamknij okno */
	kb = &cfg->keybindings[cfg->keybinding_count++];
	kb->modifiers = WLR_MODIFIER_ALT;
	kb->keysym    = XKB_KEY_F4;
	strncpy(kb->action, "close_window", sizeof(kb->action) - 1);

	/* Alt+Clucz → wyjście z kompozytora (debug) */
	kb = &cfg->keybindings[cfg->keybinding_count++];
	kb->modifiers = WLR_MODIFIER_ALT;
	kb->keysym    = XKB_KEY_Escape;
	strncpy(kb->action, "quit", sizeof(kb->action) - 1);
}

/* ------------------------------------------------------------------ */
/* Znajdowanie pliku konfiguracji                                      */
/* ------------------------------------------------------------------ */

static char g_config_path[512] = {0};

static bool find_config_file(char *out, size_t out_size) {
	/* 1. Zmienna środowiskowa */
	const char *env = getenv("TEKTURA_CONFIG");
	if (env && env[0]) {
		struct stat st;
		if (stat(env, &st) == 0) {
			strncpy(out, env, out_size - 1);
			return true;
		}
	}

	/* 2. XDG_CONFIG_HOME */
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0]) {
		snprintf(out, out_size, "%s/karton/tektura.ini", xdg);
		struct stat st;
		if (stat(out, &st) == 0) return true;
	}

	/* 3. ~/.config/karton/tektura.ini */
	const char *home = getenv("HOME");
	if (home) {
		snprintf(out, out_size, "%s/.config/karton/tektura.ini", home);
		struct stat st;
		if (stat(out, &st) == 0) return true;
	}

	/* 4. /etc/karton/tektura.ini */
	snprintf(out, out_size, "/etc/karton/tektura.ini");
	struct stat st;
	if (stat(out, &st) == 0) return true;

	out[0] = '\0';
	return false;
}

/* ------------------------------------------------------------------ */
/* Parsowanie INI                                                       */
/* ------------------------------------------------------------------ */

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)*(end-1))) end--;
	*end = '\0';
	return s;
}

/* Parsuj modyfikatory z napisu "super+shift+t" → modifiers + keysym */
static bool parse_keybinding_spec(const char *spec,
		uint32_t *modifiers, uint32_t *keysym) {
	*modifiers = 0;
	char buf[128];
	strncpy(buf, spec, sizeof(buf) - 1);
	buf[sizeof(buf)-1] = '\0';

	/* Zamień na lowercase */
	for (char *p = buf; *p; p++) *p = tolower((unsigned char)*p);

	/* Split po '+' */
	char *tok = strtok(buf, "+");
	char last_part[64] = {0};
	while (tok) {
		char *next = strtok(NULL, "+");
		if (next) {
			/* to jest modyfikator */
			if (strcmp(tok, "super") == 0 || strcmp(tok, "mod4") == 0)
				*modifiers |= WLR_MODIFIER_LOGO;
			else if (strcmp(tok, "alt") == 0 || strcmp(tok, "mod1") == 0)
				*modifiers |= WLR_MODIFIER_ALT;
			else if (strcmp(tok, "ctrl") == 0 || strcmp(tok, "control") == 0)
				*modifiers |= WLR_MODIFIER_CTRL;
			else if (strcmp(tok, "shift") == 0)
				*modifiers |= WLR_MODIFIER_SHIFT;
		} else {
			strncpy(last_part, tok, sizeof(last_part) - 1);
		}
		tok = next;
	}

	if (!last_part[0]) return false;
	xkb_keysym_t sym = xkb_keysym_from_name(last_part, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) return false;
	*keysym = sym;
	return true;
}

static void parse_ini(tektura_config *cfg, FILE *f) {
	char line[512];
	char section[64] = {0};

	while (fgets(line, sizeof(line), f)) {
		char *l = trim(line);
		if (!l[0] || l[0] == '#' || l[0] == ';') continue;

		/* Sekcja */
		if (l[0] == '[') {
			char *end = strchr(l, ']');
			if (end) {
				*end = '\0';
				strncpy(section, l + 1, sizeof(section) - 1);
			}
			continue;
		}

		/* Klucz = wartość */
		char *eq = strchr(l, '=');
		if (!eq) continue;
		*eq = '\0';
		char *key = trim(l);
		char *val = trim(eq + 1);
		/* Usuń komentarze końcowe */
		char *comment = strchr(val, '#');
		if (comment) { *comment = '\0'; val = trim(val); }
		comment = strchr(val, ';');
		if (comment) { *comment = '\0'; val = trim(val); }

		if (strcmp(section, "general") == 0) {
			if (strcmp(key, "locale") == 0)
				strncpy(cfg->general.locale, val, sizeof(cfg->general.locale) - 1);
			else if (strcmp(key, "workspaces") == 0)
				cfg->general.workspaces = (uint32_t)atoi(val);
			else if (strcmp(key, "startup") == 0)
				strncpy(cfg->general.startup_cmd, val, sizeof(cfg->general.startup_cmd) - 1);

		} else if (strcmp(section, "input") == 0) {
			if (strcmp(key, "keyboard_layout") == 0)
				strncpy(cfg->input.keyboard_layout, val, sizeof(cfg->input.keyboard_layout) - 1);
			else if (strcmp(key, "keyboard_variant") == 0)
				strncpy(cfg->input.keyboard_variant, val, sizeof(cfg->input.keyboard_variant) - 1);
			else if (strcmp(key, "repeat_rate") == 0)
				cfg->input.repeat_rate = atoi(val);
			else if (strcmp(key, "repeat_delay") == 0)
				cfg->input.repeat_delay = atoi(val);
			else if (strcmp(key, "natural_scroll") == 0)
				cfg->input.natural_scroll = (strcmp(val, "true") == 0);
			else if (strcmp(key, "cursor_theme") == 0)
				strncpy(cfg->input.cursor_theme, val, sizeof(cfg->input.cursor_theme) - 1);
			else if (strcmp(key, "cursor_size") == 0)
				cfg->input.cursor_size = atoi(val);

		} else if (strcmp(section, "idle") == 0) {
			if (strcmp(key, "timeout_lock") == 0)
				cfg->idle.timeout_lock = (uint32_t)atoi(val);
			else if (strcmp(key, "timeout_dpms") == 0)
				cfg->idle.timeout_dpms = (uint32_t)atoi(val);
			else if (strcmp(key, "timeout_suspend") == 0)
				cfg->idle.timeout_suspend = (uint32_t)atoi(val);

		} else if (strcmp(section, "security") == 0) {
			if (strcmp(key, "require_auth_on_lock") == 0)
				cfg->security.require_auth_on_lock = (strcmp(val, "true") == 0);
			else if (strcmp(key, "lock_on_sleep") == 0)
				cfg->security.lock_on_sleep = (strcmp(val, "true") == 0);

		} else if (strcmp(section, "decorations") == 0) {
			if (strcmp(key, "titlebar_height") == 0)
				cfg->decorations.titlebar_height = atoi(val);
			else if (strcmp(key, "border_width") == 0)
				cfg->decorations.border_width = atoi(val);
			else if (strcmp(key, "button_size") == 0)
				cfg->decorations.button_size = atoi(val);
			else if (strcmp(key, "default_mode") == 0)
				strncpy(cfg->decorations.default_mode, val, sizeof(cfg->decorations.default_mode) - 1);
			else if (strcmp(key, "force_csd_apps") == 0)
				strncpy(cfg->decorations.force_csd_apps, val, sizeof(cfg->decorations.force_csd_apps) - 1);
			else if (strcmp(key, "force_ssd_apps") == 0)
				strncpy(cfg->decorations.force_ssd_apps, val, sizeof(cfg->decorations.force_ssd_apps) - 1);
			else if (strcmp(key, "color_active") == 0)
				strncpy(cfg->decorations.color_active, val, sizeof(cfg->decorations.color_active) - 1);
			else if (strcmp(key, "color_inactive") == 0)
				strncpy(cfg->decorations.color_inactive, val, sizeof(cfg->decorations.color_inactive) - 1);
			else if (strcmp(key, "color_border") == 0)
				strncpy(cfg->decorations.color_border, val, sizeof(cfg->decorations.color_border) - 1);
			else if (strcmp(key, "color_btn_close") == 0)
				strncpy(cfg->decorations.color_btn_close, val, sizeof(cfg->decorations.color_btn_close) - 1);
			else if (strcmp(key, "color_btn_minimize") == 0)
				strncpy(cfg->decorations.color_btn_min, val, sizeof(cfg->decorations.color_btn_min) - 1);
			else if (strcmp(key, "color_btn_maximize") == 0)
				strncpy(cfg->decorations.color_btn_max, val, sizeof(cfg->decorations.color_btn_max) - 1);

		} else if (strcmp(section, "keybindings") == 0) {
			if (cfg->keybinding_count >= CONFIG_MAX_KEYBINDINGS) continue;
			uint32_t mods = 0, sym = 0;
			if (parse_keybinding_spec(key, &mods, &sym)) {
				config_keybinding *kb =
					&cfg->keybindings[cfg->keybinding_count++];
				kb->modifiers = mods;
				kb->keysym    = sym;
				strncpy(kb->action, val, sizeof(kb->action) - 1);
			} else {
				wlr_log(WLR_DEBUG, "config: nieznany skrót '%s'", key);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

tektura_config *config_load(void) {
	tektura_config *cfg = calloc(1, sizeof(*cfg));
	if (!cfg) return NULL;

	config_set_defaults(cfg);

	if (!find_config_file(g_config_path, sizeof(g_config_path))) {
		wlr_log(WLR_INFO, "config: brak pliku konfiguracji — używam domyślnych");
		return cfg;
	}

	FILE *f = fopen(g_config_path, "r");
	if (!f) {
		wlr_log(WLR_ERROR, "config: nie mogę otworzyć %s", g_config_path);
		return cfg;
	}

	parse_ini(cfg, f);
	fclose(f);

	wlr_log(WLR_INFO, "config: wczytano z %s (%d skrótów)",
		g_config_path, cfg->keybinding_count);
	return cfg;
}

void config_reload(tektura_config *cfg) {
	if (!cfg) return;
	config_set_defaults(cfg);
	if (!g_config_path[0]) return;
	FILE *f = fopen(g_config_path, "r");
	if (!f) return;
	parse_ini(cfg, f);
	fclose(f);
	wlr_log(WLR_INFO, "config: przeładowano konfigurację");
}

void config_destroy(tektura_config *cfg) {
	free(cfg);
}

const char *config_get_path(void) {
	return g_config_path[0] ? g_config_path : "(brak pliku — ustawienia domyślne)";
}
