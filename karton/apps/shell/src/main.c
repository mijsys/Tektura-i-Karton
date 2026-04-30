#include <adwaita.h>
#include <gtk/gtk.h>

#include <glib.h>
#include <inttypes.h>
#include <openssl/hmac.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef KARTON_THEME_DIR
#define KARTON_THEME_DIR "."
#endif

typedef struct {
  GtkWidget *launcher_revealer;
  GtkWidget *running_row;
  GtkSwitch *dark_switch;
  GtkCssProvider *theme_provider;
  gboolean dark_mode;
  guint ipc_poll_id;
  guint64 ipc_seq;
  uint8_t ipc_token[32];
  bool ipc_token_valid;
  char ipc_token_hex[65];
} ShellUiState;

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
  size_t hex_len = strlen(hex);
  if (hex_len != out_len * 2) {
    return false;
  }

  for (size_t i = 0; i < out_len; i++) {
    char tmp[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
    char *endptr = NULL;
    long value = strtol(tmp, &endptr, 16);
    if (!endptr || *endptr != '\0' || value < 0 || value > 255) {
      return false;
    }
    out[i] = (uint8_t)value;
  }

  return true;
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = hex[in[i] >> 4];
    out[i * 2 + 1] = hex[in[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

static void apply_theme(ShellUiState *state) {
  char *css_path = g_build_filename(
      KARTON_THEME_DIR,
      state->dark_mode ? "gtk-dark.css" : "gtk-light.css",
      NULL);

  if (state->theme_provider) {
    gtk_style_context_remove_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(state->theme_provider));
    g_object_unref(state->theme_provider);
  }

  state->theme_provider = gtk_css_provider_new();
  gtk_css_provider_load_from_path(state->theme_provider, css_path);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(),
      GTK_STYLE_PROVIDER(state->theme_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_free(css_path);
}

static gboolean on_dark_switch_state_set(GtkSwitch *sw, gboolean state_active, gpointer user_data) {
  (void)sw;
  ShellUiState *state = user_data;
  state->dark_mode = state_active;
  apply_theme(state);
  return FALSE;
}

static void on_toggle_launcher(GtkButton *button, gpointer user_data) {
  (void)button;
  ShellUiState *state = user_data;
  gboolean reveal = gtk_revealer_get_reveal_child(GTK_REVEALER(state->launcher_revealer));
  gtk_revealer_set_reveal_child(GTK_REVEALER(state->launcher_revealer), !reveal);
}

static GtkWidget *make_popover_title(const char *title) {
  GtkWidget *label = gtk_label_new(title);
  gtk_widget_add_css_class(label, "popover-title");
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  return label;
}

static GtkWidget *make_toggle_row(const char *name, gboolean active) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(row, "option-row");

  GtkWidget *label = gtk_label_new(name);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  GtkWidget *sw = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(sw), active);

  gtk_box_append(GTK_BOX(row), label);
  gtk_box_append(GTK_BOX(row), sw);
  return row;
}

static GtkWidget *build_quick_settings_popover(ShellUiState *state) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(box, "popover-card");
  gtk_box_append(GTK_BOX(box), make_popover_title("Szybkie ustawienia"));

  GtkWidget *brightness = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_range_set_value(GTK_RANGE(brightness), 72);
  GtkWidget *volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_range_set_value(GTK_RANGE(volume), 56);
  gtk_box_append(GTK_BOX(box), brightness);
  gtk_box_append(GTK_BOX(box), volume);

  gtk_box_append(GTK_BOX(box), make_toggle_row("Wi-Fi", TRUE));
  gtk_box_append(GTK_BOX(box), make_toggle_row("Bluetooth", FALSE));
  GtkWidget *dark_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(dark_row, "option-row");
  GtkWidget *dark_label = gtk_label_new("Tryb ciemny");
  gtk_widget_set_hexpand(dark_label, TRUE);
  gtk_widget_set_halign(dark_label, GTK_ALIGN_START);
  GtkWidget *dark_sw = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(dark_sw), state->dark_mode);
  state->dark_switch = GTK_SWITCH(dark_sw);
  g_signal_connect(dark_sw, "state-set", G_CALLBACK(on_dark_switch_state_set), state);
  gtk_box_append(GTK_BOX(dark_row), dark_label);
  gtk_box_append(GTK_BOX(dark_row), dark_sw);
  gtk_box_append(GTK_BOX(box), dark_row);
  gtk_box_append(GTK_BOX(box), make_toggle_row("Nie przeszkadzac", TRUE));
  return box;
}

