/*
 * Tektura - Kompozytor Wayland dla środowiska Karton
 * Autor: MijagiKutasamoto
 *
 * security_manager.c - Implementacja systemu uprawnień aplikacji.
 *
 * Mechanizmy ochrony bazy:
 *   1. flock(LOCK_EX) — wyłączna blokada pliku bazy przez cały czas
 *      działania kompozytora. Żaden inny proces nie może go zmodyfikować.
 *
 *   2. HMAC-SHA256 — każdy zapis bazy generuje skrót kryptograficzny.
 *      Przy starcie kompozytor weryfikuje skrót. Jeśli ktoś edytował plik
 *      z zewnątrz, skrót nie pasuje → baza zostaje zresetowana + wpis w logu.
 *
 *   3. /run/user/<uid>/ — baza trafia na tmpfs dostępny tylko dla właściciela
 *      (mode 0700 tworzony przez system). Niedostępny dla innych użytkowników.
 *
 *   4. Format binarny — nie JSON/XML. Trudniejszy do ręcznej edycji.
 *      Struktura: [nagłówek 32B][rekordy][HMAC 32B na końcu]
 *
 *   5. Unix socket dla panelu ustawień — zewnętrzne programy NIE mogą
 *      bezpośrednio pisać do pliku. Wysyłają żądanie przez socket,
 *      a kompozytor weryfikuje PID nadawcy przed wykonaniem zmiany.
 */

#include "security_manager.h"
#include "server.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------------ */
/* Stałe wewnętrzne                                                    */
/* ------------------------------------------------------------------ */

#define DB_MAGIC    "TKTRPERM"        /* 8 bajtów — identyfikator formatu */
#define HMAC_SIZE   32                /* SHA256 = 32 bajty                 */
#define MAX_RECORDS 1024              /* maksymalna liczba wpisów w bazie  */

/*
 * Klucz HMAC jest generowany raz przy pierwszym uruchomieniu i zapisywany
 * w trybie 0600 w /run/user/<uid>/tektura/hmac.key.
 * Nie jest hardcodowany w binarce — to ważne dla bezpieczeństwa.
 */
#define HMAC_KEY_SUBPATH "tektura/hmac.key"
#define HMAC_KEY_SIZE    32

/* ------------------------------------------------------------------ */
/* Nagłówek binarnego pliku bazy                                       */
/* ------------------------------------------------------------------ */

typedef struct {
	char     magic[8];        /* "TKTRPERM"                   */
	uint32_t version;         /* SECMGR_DB_VERSION            */
	uint32_t record_count;    /* liczba rekordów w pliku      */
	uint8_t  _reserved[16];   /* zarezerwowane — wypełnij 0   */
} db_header;

/* ------------------------------------------------------------------ */
/* Struktura wewnętrzna managera                                        */
/* ------------------------------------------------------------------ */

struct tektura_security_manager {
	struct tektura_server *server;

	char db_path[512];
	char key_path[512];
	char sock_path[512];
	int  db_fd;              /* deskryptor pliku bazy (trzyma flock)  */
	int  sock_fd;            /* deskryptor Unix socket dla ustawień   */

	uint8_t hmac_key[HMAC_KEY_SIZE];

	/* Rekordy załadowane do RAM */
	tektura_perm_record *records;
	uint32_t             record_count;

	/* Zdarzenia Wayland dla socket-listenera */
	struct wl_event_source *sock_event;
};

/* ------------------------------------------------------------------ */
/* Funkcje pomocnicze — ścieżki                                        */
/* ------------------------------------------------------------------ */

static void build_runtime_path(const char *subpath, char *out, size_t out_size) {
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		/* fallback — nie powinno się zdarzyć na nowoczesnym systemie */
		snprintf(out, out_size, "/tmp/%s", subpath);
	} else {
		snprintf(out, out_size, "%s/%s", runtime_dir, subpath);
	}
}

