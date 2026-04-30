/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * i18n.h - System wielojęzyczności (internationalization).
 *
 * Projekt:
 *   - Napisy UI trzymane w prostych plikach .po (gettext-compatible)
 *     w katalogu /usr/share/tektura/locale/<lang>/LC_MESSAGES/tektura.po
 *     lub w ~/.config/karton/locale/ (override użytkownika).
 *   - Fallback: jeśli brak tłumaczenia → angielski.
 *   - Zmiana języka w locie przez IPC (ACTION SET_LOCALE pl).
 *   - Makro T() do tłumaczenia napisów w kodzie źródłowym.
 *
 * Obsługiwane języki startowe:
 *   pl - Polski (domyślny dla projektu Karton)
 *   en - English (fallback)
 *
 * Dodanie nowego języka:
 *   1. Dodaj plik locale/<lang>/tektura.po z tłumaczeniami.
 *   2. Zarejestruj kod w tablicy supported_locales[] w i18n.c.
 */

#ifndef TEKTURA_I18N_H
#define TEKTURA_I18N_H

#include <stdbool.h>

/* Skrócone makro do użycia w kodzie — T("Jakiś napis") */
#define T(key) i18n_translate(key)

/* ------------------------------------------------------------------ */
/* API publiczne                                                        */
/* ------------------------------------------------------------------ */

/*
 * Inicjalizuje system i18n.
 * Wczytuje język z:
 *   1. Zmiennej środowiskowej TEKTURA_LANG lub LANG
 *   2. Fallback: "en"
 * Wczytuje tablicę tłumaczeń do RAM.
 */
void i18n_init(void);

/*
 * Zmienia język w locie. Wczytuje nowe tłumaczenia.
 * Zwraca false jeśli język nieobsługiwany.
 */
bool i18n_set_locale(const char *lang_code);

/* Zwraca aktualny kod języka (np. "pl", "en") */
const char *i18n_current_locale(void);

/*
 * Tłumaczy klucz na bieżący język.
 * Jeśli brak tłumaczenia — zwraca key (angielski fallback).
 */
const char *i18n_translate(const char *key);

/* Zwalnia zasoby systemu i18n */
void i18n_destroy(void);

/* ------------------------------------------------------------------ */
/* Klucze tłumaczeń używane przez security_manager (okienko promptu)  */
/* ------------------------------------------------------------------ */

/* Tytuł okna autoryzacji */
#define I18N_PERM_TITLE          "permission.dialog.title"
/* "%s wants to: %s" */
#define I18N_PERM_APP_WANTS      "permission.app_wants"
/* Treść żądania screencopy */
#define I18N_CAP_SCREENCOPY      "capability.screencopy"
/* Treść żądania dmabuf */
#define I18N_CAP_EXPORT_DMABUF   "capability.export_dmabuf"
/* Treść żądania virtual keyboard */
#define I18N_CAP_VIRT_KEYBOARD   "capability.virtual_keyboard"
/* Treść żądania input inhibit */
#define I18N_CAP_INPUT_INHIBIT   "capability.input_inhibit"
/* Przycisk "Zawsze zezwalaj" */
#define I18N_BTN_ALLOW_ALWAYS    "button.allow_always"
/* Przycisk "Tylko teraz" */
#define I18N_BTN_ALLOW_ONCE      "button.allow_once"
/* Przycisk "Odmów" */
#define I18N_BTN_DENY            "button.deny"
/* Przycisk "Odinstaluj" */
#define I18N_BTN_UNINSTALL       "button.uninstall"

#endif /* TEKTURA_I18N_H */
