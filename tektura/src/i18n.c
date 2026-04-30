/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * i18n.c - Implementacja systemu wielojęzyczności.
 *
 * Format pliku .po (uproszczony, kompatybilny z gettext):
 *   msgid "klucz"
 *   msgstr "tłumaczenie"
 *
 * Tłumaczenia wbudowane (embedded) — zawsze dostępne bez pliku na dysku.
 * Zewnętrzne pliki .po mogą je nadpisywać.
 */

#include "i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* Tłumaczenia wbudowane                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *key;
	const char *value;
} i18n_entry;

/* ----- Polski ----- */
static const i18n_entry translations_pl[] = {
	{I18N_PERM_TITLE,        "Żądanie uprawnień"},
	{I18N_PERM_APP_WANTS,    "%s chce: %s"},
	{I18N_CAP_SCREENCOPY,    "robić zrzuty ekranu"},
	{I18N_CAP_EXPORT_DMABUF, "nagrywać obraz ekranu (wideo)"},
	{I18N_CAP_VIRT_KEYBOARD, "emulować klawiaturę"},
	{I18N_CAP_INPUT_INHIBIT, "przechwytywać całe wejście"},
	{I18N_BTN_ALLOW_ALWAYS,  "Zawsze zezwalaj"},
	{I18N_BTN_ALLOW_ONCE,    "Tylko teraz"},
	{I18N_BTN_DENY,          "Odmów"},
	{I18N_BTN_UNINSTALL,     "Odinstaluj"},
	/* Ogólne */
	{"compositor.name",      "Tektura"},
	{"compositor.version",   "0.4.0"},
	{"error.generic",        "Wystąpił błąd"},
	{"error.permission",     "Brak uprawnień"},
	{"window.untitled",      "(bez tytułu)"},
	{"status.ready",         "Gotowy"},
	{NULL, NULL}
};

/* ----- English (fallback) ----- */
static const i18n_entry translations_en[] = {
	{I18N_PERM_TITLE,        "Permission Request"},
	{I18N_PERM_APP_WANTS,    "%s wants to: %s"},
	{I18N_CAP_SCREENCOPY,    "take screenshots"},
	{I18N_CAP_EXPORT_DMABUF, "record screen video"},
	{I18N_CAP_VIRT_KEYBOARD, "emulate keyboard"},
	{I18N_CAP_INPUT_INHIBIT, "capture all input"},
	{I18N_BTN_ALLOW_ALWAYS,  "Always allow"},
	{I18N_BTN_ALLOW_ONCE,    "Allow once"},
	{I18N_BTN_DENY,          "Deny"},
	{I18N_BTN_UNINSTALL,     "Uninstall"},
	/* General */
	{"compositor.name",      "Tektura"},
	{"compositor.version",   "0.4.0"},
	{"error.generic",        "An error occurred"},
	{"error.permission",     "Permission denied"},
	{"window.untitled",      "(untitled)"},
	{"status.ready",         "Ready"},
	{NULL, NULL}
};

/* ----- Deutsch ----- */
static const i18n_entry translations_de[] = {
	{I18N_PERM_TITLE,        "Berechtigungsanfrage"},
	{I18N_PERM_APP_WANTS,    "%s möchte: %s"},
	{I18N_CAP_SCREENCOPY,    "Screenshots aufnehmen"},
	{I18N_CAP_EXPORT_DMABUF, "Bildschirmvideo aufzeichnen"},
	{I18N_CAP_VIRT_KEYBOARD, "Tastatur emulieren"},
	{I18N_CAP_INPUT_INHIBIT, "Alle Eingaben abfangen"},
	{I18N_BTN_ALLOW_ALWAYS,  "Immer erlauben"},
	{I18N_BTN_ALLOW_ONCE,    "Nur einmal"},
	{I18N_BTN_DENY,          "Ablehnen"},
	{I18N_BTN_UNINSTALL,     "Deinstallieren"},
	{"compositor.name",      "Tektura"},
	{"compositor.version",   "0.4.0"},
	{"error.generic",        "Ein Fehler ist aufgetreten"},
	{"error.permission",     "Zugriff verweigert"},
	{"window.untitled",      "(kein Titel)"},
	{"status.ready",         "Bereit"},
	{NULL, NULL}
};