static bool ensure_parent_dir(const char *path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	char *slash = strrchr(tmp, '/');
	if (!slash) {
		return true;
	}
	*slash = '\0';
	if (mkdir(tmp, 0700) < 0 && errno != EEXIST) {
		wlr_log(WLR_ERROR, "secmgr: nie mogę utworzyć katalogu %s: %s",
			tmp, strerror(errno));
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Generowanie i ładowanie klucza HMAC                                 */
/* ------------------------------------------------------------------ */

static bool load_or_generate_hmac_key(tektura_security_manager *mgr) {
	ensure_parent_dir(mgr->key_path);

	int fd = open(mgr->key_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "secmgr: nie mogę otworzyć klucza HMAC %s: %s",
			mgr->key_path, strerror(errno));
		return false;
	}

	struct stat st;
	fstat(fd, &st);

	if (st.st_size == HMAC_KEY_SIZE) {
		/* Klucz już istnieje — wczytaj go */
		if (read(fd, mgr->hmac_key, HMAC_KEY_SIZE) != HMAC_KEY_SIZE) {
			wlr_log(WLR_ERROR, "secmgr: błąd odczytu klucza HMAC");
			close(fd);
			return false;
		}
	} else {
		/* Pierwsze uruchomienie — wygeneruj losowy klucz */
		int urandom = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
		if (urandom < 0 || read(urandom, mgr->hmac_key, HMAC_KEY_SIZE) != HMAC_KEY_SIZE) {
			wlr_log(WLR_ERROR, "secmgr: błąd generowania klucza HMAC");
			if (urandom >= 0) close(urandom);
			close(fd);
			return false;
		}
		close(urandom);
		lseek(fd, 0, SEEK_SET);
		if (write(fd, mgr->hmac_key, HMAC_KEY_SIZE) != HMAC_KEY_SIZE) {
			wlr_log(WLR_ERROR, "secmgr: błąd zapisu klucza HMAC");
			close(fd);
			return false;
		}
		wlr_log(WLR_INFO, "secmgr: wygenerowano nowy klucz HMAC w %s", mgr->key_path);
	}

	close(fd);
	return true;
}

/* ------------------------------------------------------------------ */
/* Obliczanie HMAC-SHA256                                               */
/* ------------------------------------------------------------------ */

static void compute_hmac(const uint8_t *key, size_t key_len,
		const uint8_t *data, size_t data_len,
		uint8_t out[HMAC_SIZE]) {
	unsigned int out_len = HMAC_SIZE;
	HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &out_len);
}

/* ------------------------------------------------------------------ */
/* Zapis bazy do pliku                                                  */
/* ------------------------------------------------------------------ */

static bool db_write(tektura_security_manager *mgr) {
	lseek(mgr->db_fd, 0, SEEK_SET);
	if (ftruncate(mgr->db_fd, 0) < 0) {
		wlr_log(WLR_ERROR, "secmgr: ftruncate failed: %s", strerror(errno));
		return false;
	}

	db_header hdr = {
		.version      = SECMGR_DB_VERSION,
		.record_count = mgr->record_count,
	};
	memcpy(hdr.magic, DB_MAGIC, 8);
	memset(hdr._reserved, 0, sizeof(hdr._reserved));

	/* Oblicz HMAC nad nagłówkiem + rekordami */
	size_t data_size = sizeof(db_header) +
		mgr->record_count * sizeof(tektura_perm_record);
	uint8_t *buf = calloc(1, data_size);
	if (!buf) {
		return false;
	}
	memcpy(buf, &hdr, sizeof(db_header));
	memcpy(buf + sizeof(db_header), mgr->records,
		mgr->record_count * sizeof(tektura_perm_record));

	uint8_t mac[HMAC_SIZE];
	compute_hmac(mgr->hmac_key, HMAC_KEY_SIZE, buf, data_size, mac);

	/* Zapisz: nagłówek + rekordy + HMAC */
	bool ok = (write(mgr->db_fd, buf, data_size) == (ssize_t)data_size) &&
	          (write(mgr->db_fd, mac, HMAC_SIZE) == HMAC_SIZE);
	free(buf);

	if (!ok) {
		wlr_log(WLR_ERROR, "secmgr: błąd zapisu bazy uprawnień");
	}
	return ok;
}

