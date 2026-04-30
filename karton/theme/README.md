# Karton Global Theme

Ten katalog zawiera globalny styl wizualny dla calego srodowiska Karton:
- shell
- aplikacje GTK/libadwaita tworzone w ramach Karton
- dekoracje okien (kolorystyka zblizona do tych tokenow)

## Pliki
- `gtk-light.css` - jasny styl (pastel + blue accent)
- `gtk-dark.css` - ciemny styl (granat + violet accent)

## Uzycie w aplikacjach
Laduj CSS przez `GtkCssProvider` i stosuj klasy:
- kontenery: `shell-root`, `shell-topbar`, `left-rail`, `file-window`, `quick-panel`, `shell-dock`
- elementy: `rail-item`, `rail-item active`, `quick-chip`, `quick-chip active`, `dock-item`

## Uzycie motywu
Motyw wybierany przez zmienna srodowiskowa:
- `KARTON_THEME=light` (domyslnie)
- `KARTON_THEME=dark`