/* ------------------------------------------------------------------ */
/* Rejestr obsługiwanych języków                                        */
/* ------------------------------------------------------------------ */

typedef struct {
	const char       *code;         /* "pl", "en", "de" */
	const i18n_entry *builtin;      /* wbudowane tłumaczenia */
} i18n_locale_def;

static const i18n_locale_def supported_locales[] = {
	{"pl", translations_pl},
	{"en", translations_en},
	{"de", translations_de},
	{NULL, NULL}
};

/* ------------------------------------------------------------------ */
/* Stan modułu                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	char              current_code[8];
	const i18n_entry *current_table;  /* aktywna tabela wbudowana   */
	/* Tabela z pliku .po (dynamiczna, może być NULL) */
	i18n_entry       *file_table;
	size_t            file_table_count;
} i18n_state;

static i18n_state g_i18n = {0};

/* ------------------------------------------------------------------ */
/* Ładowanie zewnętrznego pliku .po                                     */
/* ------------------------------------------------------------------ */

static void free_file_table(void) {
	if (!g_i18n.file_table) return;
	for (size_t i = 0; i < g_i18n.file_table_count; i++) {
		free((char *)g_i18n.file_table[i].key);
		free((char *)g_i18n.file_table[i].value);
	}
	free(g_i18n.file_table);
	g_i18n.file_table = NULL;
	g_i18n.file_table_count = 0;
}

/*
 * Usuwa cudzysłowy i escape sequences z wartości .po
 * np. "Hello \"world\"" → Hello "world"
 */
static char *unescape_po_string(const char *raw) {
	size_t len = strlen(raw);
	/* Usuń otaczające cudzysłowy */
	if (len >= 2 && raw[0] == '"' && raw[len-1] == '"') {
		raw++;
		len -= 2;
	}
	char *out = malloc(len + 1);
	if (!out) return NULL;
	size_t j = 0;
	for (size_t i = 0; i < len; i++, j++) {
		if (raw[i] == '\\' && i + 1 < len) {
			i++;
			switch (raw[i]) {
			case 'n':  out[j] = '\n'; break;
			case 't':  out[j] = '\t'; break;
			case '"':  out[j] = '"';  break;
			case '\\': out[j] = '\\'; break;
			default:   out[j] = raw[i]; break;
			}
		} else {
			out[j] = raw[i];
		}
	}
	out[j] = '\0';
	return out;
}

static void load_po_file(const char *lang_code) {
	free_file_table();

	/* Szukaj pliku w kolejności:
	   1. ~/.config/karton/locale/<lang>/tektura.po (override użytkownika)
	   2. /usr/share/tektura/locale/<lang>/LC_MESSAGES/tektura.po
	   3. /usr/local/share/tektura/locale/<lang>/LC_MESSAGES/tektura.po
	*/
	char paths[3][512];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(paths[0], sizeof(paths[0]),
			"%s/.config/karton/locale/%s/tektura.po", home, lang_code);
	} else {
		paths[0][0] = '\0';
	}
	snprintf(paths[1], sizeof(paths[1]),
		"/usr/share/tektura/locale/%s/LC_MESSAGES/tektura.po", lang_code);
	snprintf(paths[2], sizeof(paths[2]),
		"/usr/local/share/tektura/locale/%s/LC_MESSAGES/tektura.po", lang_code);

	FILE *f = NULL;
	for (int i = 0; i < 3; i++) {
		if (!paths[i][0]) continue;
		f = fopen(paths[i], "r");
		if (f) {
			wlr_log(WLR_DEBUG, "i18n: ładuję %s", paths[i]);
			break;
		}
	}
	if (!f) {
		/* Brak pliku — używamy tylko wbudowanych */
		return;
	}

	/* Parsuj .po */
	#define MAX_FILE_ENTRIES 512
	i18n_entry *table = calloc(MAX_FILE_ENTRIES, sizeof(i18n_entry));
	if (!table) { fclose(f); return; }
	size_t count = 0;

	char line[1024];
	char pending_key[1024]  = {0};
	char pending_val[1024]  = {0};

	while (fgets(line, sizeof(line), f)) {
		/* Usuń trailing whitespace */
		size_t line_len = strlen(line);
		while (line_len > 0 && isspace((unsigned char)line[line_len-1])) {
			line[--line_len] = '\0';
		}

		if (strncmp(line, "msgid ", 6) == 0) {
			strncpy(pending_key, line + 6, sizeof(pending_key) - 1);
		} else if (strncmp(line, "msgstr ", 7) == 0) {
			strncpy(pending_val, line + 7, sizeof(pending_val) - 1);

			if (pending_key[0] && pending_val[0] && count < MAX_FILE_ENTRIES) {
				char *k = unescape_po_string(pending_key);
				char *v = unescape_po_string(pending_val);
				if (k && v && k[0]) {
					table[count].key   = k;
					table[count].value = v;
					count++;
				} else {
					free(k);
					free(v);
				}
			}
			pending_key[0] = '\0';
			pending_val[0] = '\0';
		}
	}
	fclose(f);

	g_i18n.file_table       = table;
	g_i18n.file_table_count = count;
	wlr_log(WLR_DEBUG, "i18n: załadowano %zu wpisów z pliku .po", count);
}