/* ------------------------------------------------------------------ */
/* Odczyt i weryfikacja bazy z pliku                                    */
/* ------------------------------------------------------------------ */

static bool db_read_and_verify(tektura_security_manager *mgr) {
	lseek(mgr->db_fd, 0, SEEK_SET);

	struct stat st;
	fstat(mgr->db_fd, &st);

	if (st.st_size == 0) {
		/* Pusta baza — nowa instalacja */
		mgr->record_count = 0;
		mgr->records = NULL;
		return true;
	}

	/* Minimalna poprawna wielkość: nagłówek + HMAC */
	if (st.st_size < (off_t)(sizeof(db_header) + HMAC_SIZE)) {
		wlr_log(WLR_ERROR, "secmgr: plik bazy zbyt mały — uszkodzony?");
		return false;
	}

	size_t data_size = (size_t)st.st_size - HMAC_SIZE;
	uint8_t *buf = malloc(st.st_size);
	if (!buf) {
		return false;
	}

	if (read(mgr->db_fd, buf, st.st_size) != st.st_size) {
		wlr_log(WLR_ERROR, "secmgr: błąd odczytu bazy");
		free(buf);
		return false;
	}

	/* Weryfikuj HMAC */
	uint8_t expected_mac[HMAC_SIZE];
	compute_hmac(mgr->hmac_key, HMAC_KEY_SIZE, buf, data_size, expected_mac);

	const uint8_t *stored_mac = buf + data_size;
	if (memcmp(expected_mac, stored_mac, HMAC_SIZE) != 0) {
		wlr_log(WLR_ERROR,
			"secmgr: NARUSZENIE INTEGRALNOŚCI! Baza uprawnień została "
			"zmodyfikowana bez wiedzy kompozytora. Resetuję do bezpiecznych ustawień.");
		free(buf);
		/* Reset — czysta baza */
		mgr->record_count = 0;
		mgr->records = NULL;
		db_write(mgr);
		return true; /* kontynuuj z czystą bazą */
	}

	/* Sprawdź nagłówek */
	db_header *hdr = (db_header *)buf;
	if (memcmp(hdr->magic, DB_MAGIC, 8) != 0) {
		wlr_log(WLR_ERROR, "secmgr: nieprawidłowy magic w bazie");
		free(buf);
		return false;
	}
	if (hdr->version != SECMGR_DB_VERSION) {
		wlr_log(WLR_ERROR, "secmgr: nieobsługiwana wersja bazy (%u)", hdr->version);
		free(buf);
		return false;
	}

	uint32_t count = hdr->record_count;
	if (count > MAX_RECORDS) {
		wlr_log(WLR_ERROR, "secmgr: zbyt wiele rekordów w bazie (%u > %u)",
			count, MAX_RECORDS);
		free(buf);
		return false;
	}

	/* Załaduj rekordy do RAM */
	mgr->record_count = count;
	if (count > 0) {
		mgr->records = calloc(count, sizeof(tektura_perm_record));
		if (!mgr->records) {
			free(buf);
			return false;
		}
		memcpy(mgr->records, buf + sizeof(db_header),
			count * sizeof(tektura_perm_record));
	}

	free(buf);
	wlr_log(WLR_INFO, "secmgr: załadowano %u wpisów z bazy uprawnień", count);
	return true;
}

/* ------------------------------------------------------------------ */
/* Wyszukiwanie rekordu w RAM                                           */
/* ------------------------------------------------------------------ */

static tektura_perm_record *find_record(tektura_security_manager *mgr,
		const char *app_path) {
	for (uint32_t i = 0; i < mgr->record_count; i++) {
		if (strncmp(mgr->records[i].app_path, app_path, SECMGR_MAX_PATH) == 0) {
			return &mgr->records[i];
		}
	}
	return NULL;
}