static GtkWidget *build_calendar_popover(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(box, "popover-card");
  gtk_box_append(GTK_BOX(box), make_popover_title("Kalendarz"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("pon, 20 maja"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("10:00  Spotkanie zespolu"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("14:30  Przeglad sprintu"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("17:00  TODO: shell integration"));
  return box;
}

static GtkWidget *build_clock_popover(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(box, "popover-card");
  gtk_box_append(GTK_BOX(box), make_popover_title("Zegar"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("Biezacy czas: 10:30"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("Alarm: 07:00 (wlaczony)"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("Minutnik: 15 min"));
  gtk_box_append(GTK_BOX(box), gtk_label_new("Strefa: Europe/Warsaw"));
  return box;
}

static bool ipc_write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n <= 0) {
      return false;
    }
    off += (size_t)n;
  }
  return true;
}

static bool ipc_read_line(int fd, char *buf, size_t buf_size) {
  size_t off = 0;
  while (off + 1 < buf_size) {
    ssize_t n = read(fd, buf + off, 1);
    if (n <= 0) {
      return false;
    }
    if (buf[off] == '\n') {
      buf[off] = '\0';
      return true;
    }
    off++;
  }
  buf[buf_size - 1] = '\0';
  return true;
}

static bool ipc_send_signed_and_read(ShellUiState *state, int fd,
    const char *plain_msg, char *response, size_t response_size) {
  char seq_hex[17];
  snprintf(seq_hex, sizeof(seq_hex), "%016" PRIx64, state->ipc_seq);

  char signed_data[2048];
  snprintf(signed_data, sizeof(signed_data), "%s:%s", seq_hex, plain_msg);

  uint8_t digest[32];
  unsigned int digest_len = 0;
  HMAC(EVP_sha256(), state->ipc_token, (int)sizeof(state->ipc_token),
      (const unsigned char *)signed_data, strlen(signed_data),
      digest, &digest_len);

  char digest_hex[65];
  bytes_to_hex(digest, 32, digest_hex);

  char wire[4096];
  snprintf(wire, sizeof(wire), "%s %s %s\n", digest_hex, seq_hex, plain_msg);
  if (!ipc_write_all(fd, wire, strlen(wire))) {
    return false;
  }

  state->ipc_seq++;
  return ipc_read_line(fd, response, response_size);
}

static GPtrArray *ipc_fetch_running_apps(ShellUiState *state) {
  GPtrArray *apps = g_ptr_array_new_with_free_func(g_free);

  const char *runtime = g_getenv("XDG_RUNTIME_DIR");
  if (!runtime || !state->ipc_token_valid) {
    return apps;
  }

  char sock_path[512];
  snprintf(sock_path, sizeof(sock_path), "%s/tektura/ipc.sock", runtime);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return apps;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return apps;
  }

  char line[4096];
  if (!ipc_read_line(fd, line, sizeof(line))) {
    close(fd);
    return apps;
  }

  char hello[256];
  snprintf(hello, sizeof(hello), "HELLO %s\n", state->ipc_token_hex);
  if (!ipc_write_all(fd, hello, strlen(hello))) {
    close(fd);
    return apps;
  }

  if (!ipc_read_line(fd, line, sizeof(line))) {
    close(fd);
    return apps;
  }

  if (!g_str_has_prefix(line, "OK")) {
    close(fd);
    return apps;
  }

  if (!ipc_send_signed_and_read(state, fd, "QUERY WINDOWS", line, sizeof(line))) {
    close(fd);
    return apps;
  }

  close(fd);

  if (!g_str_has_prefix(line, "OK ")) {
    return apps;
  }

  char *payload = line + 3;
  char *saveptr = NULL;
  for (char *chunk = strtok_r(payload, ";", &saveptr);
       chunk != NULL;
       chunk = strtok_r(NULL, ";", &saveptr)) {
    char *app = strstr(chunk, "app:");
    if (!app) {
      continue;
    }
    app += 4;
    char *end = strchr(app, ',');
    if (!end) {
      end = strchr(app, '}');
    }
    if (!end || end <= app) {
      continue;
    }
    size_t len = (size_t)(end - app);
    char *name = g_strndup(app, len);
    if (name[0] == '\0') {
      g_free(name);
      continue;
    }

    bool exists = false;
    for (guint i = 0; i < apps->len; i++) {
      const char *existing = g_ptr_array_index(apps, i);
      if (g_strcmp0(existing, name) == 0) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      g_ptr_array_add(apps, name);
    } else {
      g_free(name);
    }
  }

  return apps;
}

static void clear_box_children(GtkWidget *box) {
  GtkWidget *child = gtk_widget_get_first_child(box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_box_remove(GTK_BOX(box), child);
    child = next;
  }
}

static void update_running_row(ShellUiState *state, GPtrArray *apps) {
  clear_box_children(state->running_row);

  if (apps->len == 0) {
    const char *fallback[] = {"Pliki", "Terminal", "Sklep", "Ustawienia"};
    for (int i = 0; i < 4; i++) {
      GtkWidget *btn = gtk_button_new_with_label(fallback[i]);
      gtk_widget_add_css_class(btn, "running-app");
      gtk_box_append(GTK_BOX(state->running_row), btn);
    }
    return;
  }

  for (guint i = 0; i < apps->len && i < 8; i++) {
    const char *name = g_ptr_array_index(apps, i);
    GtkWidget *btn = gtk_button_new_with_label(name);
    gtk_widget_add_css_class(btn, "running-app");
    gtk_box_append(GTK_BOX(state->running_row), btn);
  }
}

static gboolean poll_running_apps(gpointer user_data) {
  ShellUiState *state = user_data;
  GPtrArray *apps = ipc_fetch_running_apps(state);
  update_running_row(state, apps);
  g_ptr_array_free(apps, TRUE);
  return G_SOURCE_CONTINUE;
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  ShellUiState *state = user_data;
  if (state->ipc_poll_id > 0) {
    g_source_remove(state->ipc_poll_id);
  }
  if (state->theme_provider) {
    gtk_style_context_remove_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(state->theme_provider));
    g_object_unref(state->theme_provider);
  }
  g_free(state);
}

static GtkWidget *make_file_card(const char *label, const char *css_class) {
  GtkWidget *frame = gtk_frame_new(NULL);
  gtk_widget_add_css_class(frame, "file-grid-card");
  if (css_class && css_class[0]) {
    gtk_widget_add_css_class(frame, css_class);
  }

  GtkWidget *name = gtk_label_new(label);
  gtk_widget_add_css_class(name, "file-title");
  gtk_frame_set_child(GTK_FRAME(frame), name);
  return frame;
}

static GtkWidget *make_topbar_popover_button(const char *icon_name, GtkWidget *popover_content) {
  GtkWidget *btn = gtk_menu_button_new();
  gtk_widget_add_css_class(btn, "topbar-icon");

  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), icon);

  GtkWidget *popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(popover), popover_content);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn), popover);
  return btn;
}