/* ------------------------------------------------------------------ */
/* Wykrywanie języka systemowego                                        */
/* ------------------------------------------------------------------ */

static const char *detect_system_locale(void) {
	/* Priorytet: TEKTURA_LANG > LANG > LANGUAGE > LC_ALL > "en" */
	const char *envvars[] = {"TEKTURA_LANG", "LANG", "LANGUAGE", "LC_ALL", NULL};
	for (int i = 0; envvars[i]; i++) {
		const char *val = getenv(envvars[i]);
		if (val && val[0]) {
			return val;
		}
	}
	return "en";
}

static const char *normalize_locale_code(const char *raw) {
	/* "pl_PL.UTF-8" → "pl", "en_US" → "en" */
	static char code[8];
	strncpy(code, raw, sizeof(code) - 1);
	code[sizeof(code) - 1] = '\0';
	for (int i = 0; code[i]; i++) {
		if (code[i] == '_' || code[i] == '.' || code[i] == '@') {
			code[i] = '\0';
			break;
		}
	}
	return code;
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

void i18n_init(void) {
	const char *raw = detect_system_locale();
	const char *code = normalize_locale_code(raw);
	if (!i18n_set_locale(code)) {
		i18n_set_locale("en");
	}
	wlr_log(WLR_INFO, "i18n: język: %s", g_i18n.current_code);
}

bool i18n_set_locale(const char *lang_code) {
	const char *code = normalize_locale_code(lang_code);
	for (int i = 0; supported_locales[i].code; i++) {
		if (strcmp(supported_locales[i].code, code) == 0) {
			strncpy(g_i18n.current_code, code, sizeof(g_i18n.current_code) - 1);
			g_i18n.current_table = supported_locales[i].builtin;
			load_po_file(code);
			wlr_log(WLR_DEBUG, "i18n: zmieniono język na %s", code);
			return true;
		}
	}
	wlr_log(WLR_ERROR, "i18n: nieobsługiwany język '%s'", code);
	return false;
}

const char *i18n_current_locale(void) {
	return g_i18n.current_code[0] ? g_i18n.current_code : "en";
}

const char *i18n_translate(const char *key) {
	if (!key) return "";

	/* Najpierw szukaj w zewnętrznym pliku .po (wyższy priorytet) */
	for (size_t i = 0; i < g_i18n.file_table_count; i++) {
		if (g_i18n.file_table[i].key &&
		    strcmp(g_i18n.file_table[i].key, key) == 0) {
			return g_i18n.file_table[i].value;
		}
	}

	/* Potem w wbudowanej tabeli bieżącego języka */
	if (g_i18n.current_table) {
		for (int i = 0; g_i18n.current_table[i].key; i++) {
			if (strcmp(g_i18n.current_table[i].key, key) == 0) {
				return g_i18n.current_table[i].value;
			}
		}
	}

	/* Fallback: angielski wbudowany */
	for (int i = 0; translations_en[i].key; i++) {
		if (strcmp(translations_en[i].key, key) == 0) {
			return translations_en[i].value;
		}
	}

	/* Ostateczny fallback: zwróć klucz */
	return key;
}

void i18n_destroy(void) {
	free_file_table();
	memset(&g_i18n, 0, sizeof(g_i18n));
}