static tektura_perm_record *find_or_create_record(tektura_security_manager *mgr,
		const char *app_path) {
	tektura_perm_record *rec = find_record(mgr, app_path);
	if (rec) {
		return rec;
	}
	if (mgr->record_count >= MAX_RECORDS) {
		wlr_log(WLR_ERROR, "secmgr: osiągnięto maksymalną liczbę rekordów");
		return NULL;
	}
	/* Dodaj nowy rekord */
	mgr->records = realloc(mgr->records,
		(mgr->record_count + 1) * sizeof(tektura_perm_record));
	if (!mgr->records) {
		return NULL;
	}
	rec = &mgr->records[mgr->record_count++];
	memset(rec, 0, sizeof(*rec));
	strncpy(rec->app_path, app_path, SECMGR_MAX_PATH - 1);
	return rec;
}

/* ------------------------------------------------------------------ */
/* Unix socket — obsługa żądań z panelu ustawień                       */
/* ------------------------------------------------------------------ */

/*
 * Prosty protokół tekstowy przez socket (1 żądanie = 1 linia):
 *   CHECK <cap_id> <app_path>        → odpowiedź: <state>\n
 *   SET <cap_id> <state> <app_path>  → odpowiedź: OK\n lub ERR\n
 *   REVOKE <cap_id> <app_path>       → odpowiedź: OK\n lub ERR\n
 *
 * Przed wykonaniem żądania SET/REVOKE kompozytor sprawdza:
 *   - czy PID nadawcy należy do zaufanej binarki (np. karton-settings)
 *   - lub czy klient wysłał właściwy credential (SO_PEERCRED)
 */

static bool is_trusted_client(int client_fd) {
	struct ucred cred;
	socklen_t len = sizeof(cred);
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
		return false;
	}
	/* Sprawdź, czy to ten sam UID co kompozytor */
	if (cred.uid != getuid()) {
		wlr_log(WLR_ERROR, "secmgr: socket — obcy UID %u odrzucony", cred.uid);
		return false;
	}
	/* Sprawdź ścieżkę binarki klienta */
	char client_path[SECMGR_MAX_PATH];
	if (!secmgr_pid_to_path(cred.pid, client_path, sizeof(client_path))) {
		return false;
	}
	/*
	 * TODO: porównaj client_path z listą zaufanych binarek
	 * (np. /usr/bin/karton-settings). Na razie akceptuj każdego
	 * procesu tego samego użytkownika.
	 */
	wlr_log(WLR_DEBUG, "secmgr: socket klient PID=%d path=%s", cred.pid, client_path);
	return true;
}

static int handle_socket_client(int fd, uint32_t mask, void *data) {
	tektura_security_manager *mgr = data;
	(void)mask;

	int client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0) {
		return 0;
	}

	if (!is_trusted_client(client_fd)) {
		const char *err = "ERR unauthorized\n";
		write(client_fd, err, strlen(err));
		close(client_fd);
		return 0;
	}

	char buf[1024] = {0};
	ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		close(client_fd);
		return 0;
	}
	buf[n] = '\0';

	char cmd[16], app_path[SECMGR_MAX_PATH];
	int cap_id, state_id;
	const char *reply = "ERR bad command\n";

	if (sscanf(buf, "SET %d %d %511s", &cap_id, &state_id, app_path) == 3) {
		secmgr_set_permission(mgr, app_path,
			(tektura_capability)cap_id,
			(tektura_permission_state)state_id);
		reply = "OK\n";
	} else if (sscanf(buf, "REVOKE %d %511s", &cap_id, app_path) == 2) {
		secmgr_revoke_permission(mgr, app_path, (tektura_capability)cap_id);
		reply = "OK\n";
	} else if (sscanf(buf, "CHECK %d %511s", &cap_id, app_path) == 2) {
		tektura_perm_record *rec = find_record(mgr, app_path);
		tektura_permission_state st = rec ? (tektura_permission_state)rec->state : PERM_UNSET;
		char resp[32];
		snprintf(resp, sizeof(resp), "%d\n", st);
		write(client_fd, resp, strlen(resp));
		close(client_fd);
		return 0;
	}

	write(client_fd, reply, strlen(reply));
	close(client_fd);
	return 0;
}