static GtkWidget *build_launcher_panel(void) {
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class(panel, "launcher-panel");

  GtkWidget *search = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Szukaj aplikacji...");
  gtk_box_append(GTK_BOX(panel), search);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);

  const char *apps[] = {
    "Pliki", "Terminal", "Sklep", "Ustawienia", "Kalendarz", "Zegar", "Zrzuty", "Monitor"
  };
  for (int i = 0; i < 8; i++) {
    GtkWidget *btn = gtk_button_new_with_label(apps[i]);
    gtk_widget_add_css_class(btn, "launcher-item");
    gtk_grid_attach(GTK_GRID(grid), btn, i % 4, i / 4, 1, 1);
  }

  gtk_box_append(GTK_BOX(panel), grid);
  return panel;
}

static void on_app_activate(GApplication *app, gpointer user_data) {
  (void)user_data;

  ShellUiState *state = g_new0(ShellUiState, 1);
  const char *theme = g_getenv("KARTON_THEME");
  state->dark_mode = theme && g_ascii_strcasecmp(theme, "dark") == 0;

  const char *token_hex = g_getenv("TEKTURA_IPC_TOKEN");
  if (token_hex && strlen(token_hex) == 64) {
    snprintf(state->ipc_token_hex, sizeof(state->ipc_token_hex), "%s", token_hex);
    state->ipc_token_valid = hex_to_bytes(token_hex, state->ipc_token, sizeof(state->ipc_token));
  }

  AdwApplicationWindow *win = ADW_APPLICATION_WINDOW(adw_application_window_new(ADW_APPLICATION(app)));
  gtk_window_set_title(GTK_WINDOW(win), "Karton Shell");
  gtk_window_set_default_size(GTK_WINDOW(win), 1240, 760);
  apply_theme(state);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(root, "shell-root");

  GtkWidget *topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(topbar, "shell-topbar");

  GtkWidget *left = gtk_label_new("pon, 20 maj");
  gtk_widget_add_css_class(left, "topbar-clock");
  gtk_widget_set_hexpand(left, TRUE);
  gtk_widget_set_halign(left, GTK_ALIGN_START);

  GtkWidget *topbar_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(topbar_right, "topbar-right");

  GtkWidget *quick_btn = make_topbar_popover_button("emblem-system-symbolic", build_quick_settings_popover(state));
  GtkWidget *calendar_btn = make_topbar_popover_button("x-office-calendar-symbolic", build_calendar_popover());
  GtkWidget *clock_btn = gtk_menu_button_new();
  gtk_widget_add_css_class(clock_btn, "topbar-icon");
  gtk_menu_button_set_label(GTK_MENU_BUTTON(clock_btn), "10:30");
  GtkWidget *clock_popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(clock_popover), build_clock_popover());
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(clock_btn), clock_popover);

  gtk_box_append(GTK_BOX(topbar_right), quick_btn);
  gtk_box_append(GTK_BOX(topbar_right), calendar_btn);
  gtk_box_append(GTK_BOX(topbar_right), clock_btn);

  gtk_box_append(GTK_BOX(topbar), left);
  gtk_box_append(GTK_BOX(topbar), topbar_right);

  GtkWidget *workspace = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(workspace, 10);
  gtk_widget_set_margin_end(workspace, 10);
  gtk_widget_set_vexpand(workspace, TRUE);

  GtkWidget *left_rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(left_rail, "left-rail");
  GtkWidget *launcher_toggle = gtk_button_new_with_label("Launcher");
  gtk_widget_add_css_class(launcher_toggle, "rail-item");
  gtk_widget_add_css_class(launcher_toggle, "active");
  gtk_box_append(GTK_BOX(left_rail), launcher_toggle);
  gtk_box_append(GTK_BOX(left_rail), gtk_button_new_with_label("Kosz"));
  gtk_box_append(GTK_BOX(left_rail), gtk_button_new_with_label("Przegladarka"));
  gtk_box_append(GTK_BOX(left_rail), gtk_button_new_with_label("Ustawienia"));

  /* Ujednolic klasy przyciskow bocznych */
  GtkWidget *child = gtk_widget_get_first_child(left_rail);
  while (child) {
    gtk_widget_add_css_class(child, "rail-item");
    child = gtk_widget_get_next_sibling(child);
  }

  GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_hexpand(center, TRUE);

  GtkWidget *launcher_revealer = gtk_revealer_new();
  state->launcher_revealer = launcher_revealer;
  gtk_revealer_set_transition_type(GTK_REVEALER(launcher_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_transition_duration(GTK_REVEALER(launcher_revealer), 220);
  gtk_revealer_set_reveal_child(GTK_REVEALER(launcher_revealer), FALSE);
  gtk_revealer_set_child(GTK_REVEALER(launcher_revealer), build_launcher_panel());
  gtk_box_append(GTK_BOX(center), launcher_revealer);

  GtkWidget *file_window = gtk_frame_new(NULL);
  gtk_widget_add_css_class(file_window, "file-window");
  gtk_widget_set_hexpand(file_window, TRUE);
  gtk_widget_set_vexpand(file_window, TRUE);

  GtkWidget *file_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  GtkWidget *file_title = gtk_label_new("Katalog domowy");
  gtk_widget_add_css_class(file_title, "file-title");
  gtk_widget_set_halign(file_title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(file_box), file_title);

  GtkWidget *file_grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(file_grid), 10);
  gtk_grid_set_row_spacing(GTK_GRID(file_grid), 10);
  gtk_widget_set_hexpand(file_grid, TRUE);
  gtk_widget_set_vexpand(file_grid, TRUE);

  GtkWidget *cards[] = {
    make_file_card("Dokumenty", "blue"),
    make_file_card("Pobrane", "green"),
    make_file_card("Muzyka", "pink"),
    make_file_card("Obrazy", "orange"),
    make_file_card("Video", "purple"),
    make_file_card("Szablony", "yellow"),
    make_file_card("Publiczny", "teal")
  };
  for (int i = 0; i < 7; i++) {
    gtk_grid_attach(GTK_GRID(file_grid), cards[i], i % 4, i / 4, 1, 1);
  }

  gtk_box_append(GTK_BOX(file_box), file_grid);
  gtk_frame_set_child(GTK_FRAME(file_window), file_box);
  gtk_box_append(GTK_BOX(center), file_window);

  GtkWidget *dock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(dock, "shell-dock");

  GtkWidget *running_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  state->running_row = running_row;
  gtk_widget_add_css_class(running_row, "running-row");
  const char *running_apps[] = {"Pliki", "Terminal", "Sklep", "Ustawienia"};
  for (int i = 0; i < 4; i++) {
    GtkWidget *app = gtk_button_new_with_label(running_apps[i]);
    gtk_widget_add_css_class(app, "running-app");
    gtk_box_append(GTK_BOX(running_row), app);
  }

  GtkWidget *launcher_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(launcher_row, "launcher-row");
  GtkWidget *launcher_main = gtk_button_new_with_label("Launcher");
  gtk_widget_add_css_class(launcher_main, "launcher-main");
  gtk_box_append(GTK_BOX(launcher_row), launcher_main);

  const char *dock_items[] = {"Files", "Terminal", "Store", "Settings", "Shots"};
  for (int i = 0; i < 5; i++) {
    GtkWidget *item = gtk_button_new_with_label(dock_items[i]);
    gtk_widget_add_css_class(item, "dock-item");
    gtk_box_append(GTK_BOX(launcher_row), item);
  }

  gtk_box_append(GTK_BOX(dock), running_row);
  gtk_box_append(GTK_BOX(dock), launcher_row);

  gtk_box_append(GTK_BOX(workspace), left_rail);
  gtk_box_append(GTK_BOX(workspace), center);

  gtk_box_append(GTK_BOX(root), topbar);
  gtk_box_append(GTK_BOX(root), workspace);
  gtk_box_append(GTK_BOX(root), dock);

  g_signal_connect_data(launcher_toggle, "clicked",
      G_CALLBACK(on_toggle_launcher), state,
      NULL, 0);

  g_signal_connect_data(launcher_main, "clicked",
      G_CALLBACK(on_toggle_launcher), state,
      NULL, 0);

  poll_running_apps(state);
  state->ipc_poll_id = g_timeout_add_seconds(2, poll_running_apps, state);

  g_signal_connect_data(win, "destroy", G_CALLBACK(on_window_destroy), state, NULL, 0);

  adw_application_window_set_content(win, root);
  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  g_autoptr(AdwApplication) app = adw_application_new("org.karton.shell", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), NULL);
  return g_application_run(G_APPLICATION(app), argc, argv);
}
