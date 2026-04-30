# Karton shell

Status: prototype (C)

Pierwszy dzialajacy prototyp shella Karton w C, z UI opartym o GTK4 + libadwaita.
Wyglad: colorful flat, zgodny z kierunkiem ze zrzutu (lewy rail, centralne okno plikow, prawy panel szybkich ustawien).

## Globalny styl
Shell laduje globalny motyw z `karton/theme/`:
- `gtk-light.css`
- `gtk-dark.css`

Ten sam zestaw kolorow powinien byc stosowany w aplikacjach Karton i dekoracjach okien Tektury.

## Struktura
- `src/main.c` - glowne okno shella i layout prototypu
- `meson.build` - konfiguracja budowania
- `run.sh` - szybkie build + run

## Wymagania
- `meson`
- `ninja`
- `pkg-config`
- `gtk4`
- `libadwaita-1`

Na Debian/Ubuntu (przykladowo):
`sudo apt install meson ninja-build pkg-config libgtk-4-dev libadwaita-1-dev`

## Uruchomienie
```bash
cd karton/apps/shell
./run.sh
```

Uruchomienie trybu ciemnego:
```bash
cd karton/apps/shell
KARTON_THEME=dark ./run.sh
```

## Co jest teraz
- Topbar z ikonami i popupami: quick settings, kalendarz, zegar
- Launcher jako animowany panel (GtkRevealer), otwierany z lewej belki i z ikony launcher na dole
- Dolna strefa podzielona na dwa rzedy:
	- uruchomione aplikacje (na gorze)
	- launcher + ikony startowe (na dole)
- Lista uruchomionych aplikacji pobierana przez IPC z Tektury (`QUERY WINDOWS` + HMAC)
- Przelacznik "Tryb ciemny" w quick settings przelacza globalny motyw light/dark

## Co dalej
- Podlaczenie IPC do Tektury (status workspace, aktywne okno, powiadomienia)
- Realny launcher i panel powiadomien
- Integracja ustawien i zarzadzania sesja

## Zmienne srodowiskowe
- `KARTON_THEME=light|dark`
- `TEKTURA_IPC_TOKEN=<64-hex>` (token sesji z kompozytora)