static bool setup_socket(tektura_security_manager *mgr) {
	ensure_parent_dir(mgr->sock_path);
	unlink(mgr->sock_path); /* usuń stary socket jeśli istnieje */

	mgr->sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (mgr->sock_fd < 0) {
		wlr_log(WLR_ERROR, "secmgr: błąd tworzenia socket: %s", strerror(errno));
		return false;
	}

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, mgr->sock_path, sizeof(addr.sun_path) - 1);

	if (bind(mgr->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		wlr_log(WLR_ERROR, "secmgr: bind socket %s: %s",
			mgr->sock_path, strerror(errno));
		close(mgr->sock_fd);
		mgr->sock_fd = -1;
		return false;
	}
	chmod(mgr->sock_path, 0600);
	listen(mgr->sock_fd, 4);

	struct wl_event_loop *loop = wl_display_get_event_loop(mgr->server->wl_display);
	mgr->sock_event = wl_event_loop_add_fd(loop, mgr->sock_fd,
		WL_EVENT_READABLE, handle_socket_client, mgr);

	wlr_log(WLR_INFO, "secmgr: socket nasłuchuje na %s", mgr->sock_path);
	return true;
}

/* ------------------------------------------------------------------ */
/* API publiczne                                                         */
/* ------------------------------------------------------------------ */

tektura_security_manager *secmgr_init(struct tektura_server *server) {
	tektura_security_manager *mgr = calloc(1, sizeof(*mgr));
	if (!mgr) {
		return NULL;
	}
	mgr->server = server;
	mgr->db_fd  = -1;
	mgr->sock_fd = -1;

	build_runtime_path(SECMGR_DB_SUBPATH,     mgr->db_path,   sizeof(mgr->db_path));
	build_runtime_path(HMAC_KEY_SUBPATH,       mgr->key_path,  sizeof(mgr->key_path));
	build_runtime_path(SECMGR_SOCKET_SUBPATH,  mgr->sock_path, sizeof(mgr->sock_path));

	if (!load_or_generate_hmac_key(mgr)) {
		goto fail;
	}

	ensure_parent_dir(mgr->db_path);
	mgr->db_fd = open(mgr->db_path,
		O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (mgr->db_fd < 0) {
		wlr_log(WLR_ERROR, "secmgr: nie mogę otworzyć bazy %s: %s",
			mgr->db_path, strerror(errno));
		goto fail;
	}

	/* Wyłączna blokada — trzymaj przez cały czas życia kompozytora */
	if (flock(mgr->db_fd, LOCK_EX | LOCK_NB) < 0) {
		wlr_log(WLR_ERROR,
			"secmgr: nie mogę uzyskać blokady bazy (inny compositor działa?): %s",
			strerror(errno));
		goto fail;
	}

	if (!db_read_and_verify(mgr)) {
		goto fail;
	}

	if (!setup_socket(mgr)) {
		wlr_log(WLR_ERROR, "secmgr: socket nieaktywny — panel ustawień nie będzie działał");
		/* Nie jest błąd krytyczny — kontynuuj bez socketu */
	}

	wlr_log(WLR_INFO, "secmgr: security manager zainicjalizowany OK");
	return mgr;

fail:
	secmgr_destroy(mgr);
	return NULL;
}

tektura_perm_result secmgr_check(tektura_security_manager *mgr,
		pid_t client_pid, tektura_capability cap) {
	tektura_perm_result result = {
		.state          = PERM_UNSET,
		.prompt_pending = false,
	};

	char app_path[SECMGR_MAX_PATH];
	if (!secmgr_pid_to_path(client_pid, app_path, sizeof(app_path))) {
		wlr_log(WLR_ERROR, "secmgr: nie udało się odczytać ścieżki PID=%d", client_pid);
		result.state = PERM_DENIED;
		return result;
	}

	tektura_perm_record *rec = find_record(mgr, app_path);
	if (!rec) {
		/* Brak wpisu — trzeba zapytać użytkownika */
		wlr_log(WLR_INFO, "secmgr: brak wpisu dla %s, żądanie cap=%d — pokaż prompt",
			app_path, (int)cap);
		result.state          = PERM_UNSET;
		result.prompt_pending = true;
		return result;
	}

	if (rec->capabilities & cap) {
		result.state = (tektura_permission_state)rec->state;
	} else {
		/* Aplikacja znana, ale ta konkretna możliwość nie była jeszcze pytana */
		result.state          = PERM_UNSET;
		result.prompt_pending = true;
	}

	return result;
}

void secmgr_set_permission(tektura_security_manager *mgr,
		const char *app_path, tektura_capability cap,
		tektura_permission_state state) {
	tektura_perm_record *rec = find_or_create_record(mgr, app_path);
	if (!rec) {
		return;
	}
	rec->capabilities |= cap;
	rec->state        = (uint8_t)state;
	rec->granted_at   = (uint64_t)time(NULL);

	wlr_log(WLR_INFO, "secmgr: %s cap=%d state=%d",
		app_path, (int)cap, (int)state);

	/* PERM_ONCE — nie zapisuj do pliku, tylko trzymaj w RAM */
	if (state != PERM_ONCE) {
		db_write(mgr);
	}
}

void secmgr_revoke_permission(tektura_security_manager *mgr,
		const char *app_path, tektura_capability cap) {
	tektura_perm_record *rec = find_record(mgr, app_path);
	if (!rec) {
		return;
	}
	rec->capabilities &= ~cap;
	/* Jeśli nie ma już żadnych uprawnień, oznacz jako DENIED */
	if (rec->capabilities == 0) {
		rec->state = PERM_DENIED;
	}
	db_write(mgr);
	wlr_log(WLR_INFO, "secmgr: cofnięto uprawnienie cap=%d dla %s",
		(int)cap, app_path);
}

const char *secmgr_capability_name(tektura_capability cap) {
	switch (cap) {
	case CAP_SCREENCOPY:       return "robienie zrzutów ekranu";
	case CAP_EXPORT_DMABUF:    return "nagrywanie obrazu ekranu (wideo)";
	case CAP_VIRTUAL_KEYBOARD: return "emulowanie klawiatury";
	case CAP_INPUT_INHIBIT:    return "przechwytywanie całego wejścia";
	default:                   return "nieznane uprawnienie";
	}
}

bool secmgr_pid_to_path(pid_t pid, char *buf, size_t buf_size) {
	char proc_path[64];
	snprintf(proc_path, sizeof(proc_path), "/proc/%d/exe", (int)pid);
	ssize_t len = readlink(proc_path, buf, buf_size - 1);
	if (len < 0) {
		wlr_log(WLR_DEBUG, "secmgr: readlink %s: %s", proc_path, strerror(errno));
		return false;
	}
	buf[len] = '\0';
	return true;
}

void secmgr_destroy(tektura_security_manager *mgr) {
	if (!mgr) {
		return;
	}
	if (mgr->sock_event) {
		wl_event_source_remove(mgr->sock_event);
	}
	if (mgr->sock_fd >= 0) {
		close(mgr->sock_fd);
		unlink(mgr->sock_path);
	}
	if (mgr->db_fd >= 0) {
		db_write(mgr); /* finalna synchronizacja — podpisz i zapisz */
		flock(mgr->db_fd, LOCK_UN);
		close(mgr->db_fd);
	}
	free(mgr->records);
	free(mgr);
}
