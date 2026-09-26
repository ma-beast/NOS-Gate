#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "http_win32.h"

#define GATE_PORT 8080
#define GATE_WORKER_LIMIT 8
#define REQUEST_SIZE 8192
#define MAX_REDIRECTS 6
#define MAX_COOKIES 64
#define COOKIE_FILE_VERSION 1
#define MODE_ORIGINAL 0
#define MODE_LIGHT 1
#define MODE_SUPERLITE 2
#define MAX_IMAGE_RETRY_HOSTS 16
#define MAX_MEDIA_SOURCES 12
#define MAX_MEDIA_ITEMS 8
#define MAX_IFRAME_REFERERS 32
#define MEDIA_SOURCE_MP4 1
#define MEDIA_SOURCE_HLS 2
#define HLS_KIND_UNKNOWN 0
#define HLS_KIND_MUXED 1
#define HLS_KIND_VIDEO_ONLY 2

#ifdef _MSC_VER
typedef unsigned __int64 cookie_time_t;
#else
typedef unsigned long long cookie_time_t;
#endif

static volatile int gate_running = 1;
static HANDLE gate_worker_slots;
static volatile LONG gate_active_workers = 0;
static SOCKET gate_server_socket = INVALID_SOCKET;
static char gate_log_path[MAX_PATH];
static char gate_player_log_path[MAX_PATH];
static char gate_ini_path[MAX_PATH];
static char gate_cookie_path[MAX_PATH];
static char last_page_url[4096];
static int last_page_mode = MODE_ORIGINAL;
static CRITICAL_SECTION gate_state_lock;
static int gate_state_lock_ready = 0;

typedef struct media_source {
    int used;
    int type;
    int quality;
    int hls_kind;
    char label[64];
    char url[4096];
    char audio_url[4096];
} media_source;

typedef struct hls_audio_group {
    char id[128];
    char url[4096];
} hls_audio_group;

typedef struct media_catalog {
    int used;
    char key[4096];
    char title[512];
    char provider[64];
    char referer[4096];
    media_source source[MAX_MEDIA_SOURCES];
    int source_count;
    int selected_source;
} media_catalog;

static media_catalog media_items[MAX_MEDIA_ITEMS];
static int media_replace_cursor;

typedef struct iframe_referer_entry {
    int used;
    unsigned long sequence;
    char url[4096];
    char parent[4096];
} iframe_referer_entry;

static iframe_referer_entry iframe_referers[MAX_IFRAME_REFERERS];
static unsigned long iframe_referer_sequence = 1;

typedef struct cookie_entry {
    int used;
    char name[128];
    char value[1024];
    char domain[256];
    char path[512];
    cookie_time_t expires;
    unsigned long created;
    unsigned char host_only;
    unsigned char secure;
    unsigned char http_only;
    unsigned char same_site;
} cookie_entry;

static cookie_entry cookie_jar[MAX_COOKIES];
static unsigned long cookie_sequence = 1;

typedef struct image_retry_host {
    char host[256];
    int rejected_webp_jpg;
    int disabled;
} image_retry_host;

static image_retry_host image_retry_hosts[MAX_IMAGE_RETRY_HOSTS];

static void module_side_path(char *path, size_t path_size, const char *name);
static void serve_error(SOCKET s, const char *message);
static void serve_frame_url(SOCKET s, const char *url, int mode);
static int prepare_routed_target(const char *source, char *target,
                                 size_t target_size);

static void remember_iframe_referer(const char *url, const char *parent)
{
    int i, slot = -1, oldest_slot = -1;
    unsigned long oldest = 0xFFFFFFFFUL;
    if (gate_state_lock_ready) EnterCriticalSection(&gate_state_lock);
    if (!url || !url[0] || !parent || !parent[0]) do { if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock); return; } while (0);
    for (i = 0; i < MAX_IFRAME_REFERERS; ++i) {
        if (iframe_referers[i].used &&
            !_stricmp(iframe_referers[i].url, url)) {
            slot = i; break;
        }
        if (!iframe_referers[i].used && slot < 0) slot = i;
        else if (iframe_referers[i].used &&
                 iframe_referers[i].sequence < oldest) {
            oldest = iframe_referers[i].sequence; oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;
    if (slot < 0) do { if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock); return; } while (0);
    iframe_referers[slot].used = 1;
    iframe_referers[slot].sequence = iframe_referer_sequence++;
    strncpy(iframe_referers[slot].url, url,
            sizeof(iframe_referers[slot].url) - 1);
    iframe_referers[slot].url[sizeof(iframe_referers[slot].url) - 1] = 0;
    strncpy(iframe_referers[slot].parent, parent,
            sizeof(iframe_referers[slot].parent) - 1);
    iframe_referers[slot].parent[sizeof(iframe_referers[slot].parent) - 1] = 0;
    if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock);
}

static int iframe_referer_for(const char *url, char *referer,
                              size_t referer_size)
{
    int i;
    if (gate_state_lock_ready) EnterCriticalSection(&gate_state_lock);
    if (!referer_size) do { if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock); return 0; } while (0);
    referer[0] = 0;
    for (i = 0; i < MAX_IFRAME_REFERERS; ++i) {
        if (iframe_referers[i].used &&
            !_stricmp(iframe_referers[i].url, url)) {
            strncpy(referer, iframe_referers[i].parent, referer_size - 1);
            referer[referer_size - 1] = 0;
            iframe_referers[i].sequence = iframe_referer_sequence++;
            do { if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock); return 1; } while (0);
        }
    }
    do { if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock); return 0; } while (0);
}

#ifdef NOS_GATE_PARSER_TEST
static void log_line(const char *kind, const char *text)
{
    /* 0.5.1 Final: release logging disabled. */
    (void)kind; (void)text;
    return;

    /* 0.4.00 STABLE: logging retained but disabled. Set to 1 for diagnostics. */
    static int logging_enabled = 0;
    if (!logging_enabled) return;

    printf("[%s] %s\n", kind, text ? text : "");
}
#else
static void log_line(const char *kind, const char *text)
{
    SYSTEMTIME t;
    char line[4608];
    HANDLE file;
    DWORD written;

    /* 0.5.1 Final: release logging disabled; implementation retained below. */
    (void)kind;
    (void)text;
    return;

    if (!gate_log_path[0]) return;
    GetLocalTime(&t);
    sprintf(line, "%04u-%02u-%02u %02u:%02u:%02u [PID %lu] [%s] %s\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        (unsigned long)GetCurrentProcessId(), kind, text ? text : "");
    file = CreateFile(gate_log_path, GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    SetFilePointer(file, 0, NULL, FILE_END);
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}
#endif

static void open_log(void)
{
    char path[MAX_PATH], exe[MAX_PATH], note[MAX_PATH * 2 + 64], *slash;
    DWORD written;

    /* 0.5.1 Final: logging retained in source, disabled for normal release. */
    return;
    if (!GetModuleFileName(NULL, path, sizeof(path))) {
        strcpy(path, "nos-gate.log"); strcpy(exe, "NOS-Gate.exe");
    }
    else {
        strcpy(exe, path);
        slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "nos-gate.log");
        else strcpy(path, "nos-gate.log");
    }
    strcpy(gate_log_path, path);
    module_side_path(gate_ini_path, sizeof(gate_ini_path), "nos-gate.ini");
    module_side_path(gate_cookie_path, sizeof(gate_cookie_path), "nos-gate-cookies.dat");
    module_side_path(gate_player_log_path, sizeof(gate_player_log_path),
                     "nos-gate-player.log");
    {
        HANDLE file = CreateFile(gate_log_path, GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            SetFilePointer(file, 0, NULL, FILE_END);
            WriteFile(file, "\r\n", 2, &written, NULL);
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }
    log_line("START", "NOS-Gate 0.5.1 Final");
    log_line("UA", "desktop Edge 96 on Windows 10");
    sprintf(note, "EXE %.255s | LOG %.255s", exe, path);
    log_line("PATH", note);
}

static void module_side_path(char *path, size_t path_size, const char *name)
{
    char *slash;
    if (!GetModuleFileName(NULL, path, (DWORD)path_size)) {
        strncpy(path, name, path_size - 1); path[path_size - 1] = 0; return;
    }
    slash = strrchr(path, '\\');
    if (slash) {
        strncpy(slash + 1, name, path_size - (size_t)(slash + 1 - path) - 1);
        path[path_size - 1] = 0;
    } else {
        strncpy(path, name, path_size - 1); path[path_size - 1] = 0;
    }
}

static void save_error_body(const char *body, size_t body_size)
{
    char path[MAX_PATH];
    FILE *file;
    module_side_path(path, sizeof(path), "nos-gate-error.html");
    file = fopen(path, "wb");
    if (!file) { log_line("DIAG", "cannot save nos-gate-error.html"); return; }
    fwrite(body, 1, body_size, file); fclose(file);
    log_line("DIAG", "saved nos-gate-error.html");
}

static int send_all(SOCKET s, const char *data, size_t length)
{
    int count;
    while (length) {
        count = send(s, data, length > 30000 ? 30000 : (int)length, 0);
        if (count <= 0) return 0;
        data += count; length -= (size_t)count;
    }
    return 1;
}

static void send_response(SOCKET s, const char *status, const char *type,
                          const char *body, size_t length)
{
    char header[512];
    sprintf(header,
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Connection: close\r\nCache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\nExpires: 0\r\n\r\n",
        status, type, (unsigned long)length);
    send_all(s, header, strlen(header));
    if (length) send_all(s, body, length);
}

static void send_html(SOCKET s, const char *html)
{
    send_response(s, "200 OK", "text/html; charset=windows-1251",
                  html, strlen(html));
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *out, size_t out_size, const char *in)
{
    size_t n = 0;
    int a, b;
    while (*in && n + 1 < out_size) {
        if (*in == '+' ) { out[n++] = ' '; ++in; }
        else if (*in == '%' && (a = hex_value((unsigned char)in[1])) >= 0 &&
                 (b = hex_value((unsigned char)in[2])) >= 0) {
            out[n++] = (char)((a << 4) | b); in += 3;
        } else out[n++] = *in++;
    }
    out[n] = 0;
}

static size_t encoded_length(const char *s)
{
    size_t n = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        n += (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') ? 1 : 3;
    }
    return n;
}

static char *url_encode_to(char *out, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') *out++ = (char)c;
        else { *out++ = '%'; *out++ = hex[c >> 4]; *out++ = hex[c & 15]; }
    }
    return out;
}

static int starts_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s++) != tolower((unsigned char)*prefix++)) return 0;
    }
    return 1;
}

static int url_host(const char *url, char *host, size_t host_size)
{
    const char *p, *end;
    size_t length;
    if (starts_ci(url, "http://")) p = url + 7;
    else if (starts_ci(url, "https://")) p = url + 8;
    else return 0;
    end = strchr(p, '/');
    if (!end) end = p + strlen(p);
    length = (size_t)(end - p);
    if (!length || length >= host_size) return 0;
    memcpy(host, p, length); host[length] = 0;
    return 1;
}

static cookie_time_t current_cookie_time(void)
{
    time_t now = time(NULL);
    return now < 0 ? 0 : (cookie_time_t)now;
}

static void lower_ascii(char *text)
{
    while (*text) { *text = (char)tolower((unsigned char)*text); ++text; }
}

static int cookie_url_parts(const char *url, char *host, size_t host_size,
                            char *path, size_t path_size, int *secure)
{
    const char *p, *slash, *host_end, *colon;
    size_t n;
    if (starts_ci(url, "https://")) { p = url + 8; *secure = 1; }
    else if (starts_ci(url, "http://")) { p = url + 7; *secure = 0; }
    else return 0;
    slash = strchr(p, '/');
    host_end = slash ? slash : p + strlen(p);
    colon = memchr(p, ':', (size_t)(host_end - p));
    if (colon) host_end = colon;
    n = (size_t)(host_end - p);
    if (!n || n >= host_size) return 0;
    memcpy(host, p, n); host[n] = 0; lower_ascii(host);
    if (!slash) strcpy(path, "/");
    else {
        const char *query = strchr(slash, '?');
        n = query ? (size_t)(query - slash) : strlen(slash);
        if (!n) n = 1;
        if (n >= path_size) n = path_size - 1;
        memcpy(path, slash, n); path[n] = 0;
    }
    return 1;
}

static int cookie_domain_matches(const char *host, const cookie_entry *cookie)
{
    size_t hn, dn;
    if (cookie->host_only) return !_stricmp(host, cookie->domain);
    hn = strlen(host); dn = strlen(cookie->domain);
    if (hn < dn || _stricmp(host + hn - dn, cookie->domain)) return 0;
    return hn == dn || host[hn - dn - 1] == '.';
}

static int cookie_path_matches(const char *request_path, const char *cookie_path)
{
    size_t n = strlen(cookie_path);
    if (!n || strncmp(request_path, cookie_path, n)) return 0;
    return request_path[n] == 0 || cookie_path[n - 1] == '/' ||
           request_path[n] == '/';
}

static int month_number(const char *month)
{
    static const char *names[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec" };
    int i;
    for (i = 0; i < 12; ++i) if (!_strnicmp(month, names[i], 3)) return i + 1;
    return 0;
}

static int leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int parse_cookie_date(const char *text, cookie_time_t *result)
{
    char work[192], *token;
    int day = 0, month = 0, year = 0, hour = 0, minute = 0, second = 0;
    cookie_time_t days = 0;
    int y, m;
    static const int month_days[] = { 31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31 };
    size_t i;
    strncpy(work, text, sizeof(work) - 1); work[sizeof(work) - 1] = 0;
    for (i = 0; work[i]; ++i)
        if (work[i] == ',' || work[i] == '-' || work[i] == '\t') work[i] = ' ';
    token = strtok(work, " ");
    while (token) {
        int number, parsed_month;
        if (strchr(token, ':')) sscanf(token, "%d:%d:%d", &hour, &minute, &second);
        else if ((parsed_month = month_number(token)) != 0) month = parsed_month;
        else if (isdigit((unsigned char)token[0])) {
            number = atoi(token);
            if (strlen(token) >= 3 || number > 31) year = number;
            else if (!day) day = number;
            else if (!year) year = number;
        }
        token = strtok(NULL, " ");
    }
    if (year > 0 && year < 70) year += 2000;
    else if (year >= 70 && year < 100) year += 1900;
    if (!day || !month || year < 1970 || month > 12 || day > 31) {
        *result = 1; return year > 0;
    }
    for (y = 1970; y < year; ++y) days += leap_year(y) ? 366 : 365;
    for (m = 1; m < month; ++m)
        days += month_days[m - 1] + (m == 2 && leap_year(year) ? 1 : 0);
    days += (cookie_time_t)(day - 1);
    *result = days * (cookie_time_t)86400 + (cookie_time_t)hour * 3600 +
              (cookie_time_t)minute * 60 + (cookie_time_t)second;
    return 1;
}

typedef struct cookie_file_header {
    char magic[12];
    unsigned long version;
    unsigned long entry_size;
    unsigned long count;
    unsigned long sequence;
} cookie_file_header;

static void save_cookies(void)
{
    FILE *file;
    cookie_file_header header;
    int i;
    memset(&header, 0, sizeof(header));
    strcpy(header.magic, "NOSCOOKIE3");
    header.version = COOKIE_FILE_VERSION;
    header.entry_size = sizeof(cookie_entry);
    header.sequence = cookie_sequence;
    for (i = 0; i < MAX_COOKIES; ++i) if (cookie_jar[i].used) ++header.count;
    file = fopen(gate_cookie_path, "wb");
    if (!file) { log_line("COOKIE-FILE", "save failed"); return; }
    fwrite(&header, sizeof(header), 1, file);
    for (i = 0; i < MAX_COOKIES; ++i)
        if (cookie_jar[i].used) fwrite(&cookie_jar[i], sizeof(cookie_entry), 1, file);
    fclose(file);
}

static void load_cookies(void)
{
    FILE *file;
    cookie_file_header header;
    cookie_entry item;
    unsigned long i, loaded = 0;
    cookie_time_t now = current_cookie_time();
    file = fopen(gate_cookie_path, "rb");
    if (!file) { log_line("COOKIE-FILE", "new empty jar"); return; }
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        strcmp(header.magic, "NOSCOOKIE3") ||
        header.version != COOKIE_FILE_VERSION ||
        header.entry_size != sizeof(cookie_entry) || header.count > 4096) {
        fclose(file); log_line("COOKIE-FILE", "invalid file ignored"); return;
    }
    for (i = 0; i < header.count && loaded < MAX_COOKIES; ++i) {
        if (fread(&item, sizeof(item), 1, file) != 1) break;
        if (item.used && (!item.expires || item.expires > now))
            cookie_jar[loaded++] = item;
    }
    fclose(file);
    cookie_sequence = header.sequence > 0 ? header.sequence : 1;
    {
        char note[96]; sprintf(note, "loaded %lu cookie(s)", loaded);
        log_line("COOKIE-FILE", note);
    }
}

static void clear_cookies(void)
{
    memset(cookie_jar, 0, sizeof(cookie_jar));
    cookie_sequence = 1;
    save_cookies();
    log_line("COOKIE-FILE", "all cookies cleared");
}

static int cookies_for(const char *url, char *header, size_t header_size)
{
    char host[256], path[512];
    int secure, indexes[MAX_COOKIES], count = 0, i, j, chosen;
    size_t used = 0;
    cookie_time_t now = current_cookie_time();
    if (!header || !header_size) return 0;
    header[0] = 0;
    if (gate_state_lock_ready) EnterCriticalSection(&gate_state_lock);
    if (!cookie_url_parts(url, host, sizeof(host), path, sizeof(path), &secure)) do { if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock); return 0; } while (0);
    for (i = 0; i < MAX_COOKIES; ++i) {
        if (!cookie_jar[i].used) continue;
        if (cookie_jar[i].expires && cookie_jar[i].expires <= now) {
            cookie_jar[i].used = 0; continue;
        }
        if ((!cookie_jar[i].secure || secure) &&
            cookie_domain_matches(host, &cookie_jar[i]) &&
            cookie_path_matches(path, cookie_jar[i].path)) indexes[count++] = i;
    }
    for (i = 0; i < count; ++i) {
        chosen = i;
        for (j = i + 1; j < count; ++j) {
            size_t a = strlen(cookie_jar[indexes[j]].path);
            size_t b = strlen(cookie_jar[indexes[chosen]].path);
            if (a > b || (a == b && cookie_jar[indexes[j]].created <
                         cookie_jar[indexes[chosen]].created)) chosen = j;
        }
        if (chosen != i) { j = indexes[i]; indexes[i] = indexes[chosen]; indexes[chosen] = j; }
    }
    header[0] = 0;
    for (i = 0; i < count; ++i) {
        cookie_entry *c = &cookie_jar[indexes[i]];
        size_t need = strlen(c->name) + strlen(c->value) + 3;
        if (used + need >= header_size) continue;
        if (used) { header[used++] = ';'; header[used++] = ' '; }
        strcpy(header + used, c->name); used += strlen(c->name);
        header[used++] = '='; strcpy(header + used, c->value); used += strlen(c->value);
    }
    header[used] = 0;
    if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock);
    return header[0] != 0;
}

static int cookie_pair_count(const char *value)
{
    int count = 0;
    if (value && value[0]) { count = 1; while (*value) if (*value++ == ';') ++count; }
    return count;
}

static int valid_cookie_domain(const char *request_host, const char *domain)
{
    size_t hn = strlen(request_host), dn = strlen(domain);
    if (hn < dn || _stricmp(request_host + hn - dn, domain)) return 0;
    return hn == dn || request_host[hn - dn - 1] == '.';
}

static void default_cookie_path(const char *request_path, char *out, size_t out_size)
{
    const char *last = strrchr(request_path, '/');
    size_t n;
    if (!last || last == request_path) { strcpy(out, "/"); return; }
    n = (size_t)(last - request_path);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, request_path, n); out[n] = 0;
}

static void store_one_cookie(const char *url, const char *line, size_t line_length)
{
    char request_host[256], request_path[512], work[4096];
    char *part, *semi, *equals, *attribute, *attribute_value;
    cookie_entry incoming;
    int request_secure, i, slot = -1, oldest = -1, deleting = 0, existed = 0;
    int invalid_domain = 0;
    unsigned long oldest_created = 0xFFFFFFFFUL;
    cookie_time_t now = current_cookie_time();
    char note[512];
    if (!line_length || line_length >= sizeof(work) ||
        !cookie_url_parts(url, request_host, sizeof(request_host),
                          request_path, sizeof(request_path), &request_secure)) return;
    memcpy(work, line, line_length); work[line_length] = 0;
    memset(&incoming, 0, sizeof(incoming)); incoming.used = 1;
    strcpy(incoming.domain, request_host); incoming.host_only = 1;
    default_cookie_path(request_path, incoming.path, sizeof(incoming.path));
    part = work; semi = strchr(part, ';'); if (semi) *semi = 0;
    equals = strchr(part, '='); if (!equals || equals == part) return;
    *equals++ = 0;
    while (*part == ' ' || *part == '\t') ++part;
    while (strlen(part) && (part[strlen(part)-1] == ' ' || part[strlen(part)-1] == '\t'))
        part[strlen(part)-1] = 0;
    while (*equals == ' ' || *equals == '\t') ++equals;
    while (strlen(equals) && (equals[strlen(equals)-1] == ' ' || equals[strlen(equals)-1] == '\t'))
        equals[strlen(equals)-1] = 0;
    if (!part[0] || strlen(part) >= sizeof(incoming.name) ||
        strlen(equals) >= sizeof(incoming.value)) return;
    strcpy(incoming.name, part); strcpy(incoming.value, equals);
    part = semi ? semi + 1 : NULL;
    while (part && *part) {
        while (*part == ' ' || *part == '\t' || *part == ';') ++part;
        if (!*part) break;
        semi = strchr(part, ';'); if (semi) *semi = 0;
        attribute = part; attribute_value = strchr(attribute, '=');
        if (attribute_value) {
            *attribute_value++ = 0;
            while (*attribute_value == ' ' || *attribute_value == '\t') ++attribute_value;
            while (strlen(attribute_value) &&
                   (attribute_value[strlen(attribute_value)-1] == ' ' ||
                    attribute_value[strlen(attribute_value)-1] == '\t'))
                attribute_value[strlen(attribute_value)-1] = 0;
        }
        while (strlen(attribute) && attribute[strlen(attribute)-1] == ' ')
            attribute[strlen(attribute)-1] = 0;
        if (!_stricmp(attribute, "domain") && attribute_value && attribute_value[0]) {
            while (*attribute_value == '.') ++attribute_value;
            lower_ascii(attribute_value);
            if (valid_cookie_domain(request_host, attribute_value) &&
                strchr(attribute_value, '.') != NULL &&
                strlen(attribute_value) < sizeof(incoming.domain)) {
                strcpy(incoming.domain, attribute_value); incoming.host_only = 0;
            } else invalid_domain = 1;
        } else if (!_stricmp(attribute, "path") && attribute_value &&
                   attribute_value[0] == '/' && strlen(attribute_value) < sizeof(incoming.path))
            strcpy(incoming.path, attribute_value);
        else if (!_stricmp(attribute, "secure")) incoming.secure = 1;
        else if (!_stricmp(attribute, "httponly")) incoming.http_only = 1;
        else if (!_stricmp(attribute, "samesite") && attribute_value) {
            if (!_stricmp(attribute_value, "strict")) incoming.same_site = 1;
            else if (!_stricmp(attribute_value, "lax")) incoming.same_site = 2;
            else if (!_stricmp(attribute_value, "none")) incoming.same_site = 3;
        } else if (!_stricmp(attribute, "max-age") && attribute_value) {
            char *number_end;
            long seconds = strtol(attribute_value, &number_end, 10);
            if (number_end != attribute_value && !*number_end) {
                if (seconds <= 0) deleting = 1;
                else incoming.expires = now + (cookie_time_t)seconds;
            }
        } else if (!_stricmp(attribute, "expires") && attribute_value && !incoming.expires) {
            if (parse_cookie_date(attribute_value, &incoming.expires) && incoming.expires <= now)
                deleting = 1;
        }
        part = semi ? semi + 1 : NULL;
    }
    if (invalid_domain ||
        (!strncmp(incoming.name, "__Secure-", 9) && (!request_secure || !incoming.secure)) ||
        (!strncmp(incoming.name, "__Host-", 7) &&
         (!request_secure || !incoming.secure || !incoming.host_only ||
          strcmp(incoming.path, "/"))) ||
        (incoming.same_site == 3 && !incoming.secure)) {
        sprintf(note, "rejected %.120s: invalid cookie attributes", incoming.name);
        log_line("COOKIE", note); return;
    }
    for (i = 0; i < MAX_COOKIES; ++i) {
        if (cookie_jar[i].used && !strcmp(cookie_jar[i].name, incoming.name) &&
            !_stricmp(cookie_jar[i].domain, incoming.domain) &&
            !strcmp(cookie_jar[i].path, incoming.path)) { slot = i; existed = 1; break; }
        if (!cookie_jar[i].used && slot < 0) slot = i;
        if (cookie_jar[i].used && cookie_jar[i].created < oldest_created) {
            oldest_created = cookie_jar[i].created; oldest = i;
        }
    }
    if (deleting) {
        if (slot >= 0 && cookie_jar[slot].used) cookie_jar[slot].used = 0;
        sprintf(note, "deleted %.120s for %.200s%.120s", incoming.name,
                incoming.domain, incoming.path); log_line("COOKIE", note); return;
    }
    if (slot < 0) slot = oldest >= 0 ? oldest : 0;
    if (cookie_jar[slot].used) incoming.created = cookie_jar[slot].created;
    else incoming.created = cookie_sequence++;
    cookie_jar[slot] = incoming;
    sprintf(note, "%s %.120s for %.200s%.120s%s%s",
            existed ? "updated" : "stored",
            incoming.name, incoming.domain, incoming.path,
            incoming.secure ? " Secure" : "", incoming.http_only ? " HttpOnly" : "");
    log_line("COOKIE", note);
}

static void store_cookies(const char *url, const char *value, int received_count)
{
    const char *line, *end;
    int parsed = 0;
    char note[128];
    if (!value || !value[0]) return;
    if (gate_state_lock_ready) EnterCriticalSection(&gate_state_lock);
    line = value;
    while (*line) {
        end = strchr(line, '\n'); if (!end) end = line + strlen(line);
        store_one_cookie(url, line, (size_t)(end - line)); ++parsed;
        line = *end ? end + 1 : end;
    }
    save_cookies();
    sprintf(note, "received %d header(s), parsed %d", received_count, parsed);
    log_line("COOKIE", note);
    if (gate_state_lock_ready) LeaveCriticalSection(&gate_state_lock);
}

static void log_response_headers(const nw_http_result *result)
{
    char line[800];
    sprintf(line, "Server=%.240s | Via=%.160s | CF-Ray=%.100s | cf-mitigated=%.80s | Retry-After=%.80s | Set-Cookie=%d",
            result->server[0] ? result->server : "-",
            result->via[0] ? result->via : "-",
            result->cf_ray[0] ? result->cf_ray : "-",
            result->cf_mitigated[0] ? result->cf_mitigated : "-",
            result->retry_after[0] ? result->retry_after : "-",
            result->set_cookie_count);
    log_line("HEADERS", line);
}

static void decode_html_amp(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (starts_ci(r, "&amp;")) { *w++ = '&'; r += 5; }
        else if (starts_ci(r, "&#039;")) { *w++ = '\''; r += 6; }
        else if (starts_ci(r, "&quot;")) { *w++ = '"'; r += 6; }
        else *w++ = *r++;
    }
    *w = 0;
}

static int split_origin(const char *base, char *origin, size_t origin_size,
                        const char **path)
{
    const char *p, *slash;
    size_t n;
    if (starts_ci(base, "http://")) p = base + 7;
    else if (starts_ci(base, "https://")) p = base + 8;
    else return 0;
    slash = strchr(p, '/');
    if (!slash) slash = base + strlen(base);
    n = (size_t)(slash - base);
    if (n >= origin_size) return 0;
    memcpy(origin, base, n); origin[n] = 0;
    *path = slash;
    return 1;
}

static int resolve_url(const char *base, const char *value, char *out, size_t out_size)
{
    char origin[512];
    const char *base_path, *last;
    size_t n;
    if (!value[0] || value[0] == '#' || starts_ci(value, "javascript:") ||
        starts_ci(value, "mailto:") || starts_ci(value, "data:")) return 0;
    if (starts_ci(value, "http://") || starts_ci(value, "https://")) {
        if (strlen(value) >= out_size) return 0;
        strcpy(out, value); return 1;
    }
    if (!split_origin(base, origin, sizeof(origin), &base_path)) return 0;
    if (value[0] == '/' && value[1] == '/') {
        const char *colon = strchr(origin, ':');
        n = colon ? (size_t)(colon - origin) : 4;
        if (n + 1 + strlen(value) >= out_size) return 0;
        memcpy(out, origin, n); out[n] = ':'; strcpy(out + n + 1, value); return 1;
    }
    if (value[0] == '/') {
        if (strlen(origin) + strlen(value) >= out_size) return 0;
        strcpy(out, origin); strcat(out, value); return 1;
    }
    last = strrchr(base_path, '/');
    n = last ? (size_t)(last - base_path + 1) : 1;
    if (strlen(origin) + n + strlen(value) >= out_size) return 0;
    strcpy(out, origin); strncat(out, base_path, n); strcat(out, value);
    return 1;
}

static void unwrap_frogfind(char *url, size_t url_size)
{
    const char *prefix_http = "http://frogfind.com/read.php?a=";
    const char *prefix_https = "https://frogfind.com/read.php?a=";
    const char *wrapped = NULL;
    char decoded[4096];
    if (starts_ci(url, prefix_http)) wrapped = url + strlen(prefix_http);
    else if (starts_ci(url, prefix_https)) wrapped = url + strlen(prefix_https);
    if (!wrapped) return;
    url_decode(decoded, sizeof(decoded), wrapped);
    if ((starts_ci(decoded, "http://") || starts_ci(decoded, "https://")) &&
        strlen(decoded) < url_size)
        strcpy(url, decoded);
}

typedef struct html_attribute {
    const char *begin;
    const char *name_end;
    const char *value;
    const char *value_end;
    const char *end;
    int quote;
} html_attribute;

static int html_name_char(int c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':';
}

static int name_is(const char *begin, const char *end, const char *name)
{
    size_t n = (size_t)(end - begin);
    return strlen(name) == n && !_strnicmp(begin, name, n);
}

/* Parse one attribute.  Unlike the old substring search, this can never
   mistake the src part of data-src for a separate src attribute. */
static int next_html_attribute(const char **cursor, const char *tag_end,
                               html_attribute *attribute)
{
    const char *p = *cursor;
    while (p < tag_end && isspace((unsigned char)*p)) ++p;
    if (p >= tag_end || *p == '>' || *p == '/') { *cursor = p; return 0; }
    attribute->begin = p;
    while (p < tag_end && html_name_char((unsigned char)*p)) ++p;
    if (p == attribute->begin) { *cursor = p + 1; return -1; }
    attribute->name_end = p;
    while (p < tag_end && isspace((unsigned char)*p)) ++p;
    attribute->quote = 0;
    attribute->value = attribute->value_end = p;
    if (p < tag_end && *p == '=') {
        ++p;
        while (p < tag_end && isspace((unsigned char)*p)) ++p;
        if (p < tag_end && (*p == '\'' || *p == '"')) {
            attribute->quote = (unsigned char)*p++;
            attribute->value = p;
            while (p < tag_end && *p != attribute->quote) ++p;
            attribute->value_end = p;
            if (p < tag_end) ++p;
        } else {
            attribute->value = p;
            while (p < tag_end && !isspace((unsigned char)*p) && *p != '>') ++p;
            attribute->value_end = p;
        }
    }
    attribute->end = p;
    *cursor = p;
    return 1;
}

static int find_tag_attribute(const char *attributes, const char *tag_end,
                              const char *name, char *out, size_t out_size)
{
    const char *p = attributes;
    html_attribute a;
    int result;
    size_t n;
    while (p < tag_end) {
        result = next_html_attribute(&p, tag_end, &a);
        if (result < 0) continue;
        if (!result) break;
        if (!name_is(a.begin, a.name_end, name) || a.value == a.value_end) continue;
        n = (size_t)(a.value_end - a.value);
        if (n >= out_size) return 0;
        memcpy(out, a.value, n); out[n] = 0;
        decode_html_amp(out);
        return 1;
    }
    return 0;
}

static int background_url(const char *value, char *out, size_t out_size)
{
    const char *p = value, *close;
    size_t n;
    while (*p && !starts_ci(p, "http://") && !starts_ci(p, "https://")) ++p;
    if (!*p) return 0;
    close = p;
    while (*close && *close != '\'' && *close != '"' && *close != ')' &&
           *close != ' ' && *close != '\t') ++close;
    n = (size_t)(close - p);
    if (!n || n >= out_size) return 0;
    memcpy(out, p, n); out[n] = 0;
    return 1;
}

static int ensure_output(char **out, char **write, size_t *capacity, size_t need)
{
    size_t used = (size_t)(*write - *out);
    char *next;
    if (used + need + 1 < *capacity) return 1;
    while (used + need + 1 >= *capacity) *capacity = *capacity * 2 + 4096;
    next = (char *)realloc(*out, *capacity);
    if (!next) return 0;
    *out = next; *write = next + used;
    return 1;
}

static int append_output(char **out, char **write, size_t *capacity,
                         const char *text, size_t length)
{
    if (!ensure_output(out, write, capacity, length)) return 0;
    memcpy(*write, text, length); *write += length;
    return 1;
}

static int append_routed(char **out, char **write, size_t *capacity,
                         const char *absolute, const char *route)
{
    size_t route_length = strlen(route);
    size_t need = route_length + encoded_length(absolute);
    if (!ensure_output(out, write, capacity, need)) return 0;
    memcpy(*write, route, route_length); *write += route_length;
    *write = url_encode_to(*write, absolute);
    return 1;
}

static const char *mode_entry_route(int mode)
{
    if (mode == MODE_LIGHT) return "/light/";
    if (mode == MODE_SUPERLITE) return "/lite/";
    return "/go/";
}

static const char *mode_content_route(int mode)
{
    if (mode == MODE_LIGHT) return "/page/light/";
    if (mode == MODE_SUPERLITE) return "/page/lite/";
    return "/page/go/";
}

static const char *mode_name(int mode)
{
    if (mode == MODE_LIGHT) return "LITE";
    if (mode == MODE_SUPERLITE) return "SUPERLITE";
    return "ORIGINAL";
}

static int host_ends_with(const char *host, const char *suffix)
{
    size_t h = strlen(host), s = strlen(suffix);
    return h >= s && !_stricmp(host + h - s, suffix);
}

static int default_mode_for_url(const char *url)
{
    char host[256];
    if (!url_host(url, host, sizeof(host))) return MODE_LIGHT;
    if (host_ends_with(host, "flibusta.is")) return MODE_ORIGINAL;
    if (host_ends_with(host, "worldofspectrum.net") ||
        host_ends_with(host, "worldofspectrum.org")) return MODE_SUPERLITE;
    return MODE_LIGHT;
}

static int mode_key_for_url(const char *url, char *host, size_t host_size)
{
    char *port;
    if (!url_host(url, host, host_size)) return 0;
    if (!_strnicmp(host, "www.", 4)) memmove(host, host + 4, strlen(host + 4) + 1);
    port = strchr(host, ':');
    if (port) *port = 0;
    return host[0] != 0;
}

static int remembered_mode_for_url(const char *url)
{
    char host[256], value[32];
    int fallback = default_mode_for_url(url);
    if (!mode_key_for_url(url, host, sizeof(host)) || !gate_ini_path[0]) return fallback;
    GetPrivateProfileString("Modes", host, "", value, sizeof(value), gate_ini_path);
    if (!_stricmp(value, "original")) return MODE_ORIGINAL;
    if (!_stricmp(value, "light")) return MODE_LIGHT;
    if (!_stricmp(value, "superlite")) return MODE_SUPERLITE;
    return fallback;
}

static void remember_mode_for_url(const char *url, int mode)
{
    char host[256], note[320];
    const char *value;
    if (!mode_key_for_url(url, host, sizeof(host)) || !gate_ini_path[0]) return;
    value = mode == MODE_ORIGINAL ? "original" :
            (mode == MODE_SUPERLITE ? "superlite" : "light");
    WritePrivateProfileString("Modes", host, value, gate_ini_path);
    sprintf(note, "%.255s=%s", host, value);
    log_line("MODE-SAVE", note);
}

static const char *find_bounded(const char *begin, const char *end,
                                const char *needle)
{
    size_t n = strlen(needle);
    const char *p;
    if (!n || (size_t)(end - begin) < n) return NULL;
    for (p = begin; p + n <= end; ++p)
        if (!memcmp(p, needle, n)) return p;
    return NULL;
}

static const char *json_object_end(const char *open, const char *end)
{
    const char *p;
    int depth = 0, string = 0, escaped = 0;
    if (open >= end || *open != '{') return NULL;
    for (p = open; p < end; ++p) {
        unsigned char c = (unsigned char)*p;
        if (string) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') string = 0;
        } else if (c == '"') string = 1;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return p + 1;
    }
    return NULL;
}

static int utf8_put(char **write, char *end, unsigned long value)
{
    if (value <= 0x7F) {
        if (*write + 1 >= end) return 0;
        *(*write)++ = (char)value;
    } else if (value <= 0x7FF) {
        if (*write + 2 >= end) return 0;
        *(*write)++ = (char)(0xC0 | (value >> 6));
        *(*write)++ = (char)(0x80 | (value & 0x3F));
    } else {
        if (*write + 3 >= end) return 0;
        *(*write)++ = (char)(0xE0 | (value >> 12));
        *(*write)++ = (char)(0x80 | ((value >> 6) & 0x3F));
        *(*write)++ = (char)(0x80 | (value & 0x3F));
    }
    return 1;
}

static int json_string_value(const char *begin, const char *end,
                             const char *key, char *out, size_t out_size)
{
    const char *p, *q;
    char *w = out, *out_end = out + out_size;
    size_t key_length = strlen(key);
    int h1, h2, h3, h4;
    p = begin;
    while ((p = find_bounded(p, end, key)) != NULL) {
        q = p + key_length;
        while (q < end && isspace((unsigned char)*q)) ++q;
        if (q >= end || *q != ':') { p += key_length; continue; }
        ++q; while (q < end && isspace((unsigned char)*q)) ++q;
        if (q >= end || *q != '"') { p += key_length; continue; }
        ++q;
        while (q < end && *q != '"') {
            unsigned char c = (unsigned char)*q++;
            if (c == '\\' && q < end) {
                c = (unsigned char)*q++;
                if (c == 'n' || c == 'r') c = ' ';
                else if (c == 't') c = ' ';
                else if (c == 'b' || c == 'f') continue;
                else if (c == 'u' && q + 4 <= end &&
                         (h1 = hex_value((unsigned char)q[0])) >= 0 &&
                         (h2 = hex_value((unsigned char)q[1])) >= 0 &&
                         (h3 = hex_value((unsigned char)q[2])) >= 0 &&
                         (h4 = hex_value((unsigned char)q[3])) >= 0) {
                    unsigned long value = (unsigned long)((h1 << 12) |
                        (h2 << 8) | (h3 << 4) | h4);
                    q += 4;
                    if (!utf8_put(&w, out_end, value)) return 0;
                    continue;
                }
            }
            if (w + 1 >= out_end) return 0;
            *w++ = (char)c;
        }
        *w = 0;
        return q < end;
    }
    if (out_size) out[0] = 0;
    return 0;
}

static int json_field_string(const char *begin, const char *end,
                             const char *field, char *out, size_t out_size)
{
    const char *p, *open, *close;
    p = find_bounded(begin, end, field);
    if (!p) return 0;
    open = find_bounded(p + strlen(field), end, "{");
    if (!open) return 0;
    close = json_object_end(open, end);
    if (!close) return 0;
    if (json_string_value(open, close, "\"text\"", out, out_size)) return 1;
    return json_string_value(open, close, "\"simpleText\"", out, out_size);
}

static int append_html_escaped(char **out, char **write, size_t *capacity,
                               const char *text)
{
    const char *p = text, *start = text;
    const char *entity;
    size_t entity_length;
    while (*p) {
        entity = NULL; entity_length = 0;
        if (*p == '&') { entity = "&amp;"; entity_length = 5; }
        else if (*p == '<') { entity = "&lt;"; entity_length = 4; }
        else if (*p == '>') { entity = "&gt;"; entity_length = 4; }
        else if (*p == '"') { entity = "&quot;"; entity_length = 6; }
        if (entity) {
            if (!append_output(out, write, capacity, start,
                               (size_t)(p - start)) ||
                !append_output(out, write, capacity, entity, entity_length))
                return 0;
            start = p + 1;
        }
        ++p;
    }
    return append_output(out, write, capacity, start, (size_t)(p - start));
}

static media_catalog *media_catalog_for_key(const char *key, int *item_id);
static int media_catalog_add(media_catalog *catalog, int type, int quality,
                             const char *label, const char *url);

static char *youtube_results_html(const char *html, size_t length,
                                  const char *page_url, size_t *out_length)
{
    static const char page_begin[] =
        "<html><head><title>YouTube search - NOS-Gate</title>"
        "<base target=\"_top\"></head>"
        "<body bgcolor=\"#ffffff\" text=\"#000000\">";
    static const char search_begin[] =
        "<h2>YouTube search</h2>"
        "<form method=\"GET\" action=\"/lite/https%3A%2F%2Fwww.youtube.com%2Fresults\">"
        "<input name=\"search_query\" size=\"36\"> "
        "<input type=\"submit\" value=\"Search\"></form><hr>";
    static const char no_results[] =
        "<p>No static video results found. See nos-gate.log.</p>";
    static const char page_end[] =
        "<hr><p><a href=\"/\">NOS-Gate home</a></p></body></html>";
    const char *p = html, *end = html + length, *marker, *open, *close;
    char *out, *w, note[160], watch[512];
    char id[128], title[2048], channel[1024], duration[128];
    char views[256], published[256], thumbnail[4096];
    size_t capacity = 32768;
    int count = 0;
    out = (char *)malloc(capacity);
    if (!out) return NULL;
    w = out;
    if (!append_output(&out, &w, &capacity, page_begin,
                       sizeof(page_begin) - 1) ||
        !append_output(&out, &w, &capacity, search_begin,
                       sizeof(search_begin) - 1)) goto failed;
    while (count < 20 &&
           (marker = find_bounded(p, end, "\"videoRenderer\":")) != NULL) {
        open = find_bounded(marker + strlen("\"videoRenderer\":"), end, "{");
        if (!open || !(close = json_object_end(open, end))) break;
        id[0] = title[0] = channel[0] = duration[0] = 0;
        views[0] = published[0] = thumbnail[0] = 0;
        if (!json_string_value(open, close, "\"videoId\"", id, sizeof(id)) ||
            !json_field_string(open, close, "\"title\":", title, sizeof(title))) {
            p = close; continue;
        }
        json_field_string(open, close, "\"ownerText\":", channel, sizeof(channel));
        json_field_string(open, close, "\"lengthText\":", duration, sizeof(duration));
        json_field_string(open, close, "\"viewCountText\":", views, sizeof(views));
        json_field_string(open, close, "\"publishedTimeText\":", published, sizeof(published));
        sprintf(thumbnail, "https://i.ytimg.com/vi/%.120s/mqdefault.jpg", id);
        sprintf(watch, "https://www.youtube.com/watch?v=%.120s", id);
        if (!append_output(&out, &w, &capacity, "<p>", 3)) goto failed;
        if (thumbnail[0]) {
            if (!append_output(&out, &w, &capacity, "<a href=\"", 9) ||
                !append_routed(&out, &w, &capacity, watch, "/lite/") ||
                !append_output(&out, &w, &capacity, "\"><img src=\"", 12) ||
                !append_routed(&out, &w, &capacity, thumbnail, "/page/lite/") ||
                !append_output(&out, &w, &capacity,
                               "\" border=\"0\" alt=\"preview\"></a><br>", 35))
                goto failed;
        }
        if (!append_output(&out, &w, &capacity, "<b><a href=\"", 12) ||
            !append_routed(&out, &w, &capacity, watch, "/lite/") ||
            !append_output(&out, &w, &capacity, "\">", 2) ||
            !append_html_escaped(&out, &w, &capacity, title) ||
            !append_output(&out, &w, &capacity, "</a></b>", 8)) goto failed;
        if (duration[0] &&
            (!append_output(&out, &w, &capacity, " [", 2) ||
             !append_html_escaped(&out, &w, &capacity, duration) ||
             !append_output(&out, &w, &capacity, "]", 1))) goto failed;
        if (channel[0] &&
            (!append_output(&out, &w, &capacity, "<br>", 4) ||
             !append_html_escaped(&out, &w, &capacity, channel))) goto failed;
        if (views[0] &&
            (!append_output(&out, &w, &capacity, " - ", 3) ||
             !append_html_escaped(&out, &w, &capacity, views))) goto failed;
        if (published[0] &&
            (!append_output(&out, &w, &capacity, " - ", 3) ||
             !append_html_escaped(&out, &w, &capacity, published))) goto failed;
        if (!append_output(&out, &w, &capacity, "</p>\r\n", 6)) goto failed;
        ++count; p = close;
    }
    if (!count && !append_output(&out, &w, &capacity, no_results,
                                 sizeof(no_results) - 1))
        goto failed;
    if (!append_output(&out, &w, &capacity, page_end,
                       sizeof(page_end) - 1))
        goto failed;
    *w = 0; *out_length = (size_t)(w - out);
    sprintf(note, "extracted %d videoRenderer item(s), generated %d fixed JPEG thumbnail(s), %lu -> %lu bytes",
            count, count, (unsigned long)length, (unsigned long)*out_length);
    log_line("YOUTUBE-RESULTS", note);
    (void)page_url;
    return out;
failed:
    free(out); return NULL;
}


static int contains_ci(const char *text, const char *needle);

static int youtube_simple_mp4(const char *html, const char *end,
                              const char *watch_url, const char *title,
                              int *item_id, int *source_id)
{
    const char *p, *open, *close;
    char url[4096], mime[256], label[64], note[256];
    media_catalog *catalog;

    *item_id = -1;
    *source_id = -1;
    p = html;

    while ((p = find_bounded(p, end, "\"itag\":18")) != NULL) {
        open = p;
        while (open > html && *open != '{' && (p - open) < 768) --open;
        if (*open != '{') {
            p += 9;
            continue;
        }

        close = json_object_end(open, end);
        if (!close || close <= p) {
            p += 9;
            continue;
        }

        url[0] = mime[0] = label[0] = 0;
        if (!json_string_value(open, close, "\"url\"", url, sizeof(url))) {
            log_line("YOUTUBE-PLAY",
                     "itag 18 exists but direct URL is absent (ciphered)");
            p = close;
            continue;
        }

        json_string_value(open, close, "\"mimeType\"", mime, sizeof(mime));
        if (mime[0] && !contains_ci(mime, "video/mp4")) {
            p = close;
            continue;
        }

        json_string_value(open, close, "\"qualityLabel\"", label, sizeof(label));
        if (!label[0]) strcpy(label, "360p");

        catalog = media_catalog_for_key(watch_url, item_id);
        if (!catalog) return 0;

        strncpy(catalog->title, title && title[0] ? title : "YouTube video",
                sizeof(catalog->title) - 1);
        catalog->title[sizeof(catalog->title) - 1] = 0;
        strcpy(catalog->provider, "YouTube");
        strncpy(catalog->referer, watch_url, sizeof(catalog->referer) - 1);
        catalog->referer[sizeof(catalog->referer) - 1] = 0;

        *source_id = media_catalog_add(catalog, MEDIA_SOURCE_MP4, 360,
                                       label, url);
        if (*source_id < 0) return 0;
        catalog->selected_source = *source_id;

        sprintf(note, "direct muxed MP4 itag=18 item=%d source=%d label=%.40s",
                *item_id, *source_id, label);
        log_line("YOUTUBE-PLAY", note);
        return 1;
    }

    if (find_bounded(html, end, "\"streamingData\""))
        log_line("YOUTUBE-PLAY",
                 "streamingData present, no direct muxed MP4 itag 18 URL");
    else
        log_line("YOUTUBE-PLAY", "streamingData not present in watch HTML");
    return 0;
}

static char *youtube_watch_html(const char *html, size_t length,
                                const char *page_url, size_t *out_length)
{
    static const char page_begin[] =
        "<html><head><title>YouTube watch - NOS-Gate</title>"
        "<base target=\"_top\"></head>"
        "<body bgcolor=\"#ffffff\" text=\"#000000\">"
        "<h2>YouTube</h2>"
        "<form method=\"GET\" action=\"/lite/https%3A%2F%2Fwww.youtube.com%2Fresults\">"
        "<input name=\"search_query\" size=\"36\"> "
        "<input type=\"submit\" value=\"Search\"></form><hr>";
    static const char related[] = "<hr><h3>Related videos</h3>";
    static const char page_end[] =
        "<hr><p><a href=\"/\">NOS-Gate home</a></p></body></html>";
    const char *end = html + length, *p, *open, *close, *meta, *meta_open, *meta_close;
    char *out, *w, note[192], watch[512], thumb[512];
    char id[128], title[2048], channel[1024], duration[128];
    size_t capacity = 32768;
    int count = 0, play_item = -1, play_source = -1;

    id[0] = title[0] = channel[0] = duration[0] = 0;
    out = (char *)malloc(capacity);
    if (!out) return NULL;
    w = out;
    if (!append_output(&out, &w, &capacity, page_begin,
                       sizeof(page_begin) - 1)) goto failed;

    /* Current video: videoDetails is compact and stable inside
       ytInitialPlayerResponse; no JavaScript execution is needed. */
    p = find_bounded(html, end, "\"videoDetails\":");
    if (p) {
        open = find_bounded(p + strlen("\"videoDetails\":"), end, "{");
        close = open ? json_object_end(open, end) : NULL;
        if (open && close) {
            json_string_value(open, close, "\"videoId\"", id, sizeof(id));
            json_string_value(open, close, "\"title\"", title, sizeof(title));
            json_string_value(open, close, "\"author\"", channel, sizeof(channel));
            json_string_value(open, close, "\"lengthSeconds\"", duration, sizeof(duration));
        }
    }
    if (id[0] && title[0]) {
        sprintf(watch, "https://www.youtube.com/watch?v=%.120s", id);
        sprintf(thumb, "https://i.ytimg.com/vi/%.120s/mqdefault.jpg", id);
        if (!append_output(&out, &w, &capacity, "<p><a href=\"", 12) ||
            !append_routed(&out, &w, &capacity, watch, "/lite/") ||
            !append_output(&out, &w, &capacity, "\"><img src=\"", 12) ||
            !append_routed(&out, &w, &capacity, thumb, "/page/lite/") ||
            !append_output(&out, &w, &capacity,
                           "\" border=\"0\" alt=\"preview\"></a><br><b>", 39) ||
            !append_html_escaped(&out, &w, &capacity, title) ||
            !append_output(&out, &w, &capacity, "</b>", 4)) goto failed;
        if (channel[0] &&
            (!append_output(&out, &w, &capacity, "<br>", 4) ||
             !append_html_escaped(&out, &w, &capacity, channel))) goto failed;
        if (duration[0]) {
            unsigned long sec = strtoul(duration, NULL, 10);
            char human[64];
            if (sec >= 3600)
                sprintf(human, "%lu:%02lu:%02lu", sec / 3600,
                        (sec / 60) % 60, sec % 60);
            else
                sprintf(human, "%lu:%02lu", sec / 60, sec % 60);
            if (!append_output(&out, &w, &capacity, " [", 2) ||
                !append_output(&out, &w, &capacity, human, strlen(human)) ||
                !append_output(&out, &w, &capacity, "]", 1)) goto failed;
        }
        if (!append_output(&out, &w, &capacity, "</p>\r\n", 6)) goto failed;

        if (youtube_simple_mp4(html, end, page_url, title,
                               &play_item, &play_source)) {
            char play[512];
            sprintf(play,
                "<form method=\"GET\" action=\"/media/play\">"
                "<input type=\"hidden\" name=\"id\" value=\"%d\">"
                "<input type=\"hidden\" name=\"source\" value=\"%d\">"
                "<input type=\"submit\" value=\"Play 360p MP4\">"
                "</form>"
                "<p><small>Simple mode: one MP4 stream, video + audio.</small></p>",
                play_item, play_source);
            if (!append_output(&out, &w, &capacity, play, strlen(play)))
                goto failed;
        } else {
            const char *no_play =
                "<p><b>Play:</b> direct 360p MP4 is not available for this video.</p>";
            if (!append_output(&out, &w, &capacity, no_play, strlen(no_play)))
                goto failed;
        }
    }

    if (!append_output(&out, &w, &capacity, related,
                       sizeof(related) - 1)) goto failed;

    p = html;
    while (count < 24 &&
           (p = find_bounded(p, end, "{\"lockupViewModel\":")) != NULL) {
        open = p;
        close = json_object_end(open, end);
        if (!close) break;
        id[0] = title[0] = channel[0] = duration[0] = 0;

        if (!json_string_value(open, close, "\"contentId\"", id, sizeof(id))) {
            p = close; continue;
        }

        meta = find_bounded(open, close, "\"lockupMetadataViewModel\":");
        if (meta) {
            meta_open = find_bounded(meta + strlen("\"lockupMetadataViewModel\":"),
                                     close, "{");
            meta_close = meta_open ? json_object_end(meta_open, close) : NULL;
            if (meta_open && meta_close) {
                const char *t = find_bounded(meta_open, meta_close, "\"title\":");
                const char *cm = find_bounded(meta_open, meta_close,
                                               "\"contentMetadataViewModel\":");
                if (t) {
                    const char *to = find_bounded(t + strlen("\"title\":"),
                                                  meta_close, "{");
                    const char *tc = to ? json_object_end(to, meta_close) : NULL;
                    if (to && tc)
                        json_string_value(to, tc, "\"content\"", title,
                                          sizeof(title));
                }
                if (cm) {
                    const char *cmo = find_bounded(cm + strlen("\"contentMetadataViewModel\":"),
                                                   meta_close, "{");
                    const char *cmc = cmo ? json_object_end(cmo, meta_close) : NULL;
                    if (cmo && cmc)
                        json_string_value(cmo, cmc, "\"content\"", channel,
                                          sizeof(channel));
                }
            }
        }

        meta = find_bounded(open, close, "\"thumbnailBadgeViewModel\":");
        if (meta) {
            meta_open = find_bounded(meta + strlen("\"thumbnailBadgeViewModel\":"),
                                     close, "{");
            meta_close = meta_open ? json_object_end(meta_open, close) : NULL;
            if (meta_open && meta_close)
                json_string_value(meta_open, meta_close, "\"text\"", duration,
                                  sizeof(duration));
        }

        if (!title[0]) { p = close; continue; }
        sprintf(watch, "https://www.youtube.com/watch?v=%.120s", id);
        sprintf(thumb, "https://i.ytimg.com/vi/%.120s/mqdefault.jpg", id);

        if (!append_output(&out, &w, &capacity, "<p><a href=\"", 12) ||
            !append_routed(&out, &w, &capacity, watch, "/lite/") ||
            !append_output(&out, &w, &capacity, "\"><img src=\"", 12) ||
            !append_routed(&out, &w, &capacity, thumb, "/page/lite/") ||
            !append_output(&out, &w, &capacity,
                           "\" border=\"0\" alt=\"preview\"></a><br>", 35) ||
            !append_output(&out, &w, &capacity, "<b><a href=\"", 12) ||
            !append_routed(&out, &w, &capacity, watch, "/lite/") ||
            !append_output(&out, &w, &capacity, "\">", 2) ||
            !append_html_escaped(&out, &w, &capacity, title) ||
            !append_output(&out, &w, &capacity, "</a></b>", 8)) goto failed;
        if (duration[0] &&
            (!append_output(&out, &w, &capacity, " [", 2) ||
             !append_html_escaped(&out, &w, &capacity, duration) ||
             !append_output(&out, &w, &capacity, "]", 1))) goto failed;
        if (channel[0] &&
            (!append_output(&out, &w, &capacity, "<br>", 4) ||
             !append_html_escaped(&out, &w, &capacity, channel))) goto failed;
        if (!append_output(&out, &w, &capacity, "</p>\r\n", 6)) goto failed;
        ++count;
        p = close;
    }

    if (!count &&
        !append_output(&out, &w, &capacity,
                       "<p>No static related videos found.</p>", 38))
        goto failed;
    if (!append_output(&out, &w, &capacity, page_end,
                       sizeof(page_end) - 1)) goto failed;
    *w = 0;
    *out_length = (size_t)(w - out);
    sprintf(note, "watch page: current=%s, related=%d, %lu -> %lu bytes",
            title[0] ? "yes" : "no", count,
            (unsigned long)length, (unsigned long)*out_length);
    log_line("YOUTUBE-WATCH", note);
    (void)page_url;
    return out;
failed:
    free(out);
    return NULL;
}

static const char *find_tag_end(const char *p, const char *end)
{
    int quote = 0;
    while (p < end) {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '\'' || *p == '"') quote = (unsigned char)*p;
        else if (*p == '>') return p;
        ++p;
    }
    return NULL;
}

static int tag_is(const char *tag, const char *tag_end, const char *name,
                  int *closing, const char **attributes)
{
    const char *p = tag + 1, *begin;
    while (p < tag_end && isspace((unsigned char)*p)) ++p;
    *closing = 0;
    if (p < tag_end && *p == '/') { *closing = 1; ++p; }
    while (p < tag_end && isspace((unsigned char)*p)) ++p;
    begin = p;
    while (p < tag_end && html_name_char((unsigned char)*p)) ++p;
    *attributes = p;
    return name_is(begin, p, name);
}

static int tag_self_closing(const char *tag, const char *tag_end)
{
    const char *p = tag_end;
    while (p > tag && isspace((unsigned char)p[-1])) --p;
    return p > tag && p[-1] == '/';
}

static int contains_ci(const char *text, const char *needle)
{
    size_t n = strlen(needle);
    while (*text) {
        if (!_strnicmp(text, needle, n)) return 1;
        ++text;
    }
    return 0;
}

static int synchroncode_player(const char *url)
{
    char host[256];
    if (!url_host(url, host, sizeof(host))) return 0;
    return !_stricmp(host, "api.synchroncode.com") &&
           contains_ci(url, "/embed/");
}

static int remembered_explicit_iframe(const char *url)
{
    char parent[4096];
    if (synchroncode_player(url)) return 1;
    return iframe_referer_for(url, parent, sizeof(parent));
}

static int extract_js_string(const char *html, const char *name,
                             char *value, size_t value_size)
{
    const char *p = html, *q;
    size_t used = 0, name_len = strlen(name);
    int quote;
    if (!value_size) return 0;
    value[0] = 0;
    while ((p = strstr(p, name)) != NULL) {
        if (p > html && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
            p += name_len; continue;
        }
        q = p + name_len;
        while (*q && isspace((unsigned char)*q)) ++q;
        if (*q != ':') { p += name_len; continue; }
        ++q; while (*q && isspace((unsigned char)*q)) ++q;
        if (*q != '\'' && *q != '"') { p += name_len; continue; }
        quote = (unsigned char)*q++;
        while (*q && *q != quote && used + 1 < value_size) {
            if (*q == '\\') {
                ++q;
                if (!strncmp(q, "u0026", 5)) { value[used++] = '&'; q += 5; continue; }
                if (!strncmp(q, "u003d", 5)) { value[used++] = '='; q += 5; continue; }
                if (!strncmp(q, "u002f", 5) || !strncmp(q, "u002F", 5)) {
                    value[used++] = '/'; q += 5; continue;
                }
                if (*q == '/' || *q == '\\' || *q == '\'' || *q == '"')
                    value[used++] = *q++;
                else if (*q) { value[used++] = *q++; }
            } else value[used++] = *q++;
        }
        value[used] = 0;
        return used != 0;
    }
    return 0;
}

static void extract_html_title(const char *html, char *title, size_t title_size)
{
    const char *a = html, *b;
    size_t n;
    title[0] = 0;
    while (*a && _strnicmp(a, "<title", 6)) ++a;
    if (!*a || !(a = strchr(a, '>'))) return;
    ++a; b = a;
    while (*b && _strnicmp(b, "</title", 7)) ++b;
    n = (size_t)(b - a);
    if (n >= title_size) n = title_size - 1;
    memcpy(title, a, n); title[n] = 0;
}

static media_catalog *media_catalog_for_key(const char *key, int *item_id)
{
    media_catalog *catalog;
    int i, slot = -1;
    for (i = 0; i < MAX_MEDIA_ITEMS; ++i) {
        if (media_items[i].used && !strcmp(media_items[i].key, key)) {
            slot = i; break;
        }
        if (slot < 0 && !media_items[i].used) slot = i;
    }
    if (slot < 0) {
        slot = media_replace_cursor++ % MAX_MEDIA_ITEMS;
        log_line("MEDIA-SLOT", "catalog full; replacing oldest rotating slot");
    }
    catalog = &media_items[slot];
    memset(catalog, 0, sizeof(*catalog));
    catalog->used = 1; catalog->selected_source = -1;
    strncpy(catalog->key, key, sizeof(catalog->key) - 1);
    catalog->key[sizeof(catalog->key) - 1] = 0;
    *item_id = slot;
    return catalog;
}

static media_catalog *media_catalog_by_id(int item_id)
{
    if (item_id < 0 || item_id >= MAX_MEDIA_ITEMS ||
        !media_items[item_id].used) return NULL;
    return &media_items[item_id];
}

static int media_catalog_add(media_catalog *catalog, int type, int quality,
                             const char *label, const char *url)
{
    media_source *source;
    int index;
    if (!catalog || !url || !url[0] ||
        catalog->source_count >= MAX_MEDIA_SOURCES)
        return -1;
    index = catalog->source_count++;
    source = &catalog->source[index];
    memset(source, 0, sizeof(*source));
    source->used = 1; source->type = type; source->quality = quality;
    source->hls_kind = type == MEDIA_SOURCE_MP4 ? HLS_KIND_MUXED :
                                                   HLS_KIND_UNKNOWN;
    if (label) {
        strncpy(source->label, label, sizeof(source->label) - 1);
        source->label[sizeof(source->label) - 1] = 0;
    }
    strncpy(source->url, url, sizeof(source->url) - 1);
    source->url[sizeof(source->url) - 1] = 0;
    return index;
}

static void media_catalog_select_360(media_catalog *catalog)
{
    int i, best = -1, best_quality = 0x7FFFFFFF;
    int below = -1, below_quality = -1, unknown = -1;
    if (!catalog) return;
    for (i = 0; i < catalog->source_count; ++i) {
        media_source *source = &catalog->source[i];
        if (!source->used || source->hls_kind != HLS_KIND_MUXED) continue;
        if (source->quality >= 360 && source->quality < best_quality) {
            best = i; best_quality = source->quality;
        } else if (source->quality > 0 && source->quality < 360 &&
                   source->quality > below_quality) {
            below = i; below_quality = source->quality;
        } else if (source->quality <= 0 && unknown < 0) unknown = i;
    }
    catalog->selected_source = best >= 0 ? best :
                               (below >= 0 ? below : unknown);
}

static media_source *media_source_by_id(media_catalog *catalog, int source_id)
{
    if (!catalog) return NULL;
    if (source_id < 0) source_id = catalog->selected_source;
    if (source_id < 0 || source_id >= catalog->source_count ||
        !catalog->source[source_id].used) return NULL;
    return &catalog->source[source_id];
}

static media_catalog *explicit_iframe_adapter(const char *html,
    const char *player_url, const char *provider, int *item_id)
{
    media_catalog *catalog;
    char value[4096], title[512], note[256];
    int hls = 0, mp4 = 0;
    catalog = media_catalog_for_key(player_url, item_id);
    strncpy(catalog->provider, provider && provider[0] ? provider : "Unknown player",
            sizeof(catalog->provider) - 1);
    catalog->provider[sizeof(catalog->provider) - 1] = 0;
    strncpy(catalog->referer, player_url, sizeof(catalog->referer) - 1);
    extract_html_title(html, title, sizeof(title));
    if (!title[0]) strcpy(title, "Online video");
    strncpy(catalog->title, title, sizeof(catalog->title) - 1);
    if (extract_js_string(html, "download", value, sizeof(value))) {
        mp4 = media_catalog_add(catalog, MEDIA_SOURCE_MP4, 0,
                                "Direct MP4/download", value) >= 0;
    }
    if (extract_js_string(html, "hls", value, sizeof(value))) {
        hls = media_catalog_add(catalog, MEDIA_SOURCE_HLS, 0,
                                "HLS master playlist", value) >= 0;
    }
    media_catalog_select_360(catalog);
    sprintf(note, "id=%d provider=%.80s sources=%d mp4=%d hls=%d selected=%d",
            *item_id, catalog->provider, catalog->source_count, mp4, hls,
            catalog->selected_source);
    log_line("MEDIA-CATALOG", note);
    return catalog->source_count ? catalog : NULL;
}

static int hls_resolution_quality(const char *line)
{
    const char *p = line;
    int width = 0, height = 0;
    while (*p) {
        if (!_strnicmp(p, "RESOLUTION=", 11)) {
            p += 11;
            if (sscanf(p, "%dx%d", &width, &height) == 2 && height > 0)
                return height;
            return 0;
        }
        ++p;
    }
    return 0;
}

static int hls_variant_kind(const char *line)
{
    if (contains_ci(line, "AUDIO=")) return HLS_KIND_VIDEO_ONLY;
    if (contains_ci(line, "mp4a") || contains_ci(line, "ac-3") ||
        contains_ci(line, "ec-3") || contains_ci(line, "opus") ||
        contains_ci(line, "vorbis")) return HLS_KIND_MUXED;
    if (contains_ci(line, "CODECS=")) return HLS_KIND_VIDEO_ONLY;
    return HLS_KIND_UNKNOWN;
}

static int hls_attribute_value(const char *line, const char *name,
                               char *value, size_t value_size)
{
    const char *p = line, *start, *end;
    size_t name_length = strlen(name), length;
    value[0] = 0;
    while (*p) {
        if ((p == line || p[-1] == ',' || p[-1] == ':') &&
            !_strnicmp(p, name, name_length) && p[name_length] == '=') {
            start = p + name_length + 1;
            if (*start == '"') {
                ++start; end = strchr(start, '"');
            } else {
                end = strchr(start, ',');
                if (!end) end = start + strlen(start);
            }
            if (!end) return 0;
            length = (size_t)(end - start);
            if (length >= value_size) length = value_size - 1;
            memcpy(value, start, length); value[length] = 0;
            return length > 0;
        }
        ++p;
    }
    return 0;
}

static int hls_master_add_variants(media_catalog *catalog, const char *text,
                                   const char *base)
{
    const char *p = text, *line_end;
    int pending_quality = 0, pending_kind = HLS_KIND_UNKNOWN, added = 0;
    int index, i, audio_count = 0;
    hls_audio_group audio[8];
    char line[4096], absolute[4096], label[64], note[512];
    char group[128], uri[4096], pending_audio[4096];
    size_t length;
    memset(audio, 0, sizeof(audio));
    pending_audio[0] = 0;
    while (*p && audio_count < 8) {
        line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);
        length = (size_t)(line_end - p);
        while (length && (p[length - 1] == '\r' || p[length - 1] == ' ' ||
                          p[length - 1] == '\t')) --length;
        if (length >= sizeof(line)) length = sizeof(line) - 1;
        memcpy(line, p, length); line[length] = 0;
        if (!_strnicmp(line, "#EXT-X-MEDIA:", 13) &&
            contains_ci(line, "TYPE=AUDIO") &&
            hls_attribute_value(line, "GROUP-ID", group, sizeof(group)) &&
            hls_attribute_value(line, "URI", uri, sizeof(uri)) &&
            resolve_url(base, uri, absolute, sizeof(absolute))) {
            strncpy(audio[audio_count].id, group,
                    sizeof(audio[audio_count].id) - 1);
            strncpy(audio[audio_count].url, absolute,
                    sizeof(audio[audio_count].url) - 1);
            sprintf(note, "group=%.100s url=%.340s", group, absolute);
            log_line("HLS-AUDIO", note);
            ++audio_count;
        }
        p = *line_end ? line_end + 1 : line_end;
    }
    p = text;
    while (*p && catalog->source_count < MAX_MEDIA_SOURCES) {
        line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);
        length = (size_t)(line_end - p);
        while (length && (p[length - 1] == '\r' || p[length - 1] == ' ' ||
                          p[length - 1] == '\t')) --length;
        while (length && (*p == ' ' || *p == '\t')) { ++p; --length; }
        if (length >= sizeof(line)) length = sizeof(line) - 1;
        memcpy(line, p, length); line[length] = 0;
        if (!_strnicmp(line, "#EXT-X-STREAM-INF:", 18)) {
            pending_quality = hls_resolution_quality(line);
            pending_kind = hls_variant_kind(line);
            pending_audio[0] = 0;
            if (hls_attribute_value(line, "AUDIO", group, sizeof(group))) {
                for (i = 0; i < audio_count; ++i) {
                    if (!strcmp(audio[i].id, group)) {
                        strncpy(pending_audio, audio[i].url,
                                sizeof(pending_audio) - 1);
                        pending_audio[sizeof(pending_audio) - 1] = 0;
                        break;
                    }
                }
            }
        } else if (pending_quality && line[0] && line[0] != '#') {
            if (resolve_url(base, line, absolute, sizeof(absolute))) {
                sprintf(label, "HLS %dp %s", pending_quality,
                        pending_kind == HLS_KIND_MUXED ? "MUXED" :
                        (pending_kind == HLS_KIND_VIDEO_ONLY ?
                         "VIDEO ONLY" : "UNKNOWN"));
                index = media_catalog_add(catalog, MEDIA_SOURCE_HLS,
                                          pending_quality, label, absolute);
                if (index >= 0) {
                    catalog->source[index].hls_kind = pending_kind;
                    if (pending_audio[0]) {
                        strncpy(catalog->source[index].audio_url,
                                pending_audio,
                                sizeof(catalog->source[index].audio_url) - 1);
                    }
                    ++added;
                    sprintf(note, "q=%dp kind=%s audio=%s url=%.260s",
                            pending_quality,
                            pending_kind == HLS_KIND_MUXED ? "MUXED" :
                            (pending_kind == HLS_KIND_VIDEO_ONLY ?
                             "VIDEO-ONLY" : "UNKNOWN"),
                            pending_audio[0] ? "YES" : "NO", absolute);
                    log_line("HLS-VARIANT", note);
                }
            }
            pending_quality = 0;
            pending_kind = HLS_KIND_UNKNOWN;
            pending_audio[0] = 0;
        }
        p = *line_end ? line_end + 1 : line_end;
    }
    media_catalog_select_360(catalog);
    return added;
}

static const char *hls_resource_extension(const char *url)
{
    const char *p = url, *end = url + strlen(url), *slash = url, *dot = NULL;
    const char *query = strchr(url, '?');
    size_t length;
    if (query) end = query;
    while (p < end) {
        if (*p == '/') { slash = p; dot = NULL; }
        else if (*p == '.') dot = p;
        ++p;
    }
    if (!dot || dot < slash) return ".ts";
    length = (size_t)(end - dot);
    if (length == 5 && !_strnicmp(dot, ".m3u8", 5)) return ".m3u8";
    if (length == 4 && !_strnicmp(dot, ".m4s", 4)) return ".m4s";
    if (length == 4 && !_strnicmp(dot, ".mp4", 4)) return ".mp4";
    if (length == 4 && !_strnicmp(dot, ".aac", 4)) return ".aac";
    if (length == 3 && !_strnicmp(dot, ".ts", 3)) return ".ts";
    return ".ts";
}

static int append_hls_local_url(char **out, char **write, size_t *capacity,
                                int item_id, int source_id,
                                const char *url, const char *referer,
                                const char *forced_extension)
{
    char prefix[160];
    const char *extension = forced_extension;
    char *encoded_url, *encoded_referer, *end;
    size_t url_size, referer_size;
    int ok;
    url_size = encoded_length(url) + 1;
    referer_size = encoded_length(referer ? referer : "") + 1;
    if (url_size + referer_size + 128 >= REQUEST_SIZE) return 0;
    encoded_url = (char *)malloc(url_size);
    encoded_referer = (char *)malloc(referer_size);
    if (!encoded_url || !encoded_referer) {
        if (encoded_url) free(encoded_url);
        if (encoded_referer) free(encoded_referer);
        return 0;
    }
    end = url_encode_to(encoded_url, url); *end = 0;
    end = url_encode_to(encoded_referer, referer ? referer : ""); *end = 0;
    if (!extension || !extension[0]) extension = hls_resource_extension(url);
    sprintf(prefix,
            "http://127.0.0.1:%d/media/hls-resource%s?id=%d&source=%d&url=",
            8080, extension, item_id, source_id);
    ok = append_output(out, write, capacity, prefix, strlen(prefix)) &&
         append_output(out, write, capacity, encoded_url,
                       strlen(encoded_url)) &&
         append_output(out, write, capacity, "&ref=", 5) &&
         append_output(out, write, capacity, encoded_referer,
                       strlen(encoded_referer));
    free(encoded_url); free(encoded_referer);
    return ok;
}

static char *rewrite_hls_playlist(const char *text, size_t length,
                                  const char *base, int item_id,
                                  int source_id, size_t *out_length)
{
    const char *p = text, *end = text + length, *line_end;
    char *out, *write, absolute[4096], uri[4096];
    size_t capacity = length * 3 + 4096, line_length, uri_length;
    const char *uri_begin, *uri_end;
    out = (char *)malloc(capacity);
    if (!out) return NULL;
    write = out;
    while (p < end) {
        line_end = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        line_length = (size_t)(line_end - p);
        while (line_length && p[line_length - 1] == '\r') --line_length;
        if (line_length && p[0] != '#') {
            uri_length = line_length;
            if (uri_length >= sizeof(uri)) uri_length = sizeof(uri) - 1;
            memcpy(uri, p, uri_length); uri[uri_length] = 0;
            if (resolve_url(base, uri, absolute, sizeof(absolute))) {
                if (!append_hls_local_url(&out, &write, &capacity,
                                          item_id, source_id, absolute, base,
                                          NULL))
                    goto failed;
            } else if (!append_output(&out, &write, &capacity, p,
                                      line_length)) goto failed;
        } else if (line_length &&
                   (uri_begin = strstr(p, "URI=\"")) != NULL &&
                   uri_begin < p + line_length) {
            uri_begin += 5;
            uri_end = (const char *)memchr(uri_begin, '"',
                                          (size_t)(p + line_length - uri_begin));
            if (uri_end) {
                uri_length = (size_t)(uri_end - uri_begin);
                if (uri_length >= sizeof(uri)) uri_length = sizeof(uri) - 1;
                memcpy(uri, uri_begin, uri_length); uri[uri_length] = 0;
                if (resolve_url(base, uri, absolute, sizeof(absolute))) {
                    if (!append_output(&out, &write, &capacity, p,
                                       (size_t)(uri_begin - p)) ||
                        !append_hls_local_url(&out, &write, &capacity,
                                              item_id, source_id,
                                              absolute, base,
                                              !_strnicmp(p, "#EXT-X-KEY:", 11) ?
                                              ".key" : NULL) ||
                        !append_output(&out, &write, &capacity, uri_end,
                                       (size_t)(p + line_length - uri_end)))
                        goto failed;
                } else if (!append_output(&out, &write, &capacity, p,
                                           line_length)) goto failed;
            } else if (!append_output(&out, &write, &capacity, p,
                                       line_length)) goto failed;
        } else if (!append_output(&out, &write, &capacity, p,
                                   line_length)) goto failed;
        if (!append_output(&out, &write, &capacity, "\r\n", 2)) goto failed;
        p = line_end < end ? line_end + 1 : end;
    }
    *write = 0;
    *out_length = (size_t)(write - out);
    return out;
failed:
    free(out); return NULL;
}

static int hls_playlist_url(const char *url)
{
    return contains_ci(url, ".m3u8");
}

#ifndef NOS_GATE_PARSER_TEST
static int file_exists(const char *path)
{
    DWORD attr = GetFileAttributes(path);
    return attr != 0xFFFFFFFFUL && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int launch_media_url(const char *url, int hls, char *used_player,
                            size_t used_player_size)
{
    char local[MAX_PATH], command[8192], note[512];
    const char *players[3];
    const char *names[3] = { "MPlayer (local)", "VLC", "Media Player Classic" };
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES security;
    HANDLE output = INVALID_HANDLE_VALUE, input = INVALID_HANDLE_VALUE;
    DWORD wait_result, exit_code;
    int i, created;
    /* 0.5.1 Final: MPlayer belongs to NOS-Pipe, not NOS-Gate.
       Installed layout is preferred. Portable sibling layout is accepted too. */
    strcpy(local, "C:\\Program Files\\NOS-Pipe\\mplayer.exe");
    if (!file_exists(local)) {
        char gate_dir[MAX_PATH], *slash;
        module_side_path(gate_dir, sizeof(gate_dir), "NOS-Gate.exe");
        slash = strrchr(gate_dir, '\\\\');
        if (slash) {
            *slash = 0;
            slash = strrchr(gate_dir, '\\\\');
            if (slash)
                sprintf(local, "%.900s\\NOS-Pipe\\mplayer.exe", gate_dir);
        }
    }
    players[0] = local;
    players[1] = "C:\\Program Files\\VideoLAN\\vlc\\vlc.exe";
    players[2] = "C:\\Program Files\\K-Lite Codec Pack\\Media Player Classic\\mpc-hc.exe";
    for (i = 0; i < 3; ++i) {
        if (!file_exists(players[i])) continue;
        if (i == 0) {
            if (hls)
                sprintf(command, "\"%.1023s\" -quiet -cache 1024 \"%.7000s\"",
                        players[i], url);
            else
                sprintf(command, "\"%.1023s\" -quiet \"%.7000s\"",
                        players[i], url);
            log_line("MEDIA-PLAYER-CACHE", hls ?
                     "MPlayer HLS command uses -cache 1024" :
                     "MPlayer MP4 command keeps default cache policy");
        }
        else
            sprintf(command, "\"%.1023s\" \"%.7000s\"", players[i], url);
        memset(&security, 0, sizeof(security));
        security.nLength = sizeof(security); security.bInheritHandle = TRUE;
        output = CreateFile(gate_player_log_path, GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        input = CreateFile("NUL", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (output != INVALID_HANDLE_VALUE)
            SetFilePointer(output, 0, NULL, FILE_END);
        memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
        if (output != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = input; si.hStdOutput = output; si.hStdError = output;
        }
        memset(&pi, 0, sizeof(pi));
        created = CreateProcess(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL,
                                &si, &pi);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        input = output = INVALID_HANDLE_VALUE;
        if (created) {
            CloseHandle(pi.hThread);
            wait_result = WaitForSingleObject(pi.hProcess, 1500);
            if (wait_result == WAIT_OBJECT_0) {
                exit_code = 0;
                GetExitCodeProcess(pi.hProcess, &exit_code);
                CloseHandle(pi.hProcess);
                sprintf(note, "%s exited immediately with code %lu; trying next player",
                        names[i], (unsigned long)exit_code);
                log_line("MEDIA-PLAYER-FAIL", note);
                continue;
            }
            CloseHandle(pi.hProcess);
            strncpy(used_player, names[i], used_player_size - 1);
            used_player[used_player_size - 1] = 0;
            return 1;
        }
        sprintf(note, "%s CreateProcess failed with Windows error %lu",
                names[i], (unsigned long)GetLastError());
        log_line("MEDIA-PLAYER-FAIL", note);
    }
    if ((UINT)ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL) > 32) {
        strncpy(used_player, "Windows default handler", used_player_size - 1);
        used_player[used_player_size - 1] = 0;
        return 1;
    }
    return 0;
}
#else
static int launch_media_url(const char *url, int hls, char *used_player,
                            size_t used_player_size)
{
    (void)url; (void)hls; (void)used_player; (void)used_player_size;
    return 0;
}
#endif

static int iframe_url(const char *attributes, const char *tag_end,
                      const char *base, char *absolute,
                      size_t absolute_size)
{
    char src[4096];
    if (!find_tag_attribute(attributes, tag_end, "src", src, sizeof(src)) ||
        !src[0]) {
        if (!find_tag_attribute(attributes, tag_end, "data-src", src,
                                sizeof(src)) || !src[0])
            return 0;
    }
    return resolve_url(base, src, absolute, absolute_size);
}

static int host_is_or_subdomain(const char *host, const char *domain)
{
    size_t hn = strlen(host), dn = strlen(domain);
    if (hn < dn || _stricmp(host + hn - dn, domain)) return 0;
    return hn == dn || host[hn - dn - 1] == '.';
}

static const char *iframe_provider(const char *url)
{
    char host[256];
    if (!url_host(url, host, sizeof(host))) return "Unknown player";
    if (host_is_or_subdomain(host, "synchroncode.com")) return "Synchroncode";
    if (host_is_or_subdomain(host, "kodikplayer.com") ||
        host_is_or_subdomain(host, "kodik.info") ||
        host_is_or_subdomain(host, "kodik.biz")) return "Kodik";
    if (host_is_or_subdomain(host, "ceramet.net")) return "Ceramet";
    if (host_is_or_subdomain(host, "ashdi.vip")) return "Ashdi";
    if (host_is_or_subdomain(host, "youtube.com") ||
        host_is_or_subdomain(host, "youtube-nocookie.com")) return "YouTube";
    if (host_is_or_subdomain(host, "vimeo.com")) return "Vimeo";
    if (host_is_or_subdomain(host, "dailymotion.com") ||
        host_is_or_subdomain(host, "dai.ly")) return "Dailymotion";
    return "Unknown player";
}

static int probable_media_iframe(const char *attributes, const char *tag_end,
                                 const char *url, const char *provider)
{
    char value[512];
    if (_stricmp(provider, "Unknown player")) return 1;
    if (contains_ci(url, "/embed/") || contains_ci(url, "/player/") ||
        contains_ci(url, "/serial/") || contains_ci(url, "/movie/") ||
        contains_ci(url, "/video/") || contains_ci(url, "player") ||
        contains_ci(url, "stream")) return 1;
    if (find_tag_attribute(attributes, tag_end, "title", value,
                           sizeof(value)) &&
        (contains_ci(value, "player") || contains_ci(value, "video"))) return 1;
    if (find_tag_attribute(attributes, tag_end, "class", value,
                           sizeof(value)) &&
        (contains_ci(value, "player") || contains_ci(value, "video"))) return 1;
    if (find_tag_attribute(attributes, tag_end, "id", value,
                           sizeof(value)) &&
        (contains_ci(value, "player") || contains_ci(value, "video"))) return 1;
    return 0;
}

static void html_escape_text(const char *text, char *out, size_t out_size)
{
    size_t used = 0;
    const char *replacement;
    while (*text && used + 1 < out_size) {
        replacement = NULL;
        if (*text == '&') replacement = "&amp;";
        else if (*text == '<') replacement = "&lt;";
        else if (*text == '>') replacement = "&gt;";
        else if (*text == '"') replacement = "&quot;";
        if (replacement) {
            size_t n = strlen(replacement);
            if (used + n >= out_size) break;
            memcpy(out + used, replacement, n); used += n; ++text;
        } else out[used++] = *text++;
    }
    out[used] = 0;
}

static int youtube_page(const char *url)
{
    char host[256];
    size_t n;
    if (!url_host(url, host, sizeof(host))) return 0;
    n = strlen(host);
    return !_stricmp(host, "youtube.com") ||
           (n > 12 && !_stricmp(host + n - 12, ".youtube.com"));
}

static int unrecoverable_old_ie_image(const char *attributes,
                                      const char *tag_end)
{
    char value[4096];
    if (!find_tag_attribute(attributes, tag_end, "src", value, sizeof(value)) &&
        !find_tag_attribute(attributes, tag_end, "data-src", value, sizeof(value)))
        return 0;
    return contains_ci(value, ".svg");
}

static int youtube_search_input(const char *tag, const char *tag_end)
{
    const char *attributes;
    char value[256];
    int closing;
    if (!tag_is(tag, tag_end, "input", &closing, &attributes) || closing)
        return 0;
    return find_tag_attribute(attributes, tag_end, "name", value,
                              sizeof(value)) &&
           !_stricmp(value, "search_query");
}

static int rewrite_tag(const char *tag, const char *tag_end, const char *base,
                       const char *nav_route, const char *resource_route,
                       int processed, int super_lite,
                       int forced_width, int forced_height,
                       char **out, char **write, size_t *capacity)
{
    const char *p, *attributes, *copy_from;
    html_attribute a;
    char value[4096], lazy[4096], lazy_absolute[4096];
    char absolute[4096], background[4096], explicit_w_text[32], explicit_h_text[32];
    int closing, result, is_img, have_lazy, have_src, have_background, have_style;
    int explicit_w = 0, explicit_h = 0, lock_img_size = 0, card_half = 0, legacy_card = 0, legacy_wrap = 0, legacy_group = 0;
    int is_href, is_src, is_action, is_style, is_target, is_event, is_heavy;
    int is_link_tag, is_form_tag;
    const char *attribute_route;
    size_t n;

    tag_is(tag, tag_end, "img", &closing, &attributes);
    is_link_tag = tag_is(tag, tag_end, "link", &closing, &attributes) && !closing;
    is_form_tag = tag_is(tag, tag_end, "form", &closing, &attributes) && !closing;
    is_img = !closing && tag_is(tag, tag_end, "img", &closing, &attributes);
    have_lazy = is_img && find_tag_attribute(attributes, tag_end, "data-src",
                                              lazy, sizeof(lazy));
    if (have_lazy && !resolve_url(base, lazy, lazy_absolute,
                                  sizeof(lazy_absolute))) have_lazy = 0;
    have_src = is_img && find_tag_attribute(attributes, tag_end, "src",
                                             value, sizeof(value));
    have_background = find_tag_attribute(attributes, tag_end, "data-bg-image",
                                         value, sizeof(value)) &&
                      background_url(value, background, sizeof(background)) &&
                      resolve_url(base, background, absolute, sizeof(absolute));
    if (super_lite) have_background = 0;
    if (have_background) strcpy(background, absolute);
    have_style = find_tag_attribute(attributes, tag_end, "style",
                                    value, sizeof(value));
    card_half = is_img && find_tag_attribute(attributes, tag_end,
                                             "data-nos-card-half",
                                             value, sizeof(value));
    legacy_card = is_img && find_tag_attribute(attributes, tag_end,
                                               "data-nos-card",
                                               value, sizeof(value));
    legacy_wrap = find_tag_attribute(attributes, tag_end,
                                     "data-nos-card-wrap", value, sizeof(value));
    legacy_group = find_tag_attribute(attributes, tag_end,
                                      "data-nos-card-group", value, sizeof(value));
    if (processed && is_img &&
        find_tag_attribute(attributes, tag_end, "width",
                           explicit_w_text, sizeof(explicit_w_text)) &&
        find_tag_attribute(attributes, tag_end, "height",
                           explicit_h_text, sizeof(explicit_h_text))) {
        explicit_w = atoi(explicit_w_text);
        explicit_h = atoi(explicit_h_text);
        if (explicit_w > 0 && explicit_h > 0) {
            if (card_half && !legacy_card) {
                explicit_w = (explicit_w + 1) / 2;
                explicit_h = (explicit_h + 1) / 2;
            }
            lock_img_size = 1;
        }
    }

    p = attributes; copy_from = tag;
    while (p < tag_end) {
        result = next_html_attribute(&p, tag_end, &a);
        if (result < 0) continue;
        if (!result) break;
        is_href = name_is(a.begin, a.name_end, "href");
        is_src = name_is(a.begin, a.name_end, "src");
        is_action = name_is(a.begin, a.name_end, "action");
        is_style = name_is(a.begin, a.name_end, "style");
        is_target = name_is(a.begin, a.name_end, "target");
        is_event = a.name_end - a.begin >= 3 &&
                   tolower((unsigned char)a.begin[0]) == 'o' &&
                   tolower((unsigned char)a.begin[1]) == 'n';
        is_heavy = name_is(a.begin, a.name_end, "srcset") ||
                   name_is(a.begin, a.name_end, "sizes") ||
                   name_is(a.begin, a.name_end, "loading") ||
                   name_is(a.begin, a.name_end, "fetchpriority") ||
                   name_is(a.begin, a.name_end, "decoding") ||
                   (a.name_end - a.begin > 5 &&
                    !_strnicmp(a.begin, "data-", 5));

        if ((is_form_tag && is_target) ||
            (processed && ((super_lite && is_style && !legacy_wrap && !legacy_group) || is_target || is_event ||
             (super_lite && name_is(a.begin, a.name_end, "data-bg-image")) ||
             (is_heavy && !name_is(a.begin, a.name_end, "data-bg-image") &&
              !name_is(a.begin, a.name_end, "data-nos-card-wrap") &&
              !name_is(a.begin, a.name_end, "data-nos-card-group") &&
              !name_is(a.begin, a.name_end, "data-nos-card-count") &&
              !name_is(a.begin, a.name_end, "data-nos-card-width") &&
              !name_is(a.begin, a.name_end, "data-nos-card-img"))))) {
            if (!append_output(out, write, capacity, copy_from,
                               (size_t)(a.begin - copy_from))) return 0;
            copy_from = a.end;
            continue;
        }

        if ((legacy_card && name_is(a.begin, a.name_end, "data-nos-card")) ||
            (card_half && name_is(a.begin, a.name_end, "data-nos-card-half")) ||
            (have_lazy && name_is(a.begin, a.name_end, "data-src")) ||
            (have_background && name_is(a.begin, a.name_end, "data-bg-image"))) {
            if (!append_output(out, write, capacity, copy_from,
                               (size_t)(a.begin - copy_from))) return 0;
            copy_from = a.end;
            continue;
        }
        if (lock_img_size && !super_lite && have_style && is_style &&
            a.value != a.value_end) {
            char fixed_style[96];
            if (!append_output(out, write, capacity, copy_from,
                               (size_t)(a.value_end - copy_from))) return 0;
            if (a.value_end[-1] != ';' &&
                !append_output(out, write, capacity, ";", 1)) return 0;
            sprintf(fixed_style, "width:%dpx;height:%dpx", explicit_w, explicit_h);
            if (!append_output(out, write, capacity,
                               fixed_style, strlen(fixed_style))) return 0;
            copy_from = a.value_end;
            continue;
        }
        if (have_background && have_style && is_style && a.value != a.value_end) {
            if (!append_output(out, write, capacity, copy_from,
                               (size_t)(a.value_end - copy_from))) return 0;
            if (a.value_end[-1] != ';' &&
                !append_output(out, write, capacity, ";", 1)) return 0;
            if (!append_output(out, write, capacity,
                               "background-image:url('", 22)) return 0;
            if (!append_routed(out, write, capacity, background, resource_route)) return 0;
            if (!append_output(out, write, capacity, "')", 2)) return 0;
            copy_from = a.value_end;
            continue;
        }
        if (a.value == a.value_end || (!is_href && !is_src && !is_action)) continue;
        n = (size_t)(a.value_end - a.value);
        if (n >= sizeof(value)) continue;
        if (have_lazy && is_src) strcpy(value, lazy_absolute);
        else { memcpy(value, a.value, n); value[n] = 0; decode_html_amp(value); }
        if (processed && (is_href || is_action) && starts_ci(value, "javascript:")) {
            if (!append_output(out, write, capacity, copy_from,
                               (size_t)(a.begin - copy_from))) return 0;
            copy_from = a.end;
            continue;
        }
        if (!resolve_url(base, value, absolute, sizeof(absolute))) continue;
        unwrap_frogfind(absolute, sizeof(absolute));
        if (!append_output(out, write, capacity, copy_from,
                           (size_t)(a.value - copy_from))) return 0;
        if (!a.quote && !append_output(out, write, capacity, "\"", 1)) return 0;
        attribute_route = (is_src || is_action || (is_href && is_link_tag)) ?
                          resource_route : nav_route;
        if (!append_routed(out, write, capacity, absolute, attribute_route)) return 0;
        if (!a.quote && !append_output(out, write, capacity, "\"", 1)) return 0;
        copy_from = a.value_end;
    }
    if (!append_output(out, write, capacity, copy_from,
                       (size_t)(tag_end - copy_from))) return 0;
    if (have_background && !have_style) {
        if (!append_output(out, write, capacity,
                           " style=\"background-image:url('", 30)) return 0;
        if (!append_routed(out, write, capacity, background, resource_route)) return 0;
        if (!append_output(out, write, capacity, "')\"", 3)) return 0;
    }
    if (have_lazy && !have_src) {
        if (!append_output(out, write, capacity, " src=\"", 6)) return 0;
        if (!append_routed(out, write, capacity, lazy_absolute, resource_route)) return 0;
        if (!append_output(out, write, capacity, "\"", 1)) return 0;
    }
    if (is_form_tag &&
        !append_output(out, write, capacity, " target=\"content\"", 17)) return 0;
    if (lock_img_size && (!have_style || super_lite)) {
        char fixed_style[112];
        sprintf(fixed_style, " style=\"width:%dpx;height:%dpx\"",
                explicit_w, explicit_h);
        if (!append_output(out, write, capacity,
                           fixed_style, strlen(fixed_style))) return 0;
    }
    if (is_img && forced_width > 0 && forced_height > 0) {
        char geometry[96];
        sprintf(geometry, " width=\"%d\" height=\"%d\"", forced_width, forced_height);
        if (!append_output(out, write, capacity, geometry, strlen(geometry))) return 0;
    }
    if (lock_img_size) {
        char note[160];
        sprintf(note, "explicit IMG locked to %dx%d in %s",
                explicit_w, explicit_h, super_lite ? "SUPERLITE" : "LITE");
        log_line("IMG-SIZE-FIX", note);
    }
    if (have_lazy) log_line("HTML", "data-src converted to ordinary img src");
    if (have_background) log_line("HTML", "data-bg-image converted to ordinary CSS background");
    return append_output(out, write, capacity, tag_end, 1);
}

static int starts_ci_bounded(const char *p, const char *end, const char *text)
{
    size_t n = strlen(text);
    return (size_t)(end - p) >= n && !_strnicmp(p, text, n);
}

static int search_submit_button(const char *tag, const char *tag_end)
{
    const char *attributes;
    char value[4096];
    int closing;
    if (!tag_is(tag, tag_end, "button", &closing, &attributes) || closing)
        return 0;
    if (find_tag_attribute(attributes, tag_end, "type", value, sizeof(value)) &&
        _stricmp(value, "submit")) return 0;
    if (find_tag_attribute(attributes, tag_end, "aria-label", value,
                           sizeof(value)) && contains_ci(value, "search"))
        return 1;
    if (find_tag_attribute(attributes, tag_end, "class", value,
                           sizeof(value)) && contains_ci(value, "search"))
        return 1;
    return 0;
}

static int google_page(const char *url)
{
    char host[256];
    size_t n;
    if (!url_host(url, host, sizeof(host))) return 0;
    n = strlen(host);
    return !_stricmp(host, "google.com") ||
           (n > 11 && !_stricmp(host + n - 11, ".google.com")) ||
           (n > 10 && !_strnicmp(host, "www.google.", 11));
}

typedef struct search_form_info {
    int found;
    int have_submit;
    int hidden_count;
    char method[16];
    char action[512];
    char field[128];
} search_form_info;

static int search_word(const char *value)
{
    return contains_ci(value, "search") || contains_ci(value, "query") ||
           contains_ci(value, "keyword") || strstr(value, "\320\277\320\276\320\270\321\201\320\272") != NULL;
}

static int probable_search_input(const char *tag, const char *tag_end,
                                 char *field, size_t field_size)
{
    const char *attributes;
    char value[512], name[128], type[64];
    int closing;
    name[0] = 0;
    type[0] = 0;
    if (!tag_is(tag, tag_end, "input", &closing, &attributes) || closing)
        return 0;
    find_tag_attribute(attributes, tag_end, "name", name, sizeof(name));
    find_tag_attribute(attributes, tag_end, "type", type, sizeof(type));
    if (type[0] && _stricmp(type, "text") && _stricmp(type, "search"))
        return 0;
    if (!_stricmp(name, "submit") || !_stricmp(name, "button") ||
        !_stricmp(name, "image")) return 0;
    if (!_stricmp(type, "search")) goto found;
    if (name[0] && (!_stricmp(name, "q") || !_stricmp(name, "s") ||
        !_stricmp(name, "query") || !_stricmp(name, "search") ||
        !_stricmp(name, "search_query") || !_stricmp(name, "story") ||
        !_stricmp(name, "keyword") || !_stricmp(name, "keywords"))) goto found;
    if (find_tag_attribute(attributes, tag_end, "placeholder", value,
                           sizeof(value)) && search_word(value)) goto found;
    if (find_tag_attribute(attributes, tag_end, "id", value,
                           sizeof(value)) && search_word(value)) goto found;
    if (find_tag_attribute(attributes, tag_end, "class", value,
                           sizeof(value)) && search_word(value)) goto found;
    return 0;
found:
    if (field && field_size) {
        strncpy(field, name[0] ? name : "(unnamed)", field_size - 1);
        field[field_size - 1] = 0;
    }
    return 1;
}

static int form_submit_control(const char *tag, const char *tag_end,
                               int *image_submit)
{
    const char *attributes;
    char value[64];
    int closing;
    if (image_submit) *image_submit = 0;
    if (tag_is(tag, tag_end, "button", &closing, &attributes) && !closing) {
        if (!find_tag_attribute(attributes, tag_end, "type", value, sizeof(value)) ||
            !_stricmp(value, "submit")) return 1;
        return 0;
    }
    if (!tag_is(tag, tag_end, "input", &closing, &attributes) || closing)
        return 0;
    if (!find_tag_attribute(attributes, tag_end, "type", value, sizeof(value)))
        return 0;
    if (!_stricmp(value, "image")) {
        if (image_submit) *image_submit = 1;
        return 1;
    }
    return !_stricmp(value, "submit");
}

static void log_search_form_controls(const char *begin, const char *end)
{
    const char *p = begin, *tag_end, *attributes;
    char name[128], id[128], placeholder[160], type[64], note[512];
    const char *kind;
    int closing;
    while (p < end) {
        p = (const char *)memchr(p, '<', (size_t)(end - p));
        if (!p) break;
        tag_end = find_tag_end(p + 1, end);
        if (!tag_end) break;
        kind = NULL;
        if (tag_is(p, tag_end, "input", &closing, &attributes) && !closing)
            kind = "input";
        else if (tag_is(p, tag_end, "button", &closing, &attributes) && !closing)
            kind = "button";
        else if (tag_is(p, tag_end, "textarea", &closing, &attributes) && !closing)
            kind = "textarea";
        else if (tag_is(p, tag_end, "select", &closing, &attributes) && !closing)
            kind = "select";
        if (kind) {
            name[0] = id[0] = placeholder[0] = type[0] = 0;
            find_tag_attribute(attributes, tag_end, "name", name, sizeof(name));
            find_tag_attribute(attributes, tag_end, "id", id, sizeof(id));
            find_tag_attribute(attributes, tag_end, "placeholder", placeholder,
                               sizeof(placeholder));
            find_tag_attribute(attributes, tag_end, "type", type, sizeof(type));
            if (!type[0]) {
                if (!_stricmp(kind, "input")) strcpy(type, "text/default");
                else if (!_stricmp(kind, "button")) strcpy(type, "submit/default");
                else strncpy(type, kind, sizeof(type) - 1);
                type[sizeof(type) - 1] = 0;
            }
            sprintf(note,
                    "control=%.12s type=%.48s name=%.100s id=%.100s placeholder=%.120s",
                    kind, type, name[0] ? name : "(none)",
                    id[0] ? id : "(none)",
                    placeholder[0] ? placeholder : "(none)");
            log_line("SEARCH-CONTROL", note);
        }
        p = tag_end + 1;
    }
}

static const char *inspect_search_form(const char *form, const char *form_end,
                                       const char *document_end,
                                       search_form_info *info)
{
    const char *p, *tag_end, *attributes, *close = document_end;
    char value[512];
    int closing, image_submit;
    memset(info, 0, sizeof(*info));
    strcpy(info->method, "GET");
    if (tag_is(form, form_end, "form", &closing, &attributes) && !closing) {
        if (find_tag_attribute(attributes, form_end, "method", value,
                               sizeof(value))) {
            strncpy(info->method, value, sizeof(info->method) - 1);
            info->method[sizeof(info->method) - 1] = 0;
        }
        if (find_tag_attribute(attributes, form_end, "action", value,
                               sizeof(value))) {
            strncpy(info->action, value, sizeof(info->action) - 1);
            info->action[sizeof(info->action) - 1] = 0;
        }
    }
    p = form_end + 1;
    while (p < document_end) {
        p = (const char *)memchr(p, '<', (size_t)(document_end - p));
        if (!p) break;
        tag_end = find_tag_end(p + 1, document_end);
        if (!tag_end) break;
        if (tag_is(p, tag_end, "form", &closing, &attributes) && closing) {
            close = p; break;
        }
        if (probable_search_input(p, tag_end, info->field,
                                  sizeof(info->field))) info->found = 1;
        if (tag_is(p, tag_end, "input", &closing, &attributes) && !closing &&
            find_tag_attribute(attributes, tag_end, "type", value,
                               sizeof(value)) && !_stricmp(value, "hidden"))
            ++info->hidden_count;
        if (form_submit_control(p, tag_end, &image_submit)) info->have_submit = 1;
        p = tag_end + 1;
    }
    if (info->found) log_search_form_controls(form_end + 1, close);
    return close;
}

static int opensearch_link(const char *tag, const char *tag_end,
                           char *href, size_t href_size)
{
    const char *attributes;
    char rel[128], type[256];
    int closing;
    if (!tag_is(tag, tag_end, "link", &closing, &attributes) || closing)
        return 0;
    if (!find_tag_attribute(attributes, tag_end, "rel", rel, sizeof(rel)) ||
        !contains_ci(rel, "search")) return 0;
    type[0] = 0;
    find_tag_attribute(attributes, tag_end, "type", type, sizeof(type));
    if (type[0] && !contains_ci(type, "opensearch")) return 0;
    return find_tag_attribute(attributes, tag_end, "href", href, href_size);
}

static int html_range_has_visible_text(const char *begin, const char *end)
{
    const char *p = begin;
    int inside_tag = 0, quote = 0;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        if (inside_tag) {
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '>') inside_tag = 0;
            continue;
        }
        if (c == '<') { inside_tag = 1; continue; }
        if (isspace(c)) continue;
        if (c == '&' && starts_ci_bounded(p - 1, end, "&nbsp;")) {
            p += 5; continue;
        }
        return 1;
    }
    return 0;
}

static int append_google_search(char **out, char **write, size_t *capacity,
                                const char *route)
{
    static const char google_url[] = "https://www.google.com/search";
    if (!append_output(out, write, capacity,
        "<hr><form method=\"GET\" action=\"", 31) ||
        !append_routed(out, write, capacity, google_url, route) ||
        !append_output(out, write, capacity,
        "\"><b>Google search:</b> <input name=\"q\" size=\"36\"> "
        "<input type=\"submit\" value=\"Search\"></form>", 94)) return 0;
    return 1;
}


static int lite_dangerous_empty_anchor(const char *tag, const char *tag_end,
                                       const char *end, const char **skip_to)
{
    const char *attributes, *close, *close_end;
    char href[1024], style[2048], cls[512], id[512];
    int closing, suspicious = 0;
    if (!tag_is(tag, tag_end, "a", &closing, &attributes) || closing) return 0;
    href[0] = style[0] = cls[0] = id[0] = 0;
    find_tag_attribute(attributes, tag_end, "href", href, sizeof(href));
    find_tag_attribute(attributes, tag_end, "style", style, sizeof(style));
    find_tag_attribute(attributes, tag_end, "class", cls, sizeof(cls));
    find_tag_attribute(attributes, tag_end, "id", id, sizeof(id));

    /* Script-only anchors can never work in LITE. */
    if (starts_ci(href, "javascript:") || href[0] == '#') suspicious = 1;

    /* Empty absolute/fixed overlay anchors are typical ad/click layers.
       Require both emptiness and structural overlay evidence: names alone
       never cause removal. */
    if ((contains_ci(style, "position:fixed") ||
         contains_ci(style, "position: fixed") ||
         contains_ci(style, "position:absolute") ||
         contains_ci(style, "position: absolute")) &&
        (contains_ci(style, "z-index") ||
         contains_ci(style, "width:100%") ||
         contains_ci(style, "width: 100%") ||
         contains_ci(style, "height:100%") ||
         contains_ci(style, "height: 100%")))
        suspicious = 1;

    if (!suspicious) return 0;
    close = tag_end + 1;
    while (close < end && !starts_ci_bounded(close, end, "</a")) ++close;
    if (close >= end) return 0;
    if (html_range_has_visible_text(tag_end + 1, close)) return 0;
    /* Keep anchors containing an actual image; it may be useful navigation. */
    {
        const char *q = tag_end + 1;
        while (q < close) {
            q = (const char *)memchr(q, '<', (size_t)(close - q));
            if (!q) break;
            close_end = find_tag_end(q + 1, close);
            if (!close_end) break;
            if (tag_is(q, close_end, "img", &closing, &attributes) && !closing)
                return 0;
            q = close_end + 1;
        }
    }
    close_end = find_tag_end(close + 1, end);
    if (!close_end) return 0;
    *skip_to = close_end + 1;
    return 1;
}


static void lite_layout_diag_img(const char *tag, const char *tag_end,
                                 const char *html_begin, const char *html_end,
                                 const char *base, int *count)
{
    char src[1024], width[64], height[64], cls[256], id[256], style[512];
    char note[4096], absurl[2048];
    const char *q, *parent_start = NULL, *parent_end = NULL;
    int has_w, has_h;

    src[0]=width[0]=height[0]=cls[0]=id[0]=style[0]=absurl[0]=0;
    find_tag_attribute(tag+1, tag_end, "src", src, sizeof(src));
    has_w=find_tag_attribute(tag+1, tag_end, "width", width, sizeof(width));
    has_h=find_tag_attribute(tag+1, tag_end, "height", height, sizeof(height));
    if (has_w && has_h) return;

    find_tag_attribute(tag+1, tag_end, "class", cls, sizeof(cls));
    find_tag_attribute(tag+1, tag_end, "id", id, sizeof(id));
    find_tag_attribute(tag+1, tag_end, "style", style, sizeof(style));
    if (src[0]) resolve_url(base, src, absurl, sizeof(absurl));

    /* Find nearest opening tag before IMG, only for a short structural hint.
       Diagnostic only; no parser decisions are based on this. */
    q=tag;
    while (q>html_begin) {
        --q;
        if (*q=='<') { parent_start=q; break; }
    }
    if (parent_start) parent_end=find_tag_end(parent_start+1, tag);

    sprintf(note,
        "IMG #%d missing-size width=%s height=%s src=%.1200s class=%.220s id=%.220s style=%.400s parent=%.900s",
        ++(*count),
        has_w ? width : "(none)", has_h ? height : "(none)",
        absurl[0] ? absurl : (src[0] ? src : "(none)"),
        cls[0] ? cls : "(none)", id[0] ? id : "(none)",
        style[0] ? style : "(none)",
        (parent_start && parent_end) ? parent_start : "(unknown)");
    /* Keep each diagnostic line bounded even if parent tag is huge. */
    note[sizeof(note)-1]=0;
    log_line("LITE-LAYOUT-DIAG", note);
}



static int card_diag_tag_name(const char *tag, const char *tag_end,
                              char *name, size_t name_size,
                              int *closing, const char **attributes)
{
    const char *p=tag+1, *b;
    size_t n;
    while (p<tag_end && isspace((unsigned char)*p)) ++p;
    *closing=0;
    if (p<tag_end && *p=='/') { *closing=1; ++p; }
    while (p<tag_end && isspace((unsigned char)*p)) ++p;
    b=p;
    while (p<tag_end && html_name_char((unsigned char)*p)) ++p;
    n=(size_t)(p-b);
    if (!n || n>=name_size) return 0;
    memcpy(name,b,n); name[n]=0; *attributes=p;
    return 1;
}

static int card_diag_item_signature(const char *start, const char *finish,
                                    char *sig, size_t sig_size,
                                    char *sample, size_t sample_size,
                                    int *have_img, int *have_link, int *have_text)
{
    const char *p=start, *te, *attrs, *q;
    char name[24], cls[160], src[256];
    int closing, tags=0, imgs=0, links=0, textchars=0;
    size_t su=0, i;

    cls[0]=src[0]=0; sample[0]=0;
    *have_img=*have_link=*have_text=0;

    /* Signature intentionally ignores random class names and text.
       It records only a short sequence of structural tags plus IMG/A presence. */
    while (p<finish && tags<18) {
        if (*p!='<') {
            q=(const char *)memchr(p,'<',(size_t)(finish-p));
            if (!q) q=finish;
            while (p<q) {
                if (!isspace((unsigned char)*p)) {
                    ++textchars;
                    if (sample_size>1 && strlen(sample)+1<sample_size &&
                        strlen(sample)<80) {
                        size_t z=strlen(sample);
                        sample[z]=(*p=='\r'||*p=='\n'||*p=='\t')?' ':*p;
                        sample[z+1]=0;
                    }
                }
                ++p;
            }
            continue;
        }
        te=find_tag_end(p+1,finish);
        if (!te) break;
        if (card_diag_tag_name(p,te,name,sizeof(name),&closing,&attrs)) {
            if (!closing) {
                if (!_stricmp(name,"img")) { ++imgs; *have_img=1; }
                if (!_stricmp(name,"a")) { ++links; *have_link=1; }
                if (su+strlen(name)+2<sig_size) {
                    if (su) sig[su++]='/';
                    for (i=0; name[i] && su+1<sig_size; ++i)
                        sig[su++]=(char)tolower((unsigned char)name[i]);
                    sig[su]=0;
                }
                ++tags;
            }
        }
        p=te+1;
    }
    *have_text=textchars>=3;
    if (!sig[0]) return 0;
    return 1;
}

static const char *card_diag_matching_close(const char *open_tag,
                                            const char *open_end,
                                            const char *end,
                                            const char *wanted)
{
    const char *p=open_end+1, *te, *attrs;
    char name[24];
    int closing, depth=1;
    while (p<end) {
        p=(const char *)memchr(p,'<',(size_t)(end-p));
        if (!p) return NULL;
        te=find_tag_end(p+1,end);
        if (!te) return NULL;
        if (card_diag_tag_name(p,te,name,sizeof(name),&closing,&attrs) &&
            !_stricmp(name,wanted)) {
            if (closing) {
                if (--depth==0) return te+1;
            } else if (!tag_self_closing(p,te)) ++depth;
        }
        p=te+1;
    }
    return NULL;
}

static void card_candidate_diag(const char *html, const char *end, int mode)
{
    const char *p=html, *te, *attrs, *item_end;
    char name[24], cls[180], sig[256], last_sig[256], sample[128];
    char group_sample[128], note[900], last_name[24];
    int closing, hi, hl, ht, run=0, groups=0, items=0;

    if (mode==MODE_ORIGINAL) return;
    last_sig[0]=last_name[0]=group_sample[0]=0;

    while (p<end) {
        p=(const char *)memchr(p,'<',(size_t)(end-p));
        if (!p) break;
        te=find_tag_end(p+1,end);
        if (!te) break;
        if (!card_diag_tag_name(p,te,name,sizeof(name),&closing,&attrs) ||
            closing ||
            (_stricmp(name,"div") && _stricmp(name,"li") &&
             _stricmp(name,"td") && _stricmp(name,"article"))) {
            p=te+1; continue;
        }

        item_end=card_diag_matching_close(p,te,end,name);
        if (!item_end) { p=te+1; continue; }

        sig[0]=sample[0]=0;
        if (!card_diag_item_signature(te+1,item_end-1,sig,sizeof(sig),
                                      sample,sizeof(sample),&hi,&hl,&ht) ||
            !hi || !hl || !ht) {
            run=0; last_sig[0]=last_name[0]=0;
            p=te+1; continue;
        }

        /* Consecutive structurally similar card-like items. */
        if (last_sig[0] && !_stricmp(last_name,name) && !strcmp(last_sig,sig)) {
            ++run;
        } else {
            run=1;
            strcpy(last_sig,sig);
            strncpy(last_name,name,sizeof(last_name)-1);
            last_name[sizeof(last_name)-1]=0;
            strncpy(group_sample,sample,sizeof(group_sample)-1);
            group_sample[sizeof(group_sample)-1]=0;
        }

        if (run==3) {
            cls[0]=0;
            find_tag_attribute(attrs,te,"class",cls,sizeof(cls));
            sprintf(note,
                    "group>=3 item=%s class=%.150s signature=%.230s img=yes link=yes text=yes sample=%.120s",
                    name,cls[0]?cls:"(none)",sig,group_sample);
            note[sizeof(note)-1]=0;
            log_line("CARD-CANDIDATE",note);
            ++groups; items+=3;
        } else if (run>3) ++items;

        /* Do not skip subtree: nested card lists can be useful diagnostics too. */
        p=te+1;
    }
    sprintf(note,"summary groups=%d repeated-items>=%d mode=%s",
            groups,items,mode==MODE_LIGHT?"LITE":"SUPERLITE");
    log_line("CARD-CANDIDATE",note);
}


/* Convert only very obvious repeated DIV card groups to old HTML tables.
   This runs before the normal LITE rewrite.  It deliberately removes the
   original parent/direct-child classes from the layout skeleton, so modern
   descendant positioning CSS cannot pull captions/images out of table cells. */

static int append_legacy_card_inner(char **out, char **w, size_t *cap,
                                    const char *start, const char *finish)
{
    const char *p=start, *te, *attrs;
    int closing;
    while (p<finish) {
        const char *q=(const char *)memchr(p,'<',(size_t)(finish-p));
        if (!q) return append_output(out,w,cap,p,(size_t)(finish-p));
        if (q>p && !append_output(out,w,cap,p,(size_t)(q-p))) return 0;
        te=find_tag_end(q+1,finish);
        if (!te) return append_output(out,w,cap,q,(size_t)(finish-q));
        if (tag_is(q,te,"img",&closing,&attrs) && !closing) {
            if (!append_output(out,w,cap,q,(size_t)(te-q))) return 0;
            if (!append_output(out,w,cap," data-nos-card-img=\"1\"",
                               strlen(" data-nos-card-img=\"1\""))) return 0;
            if (!append_output(out,w,cap,te,1)) return 0;
        } else {
            if (!append_output(out,w,cap,q,(size_t)(te+1-q))) return 0;
        }
        p=te+1;
    }
    return 1;
}

static int legacy_card_half_width(const char *start, const char *finish)
{
    const char *p=start, *te, *attrs;
    char wt[32];
    int closing, iw;
    while (p<finish) {
        p=(const char *)memchr(p,'<',(size_t)(finish-p));
        if (!p) break;
        te=find_tag_end(p+1,finish);
        if (!te) break;
        if (tag_is(p,te,"img",&closing,&attrs) && !closing &&
            find_tag_attribute(attrs,te,"width",wt,sizeof(wt))) {
            iw=atoi(wt);
            if (iw>0) return iw;
        }
        p=te+1;
    }
    return 200;
}

static char *legacy_card_layout(const char *html, size_t length, int mode,
                                size_t *new_length, int *groups_done)
{
    const char *end=html+length, *p=html, *te, *attrs, *parent_end;
    char *out=NULL, *w=NULL, name[24], note[256];
    size_t cap=length+4096;
    int closing, groups=0;

    if (mode==MODE_ORIGINAL) return NULL;
    out=(char *)malloc(cap);
    if (!out) return NULL;
    w=out;

    while (p<end) {
        const char *scan, *ct, *ca, *child_end;
        const char *child_start[64], *child_open_end[64], *child_close_start[64], *child_finish[64];
        int child_ok[64], child_count=0, good=0, depth=0, cc, hi, hl, ht;
        char cn[24], sig[256], sample[128];

        if (*p!='<') {
            const char *q=(const char *)memchr(p,'<',(size_t)(end-p));
            if (!q) q=end;
            if (!append_output(&out,&w,&cap,p,(size_t)(q-p))) goto fail;
            p=q; continue;
        }
        te=find_tag_end(p+1,end);
        if (!te) {
            if (!append_output(&out,&w,&cap,p,(size_t)(end-p))) goto fail;
            p=end; break;
        }
        if (!card_diag_tag_name(p,te,name,sizeof(name),&closing,&attrs) ||
            closing || _stricmp(name,"div")) {
            if (!append_output(&out,&w,&cap,p,(size_t)(te+1-p))) goto fail;
            p=te+1; continue;
        }
        parent_end=card_diag_matching_close(p,te,end,"div");
        if (!parent_end) {
            if (!append_output(&out,&w,&cap,p,(size_t)(te+1-p))) goto fail;
            p=te+1; continue;
        }

        /* Collect direct child DIVs only. */
        scan=te+1; depth=0;
        while (scan<parent_end-1 && child_count<64) {
            scan=(const char *)memchr(scan,'<',(size_t)((parent_end-1)-scan));
            if (!scan) break;
            ct=find_tag_end(scan+1,parent_end-1);
            if (!ct) break;
            if (card_diag_tag_name(scan,ct,cn,sizeof(cn),&cc,&ca) && !_stricmp(cn,"div")) {
                if (!cc) {
                    if (depth==0) {
                        child_end=card_diag_matching_close(scan,ct,parent_end-1,"div");
                        if (!child_end) break;
                        child_start[child_count]=scan;
                        child_open_end[child_count]=ct;
                        child_close_start[child_count]=child_end;
                        while (child_close_start[child_count]>ct &&
                               child_close_start[child_count][-1]!='<')
                            --child_close_start[child_count];
                        if (child_close_start[child_count]>ct &&
                            child_close_start[child_count][-1]=='<')
                            --child_close_start[child_count];
                        child_finish[child_count]=child_end;
                        sig[0]=sample[0]=0;
                        child_ok[child_count]=
                            card_diag_item_signature(ct+1,child_close_start[child_count],
                                                     sig,sizeof(sig),sample,sizeof(sample),
                                                     &hi,&hl,&ht) && hi && hl && ht;
                        if (child_ok[child_count]) ++good;
                        ++child_count;
                        scan=child_end;
                        continue;
                    }
                    ++depth;
                } else if (depth>0) --depth;
            }
            scan=ct+1;
        }

        /* Conservative rule: at least 3 direct children and every one is a
           card-like IMG + A + text item.  Mixed containers are untouched. */
        if (child_count>=3 && good==child_count) {
            int i, card_w=0, this_w;
            const char *q;
            char open_group[260], open_card[220], script[2600];

            for (i=0;i<child_count;i++) {
                this_w=legacy_card_half_width(child_open_end[i]+1,
                                               child_close_start[i]);
                if (this_w>card_w) card_w=this_w;
            }
            if (card_w<80) card_w=200;

            sprintf(open_group,
                "<div data-nos-card-group=\"1\" data-nos-card-count=\"%d\" "
                "data-nos-card-width=\"%d\" style=\"display:block;clear:both;width:100%%\">",
                child_count,card_w);
            if (!append_output(&out,&w,&cap,open_group,strlen(open_group))) goto fail;

            for (i=0;i<child_count;i++) {
                sprintf(open_card,
                    "<div data-nos-card-wrap=\"1\" style=\"float:left;display:block;width:%dpx\">",
                    card_w);
                if (!append_output(&out,&w,&cap,open_card,strlen(open_card))) goto fail;
                q=child_open_end[i]+1;
                if (child_close_start[i]>q &&
                    !append_legacy_card_inner(&out,&w,&cap,q,
                                              child_close_start[i])) goto fail;
                if (!append_output(&out,&w,&cap,"</div>",6)) goto fail;
            }
            if (!append_output(&out,&w,&cap,"<br clear=\"all\"></div>",
                               strlen("<br clear=\"all\"></div>"))) goto fail;

            /* Trusted NOS-Gate layout only. Source-site scripts are still removed.
               Old-IE compatible: actual content width is known only in the browser.
               Try all cards in one row; if that would require <50%, reduce columns.
               Never enlarge above 100%. */
            sprintf(script,
                "<script type=\"text/javascript\" data-nos-layout=\"1\">(function(){"
                "var ds=document.getElementsByTagName('div'),g=null,i,j,a=[],n=%d,ow=%d;"
                "for(i=ds.length-1;i>=0;i--){if(ds[i].getAttribute('data-nos-card-group')=='1'){g=ds[i];break;}}"
                "if(!g)return;for(i=0;i<g.childNodes.length;i++){var x=g.childNodes[i];"
                "if(x.nodeType==1&&x.getAttribute&&x.getAttribute('data-nos-card-wrap')=='1')a[a.length]=x;}"
                "var vw=g.clientWidth||document.body.clientWidth||800;if(vw>24)vw-=16;"
                "var c=a.length,p=100;while(c>1){p=Math.floor(vw*100/(c*ow));if(p>=50)break;c--;}"
                "if(p>100)p=100;if(p<50)p=50;var nw=Math.floor(ow*p/100);"
                "for(i=0;i<a.length;i++){a[i].style.width=nw+'px';a[i].style.cssFloat='left';a[i].style.styleFloat='left';"
                "var im=a[i].getElementsByTagName('img');for(j=0;j<im.length;j++){"
                "var iw=parseInt(im[j].getAttribute('width'),10),ih=parseInt(im[j].getAttribute('height'),10);"
                "if(iw>0&&ih>0){im[j].style.width=Math.floor(iw*p/100)+'px';im[j].style.height=Math.floor(ih*p/100)+'px';"
                "im[j].width=Math.floor(iw*p/100);im[j].height=Math.floor(ih*p/100);}}}"
                "if(window.console&&console.log)console.log('NOS cards '+a.length+' cols '+c+' scale '+p+'%% width '+vw);"
                "})();</script>", child_count,card_w);
            if (!append_output(&out,&w,&cap,script,strlen(script))) goto fail;

            ++groups;
            sprintf(note,
                    "converted DIV group: cards=%d flow=window-scale original-width=%dpx mode=%s",
                    child_count,card_w,mode==MODE_LIGHT?"LITE":"SUPERLITE");
            log_line("LEGACY-CARD",note);
            p=parent_end;
            continue;
        }

        if (!append_output(&out,&w,&cap,p,(size_t)(te+1-p))) goto fail;
        p=te+1;
    }
    *w=0;
    if (new_length) *new_length=(size_t)(w-out);
    if (groups_done) *groups_done=groups;
    return out;
fail:
    free(out);
    return NULL;
}

static int lite_image_dimensions(const unsigned char *b, size_t n, int *w, int *h)
{
    size_t p;
    if (n >= 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) {
        *w=(int)((b[16]<<24)|(b[17]<<16)|(b[18]<<8)|b[19]);
        *h=(int)((b[20]<<24)|(b[21]<<16)|(b[22]<<8)|b[23]);
        return *w>0 && *h>0;
    }
    if (n >= 10 && !memcmp(b, "GIF8", 4)) {
        *w=(int)(b[6] | (b[7]<<8)); *h=(int)(b[8] | (b[9]<<8));
        return *w>0 && *h>0;
    }
    if (n >= 4 && b[0]==0xff && b[1]==0xd8) {
        p=2;
        while (p+9<n) {
            unsigned int len, marker;
            if (b[p]!=0xff) { ++p; continue; }
            while (p<n && b[p]==0xff) ++p;
            if (p>=n) break;
            marker=b[p];
            if (marker==0xd8 || marker==0xd9) { ++p; continue; }
            if (p+2>=n) break;
            len=(unsigned int)((b[p+1]<<8)|b[p+2]);
            if (len<2 || p+1+len>n) break;
            if ((marker>=0xc0 && marker<=0xc3) ||
                (marker>=0xc5 && marker<=0xc7) ||
                (marker>=0xc9 && marker<=0xcb) ||
                (marker>=0xcd && marker<=0xcf)) {
                *h=(int)((b[p+4]<<8)|b[p+5]);
                *w=(int)((b[p+6]<<8)|b[p+7]);
                return *w>0 && *h>0;
            }
            p += 1 + len;
        }
    }
    return 0;
}

static int lite_geometry_candidate(const char *attributes, const char *tag_end,
                                   const char *base, char *absolute, size_t absolute_size)
{
    char src[2048], tmp[256];
    const char *dot;
    if (find_tag_attribute(attributes, tag_end, "width", tmp, sizeof(tmp)) ||
        find_tag_attribute(attributes, tag_end, "height", tmp, sizeof(tmp)) ||
        find_tag_attribute(attributes, tag_end, "class", tmp, sizeof(tmp)) ||
        find_tag_attribute(attributes, tag_end, "id", tmp, sizeof(tmp)) ||
        find_tag_attribute(attributes, tag_end, "style", tmp, sizeof(tmp)))
        return 0;
    if (!find_tag_attribute(attributes, tag_end, "src", src, sizeof(src)) ||
        !resolve_url(base, src, absolute, absolute_size)) return 0;
    dot=strrchr(absolute,'.');
    if (!dot) return 0;
    return starts_ci(dot,".jpg") || starts_ci(dot,".jpeg") ||
           starts_ci(dot,".png") || starts_ci(dot,".gif");
}

static int lite_probe_image_geometry(const char *url, const char *referer,
                                     int *w, int *h)
{
    volatile int cancel=0;
    nw_http_result r;
    char error[512], note[1200];
    char cookie[16384];
    cookies_for(url, cookie, sizeof(cookie));
    memset(&r,0,sizeof(r));
    if (!nw_http_request(url, "GET", NULL, 0, NULL,
                         referer && referer[0] ? referer : NULL,
                         cookie[0] ? cookie : NULL, &r, &cancel, error, sizeof(error))) {
        sprintf(note,"probe failed %.700s | %.350s",url,error);
        log_line("IMAGE-GEOMETRY",note);
        return 0;
    }
    if (r.status>=200 && r.status<300 &&
        lite_image_dimensions((const unsigned char *)r.body,r.body_size,w,h)) {
        sprintf(note,"probe %.700s => %dx%d (%lu bytes)",
                url,*w,*h,(unsigned long)r.body_size);
        log_line("IMAGE-GEOMETRY",note);
        nw_http_result_free(&r);
        return 1;
    }
    sprintf(note,"probe no-size %.700s | HTTP %d %.120s %lu bytes",
            url,r.status,r.content_type,(unsigned long)r.body_size);
    log_line("IMAGE-GEOMETRY",note);
    nw_http_result_free(&r);
    return 0;
}


static const char *media_find_div_close(const char *p, const char *end)
{
    while (p+6<=end) {
        if (p[0]=='<' && p[1]=='/' &&
            (p[2]=='d'||p[2]=='D') && (p[3]=='i'||p[3]=='I') &&
            (p[4]=='v'||p[4]=='V') && p[5]=='>') return p;
        ++p;
    }
    return NULL;
}


/* Media structure classifier.
   Passive only: groups obvious PLAYER slots and separates PLAYER/TRAILER.
   It deliberately does not follow/fetch player URLs yet. */
static void detect_media_structure(const char *html, size_t length, int mode)
{
    const char *p=html, *end=html+length, *te, *attrs;
    int closing;
    int blocks=0, players=0, trailers=0, unknown_iframes=0;
    int player_depth=0, trailer_depth=0;
    char src[1024], id[256], cls[256], params[1024], title[256], note[1800];
    char stack_name[64][16];
    unsigned char stack_player[64], stack_trailer[64];
    int sp=0;

    while (p<end) {
        p=(const char *)memchr(p,'<',(size_t)(end-p));
        if (!p) break;
        te=find_tag_end(p+1,end);
        if (!te) break;

        src[0]=id[0]=cls[0]=params[0]=title[0]=0;

        if (tag_is(p,te,"div",&closing,&attrs)) {
            if (closing) {
                if (sp>0) {
                    --sp;
                    if (stack_player[sp] && player_depth>0) --player_depth;
                    if (stack_trailer[sp] && trailer_depth>0) --trailer_depth;
                }
            } else {
                int strong_player=0, strong_trailer=0, top_block=0;
                find_tag_attribute(attrs,te,"id",id,sizeof(id));
                find_tag_attribute(attrs,te,"class",cls,sizeof(cls));
                find_tag_attribute(attrs,te,"data-params",params,sizeof(params));

                strong_trailer =
                    contains_ci(id,"trailer") || contains_ci(cls,"trailer") ||
                    contains_ci(id,"trl2") || contains_ci(cls,"trl2");

                strong_player =
                    contains_ci(params,"action=iframe") ||
                    contains_ci(params,"-player") ||
                    contains_ci(id,"b-player") || contains_ci(cls,"b-player") ||
                    contains_ci(id,"inner-page__player") ||
                    contains_ci(cls,"inner-page__player");

                /* Count only the outer media block, not controls/wrappers below it. */
                if ((strong_player || strong_trailer) &&
                    player_depth==0 && trailer_depth==0) {
                    top_block=1;
                    ++blocks;
                    sprintf(note,
                            "MEDIA-BLOCK #%d id=%.160s class=%.240s mode=%s",
                            blocks,id[0]?id:"-",cls[0]?cls:"-",
                            mode==MODE_LIGHT?"LITE":"SUPERLITE");
                    log_line("MEDIA-STRUCT",note);
                }

                if (params[0] &&
                    (contains_ci(params,"action=iframe") ||
                     contains_ci(params,"-player"))) {
                    ++players;
                    sprintf(note,
                            "PLAYER #%d via=slot params=%.1000s",
                            players,params);
                    log_line("MEDIA-STRUCT",note);
                }

                if (sp<64) {
                    strcpy(stack_name[sp],"div");
                    stack_player[sp]=(unsigned char)(strong_player?1:0);
                    stack_trailer[sp]=(unsigned char)(strong_trailer?1:0);
                    ++sp;
                    if (strong_player) ++player_depth;
                    if (strong_trailer) ++trailer_depth;
                }
            }
        } else if (tag_is(p,te,"iframe",&closing,&attrs) && !closing) {
            int is_trailer=0, is_player=0;
            find_tag_attribute(attrs,te,"src",src,sizeof(src));
            find_tag_attribute(attrs,te,"id",id,sizeof(id));
            find_tag_attribute(attrs,te,"class",cls,sizeof(cls));
            find_tag_attribute(attrs,te,"title",title,sizeof(title));

            /* Parent context wins: an iframe inside trl2/trailer is a trailer,
               even if its own URL merely says youtube/embed. */
            if (trailer_depth>0 ||
                contains_ci(src,"trailer") || contains_ci(id,"trailer") ||
                contains_ci(cls,"trailer") || contains_ci(title,"trailer")) {
                is_trailer=1;
            }

            if (!is_trailer &&
                (player_depth>0 ||
                 contains_ci(src,"/embed/movie/") ||
                 contains_ci(src,"/movie/") ||
                 contains_ci(src,"/vod/") ||
                 contains_ci(src,"videobalanser") ||
                 contains_ci(src,"ashdi") ||
                 contains_ci(src,"kodik") ||
                 contains_ci(id,"player") || contains_ci(cls,"player") ||
                 contains_ci(title,"player") || contains_ci(title,"xfplayer") ||
                 contains_ci(id,"xfplayer") || contains_ci(cls,"xfplayer"))) {
                is_player=1;
            }

            if (is_trailer) {
                ++trailers;
                sprintf(note,
                        "TRAILER #%d src=%.1000s id=%.120s class=%.120s title=%.120s",
                        trailers,src[0]?src:"(empty)",id[0]?id:"-",
                        cls[0]?cls:"-",title[0]?title:"-");
                log_line("MEDIA-STRUCT",note);
            } else if (is_player) {
                ++players;
                sprintf(note,
                        "PLAYER #%d via=iframe src=%.1000s id=%.120s class=%.120s title=%.120s",
                        players,src[0]?src:"(empty)",id[0]?id:"-",
                        cls[0]?cls:"-",title[0]?title:"-");
                log_line("MEDIA-STRUCT",note);
            } else {
                ++unknown_iframes;
                sprintf(note,
                        "IFRAME-UNKNOWN #%d src=%.1000s id=%.120s class=%.120s",
                        unknown_iframes,src[0]?src:"(empty)",
                        id[0]?id:"-",cls[0]?cls:"-");
                log_line("MEDIA-STRUCT",note);
            }
        }
        p=te+1;
    }

    sprintf(note,
            "summary blocks=%d players=%d trailers=%d unknown-iframes=%d mode=%s",
            blocks,players,trailers,unknown_iframes,
            mode==MODE_LIGHT?"LITE":"SUPERLITE");
    log_line("MEDIA-STRUCT",note);
}

/* Passive media detector: diagnostics only, no fetches and no HTML changes. */
static void detect_media_containers(const char *html, size_t length, int mode)
{
    const char *p=html, *end=html+length, *te, *attrs;
    int closing, direct=0, iframe=0, dynamic=0;
    char src[1024], id[256], cls[256], note[1600];

    while (p<end) {
        p=(const char *)memchr(p,'<',(size_t)(end-p));
        if (!p) break;
        te=find_tag_end(p+1,end);
        if (!te) break;
        src[0]=id[0]=cls[0]=0;

        if (tag_is(p,te,"video",&closing,&attrs) && !closing) {
            find_tag_attribute(attrs,te,"src",src,sizeof(src));
            sprintf(note,"type=DIRECT tag=video src=%.900s mode=%s",
                    src[0]?src:"(inline/unknown)",mode==MODE_LIGHT?"LITE":"SUPERLITE");
            log_line("MEDIA-DETECT",note); ++direct;
        } else if (tag_is(p,te,"audio",&closing,&attrs) && !closing) {
            find_tag_attribute(attrs,te,"src",src,sizeof(src));
            sprintf(note,"type=DIRECT tag=audio src=%.900s mode=%s",
                    src[0]?src:"(inline/unknown)",mode==MODE_LIGHT?"LITE":"SUPERLITE");
            log_line("MEDIA-DETECT",note); ++direct;
        } else if (tag_is(p,te,"source",&closing,&attrs) && !closing) {
            find_tag_attribute(attrs,te,"src",src,sizeof(src));
            sprintf(note,"type=DIRECT tag=source src=%.900s mode=%s",
                    src[0]?src:"(unknown)",mode==MODE_LIGHT?"LITE":"SUPERLITE");
            log_line("MEDIA-DETECT",note); ++direct;
        } else if (tag_is(p,te,"iframe",&closing,&attrs) && !closing) {
            find_tag_attribute(attrs,te,"src",src,sizeof(src));
            find_tag_attribute(attrs,te,"id",id,sizeof(id));
            find_tag_attribute(attrs,te,"class",cls,sizeof(cls));
            sprintf(note,"type=IFRAME src=%.850s id=%.120s class=%.120s mode=%s",
                    src[0]?src:"(empty)",id[0]?id:"-",cls[0]?cls:"-",
                    mode==MODE_LIGHT?"LITE":"SUPERLITE");
            log_line("MEDIA-DETECT",note); ++iframe;
        } else if (tag_is(p,te,"div",&closing,&attrs) && !closing) {
            const char *after=te+1, *close;
            find_tag_attribute(attrs,te,"id",id,sizeof(id));
            find_tag_attribute(attrs,te,"class",cls,sizeof(cls));
            close=media_find_div_close(after,end);
            if (close && close-after<256 &&
                (contains_ci(id,"player") || contains_ci(id,"video") ||
                 contains_ci(id,"media") || contains_ci(id,"gidonline") ||
                 contains_ci(cls,"player") || contains_ci(cls,"video") ||
                 contains_ci(cls,"media") || contains_ci(cls,"tray"))) {
                const char *q=after; int empty=1;
                while (q<close) {
                    if (*q==' ' || *q=='\t' || *q=='\r' || *q=='\n') { ++q; continue; }
                    empty=0; break;
                }
                if (empty) {
                    sprintf(note,"type=DYNAMIC empty-div id=%.180s class=%.180s mode=%s",
                            id[0]?id:"-",cls[0]?cls:"-",
                            mode==MODE_LIGHT?"LITE":"SUPERLITE");
                    log_line("MEDIA-DETECT",note); ++dynamic;
                }
            }
        } else if (tag_is(p,te,"script",&closing,&attrs) && !closing) {
            find_tag_attribute(attrs,te,"src",src,sizeof(src));
            if (src[0] && (contains_ci(src,"player") || contains_ci(src,"video") ||
                           contains_ci(src,"embed") || contains_ci(src,"replace"))) {
                sprintf(note,"type=DYNAMIC script=%.900s mode=%s",src,
                        mode==MODE_LIGHT?"LITE":"SUPERLITE");
                log_line("MEDIA-DETECT",note); ++dynamic;
            }
        }
        p=te+1;
    }
    if (contains_ci(html,".m3u8") || contains_ci(html,".mpd") ||
        contains_ci(html,".mp4") || contains_ci(html,".webm") ||
        contains_ci(html,".m4a") || contains_ci(html,".mp3")) {
        log_line("MEDIA-DETECT","type=DIRECT media-url-signature present"); ++direct;
    }
    if (contains_ci(html,"synchroncode") || contains_ci(html,"videobalanser") ||
        contains_ci(html,"ashdi")) {
        log_line("MEDIA-DETECT","type=IFRAME known-embed-family present"); ++iframe;
    }
    sprintf(note,"summary direct=%d iframe=%d dynamic=%d mode=%s",
            direct,iframe,dynamic,mode==MODE_LIGHT?"LITE":"SUPERLITE");
    log_line("MEDIA-DETECT",note);
}


#define PAGE_MEDIA_MAX 16
#define PAGE_MEDIA_PLAYER 1
#define PAGE_MEDIA_TRAILER 2
#define PAGE_MEDIA_DYNAMIC 3

typedef struct page_media_item {
    int type;
    int active;
    char label[96];
    char url[4096];
    char params[1024];
} page_media_item;

typedef struct page_media_list {
    int count;
    page_media_item item[PAGE_MEDIA_MAX];
} page_media_list;

static int page_media_duplicate(page_media_list *list, const char *url,
                                const char *params)
{
    int i;
    for (i=0; i<list->count; ++i) {
        if (url && url[0] && list->item[i].url[0] &&
            !_stricmp(list->item[i].url,url)) return 1;
        if (params && params[0] && list->item[i].params[0] &&
            !_stricmp(list->item[i].params,params)) return 1;
    }
    return 0;
}

static void page_media_add(page_media_list *list, int type, int active,
                           const char *label, const char *url,
                           const char *params)
{
    page_media_item *item;
    char note[1400];
    if (!list || list->count>=PAGE_MEDIA_MAX) return;
    if (page_media_duplicate(list,url,params)) return;
    item=&list->item[list->count++];
    memset(item,0,sizeof(*item));
    item->type=type; item->active=active;
    if (label && label[0]) {
        strncpy(item->label,label,sizeof(item->label)-1);
        item->label[sizeof(item->label)-1]=0;
    }
    if (url && url[0]) {
        strncpy(item->url,url,sizeof(item->url)-1);
        item->url[sizeof(item->url)-1]=0;
    }
    if (params && params[0]) {
        strncpy(item->params,params,sizeof(item->params)-1);
        item->params[sizeof(item->params)-1]=0;
    }
    sprintf(note,"add type=%d active=%d label=%.80s url=%.900s params=%.300s",
            type,active,item->label,item->url,item->params);
    log_line("MEDIA-LIST",note);
}

static void collect_page_media(const char *html, size_t length,
                               const char *base, page_media_list *list)
{
    const char *p=html, *end=html+length, *te, *attrs;
    int closing, trailer_depth=0, player_depth=0, sp=0;
    unsigned char stack_player[64], stack_trailer[64];
    char src[4096], absolute[4096], id[256], cls[256], title[256];
    char params[1024], label[96];
    if (!list) return;
    memset(list,0,sizeof(*list));

    while (p<end) {
        p=(const char *)memchr(p,'<',(size_t)(end-p));
        if (!p) break;
        te=find_tag_end(p+1,end);
        if (!te) break;

        src[0]=absolute[0]=id[0]=cls[0]=title[0]=params[0]=label[0]=0;

        if (tag_is(p,te,"div",&closing,&attrs)) {
            if (closing) {
                if (sp>0) {
                    --sp;
                    if (stack_player[sp] && player_depth>0) --player_depth;
                    if (stack_trailer[sp] && trailer_depth>0) --trailer_depth;
                }
            } else {
                int strong_player, strong_trailer;
                find_tag_attribute(attrs,te,"id",id,sizeof(id));
                find_tag_attribute(attrs,te,"class",cls,sizeof(cls));
                find_tag_attribute(attrs,te,"data-params",params,sizeof(params));

                strong_trailer=contains_ci(id,"trailer") ||
                    contains_ci(cls,"trailer") || contains_ci(id,"trl2") ||
                    contains_ci(cls,"trl2");
                strong_player=contains_ci(params,"action=iframe") ||
                    contains_ci(params,"-player") ||
                    contains_ci(id,"b-player") || contains_ci(cls,"b-player") ||
                    contains_ci(id,"inner-page__player") ||
                    contains_ci(cls,"inner-page__player");

                if (params[0] && (contains_ci(params,"action=iframe") ||
                                  contains_ci(params,"-player"))) {
                    const char *m;
                    strcpy(label,"Dynamic player");
                    m=strstr(params,"mod=");
                    if (m) {
                        size_t n=0; m+=4;
                        while (m[n] && m[n]!='&' && n<sizeof(label)-1) {
                            label[n]=m[n]; ++n;
                        }
                        label[n]=0;
                    }
                    page_media_add(list,PAGE_MEDIA_DYNAMIC,0,label,NULL,params);
                }

                if (sp<64) {
                    stack_player[sp]=(unsigned char)(strong_player?1:0);
                    stack_trailer[sp]=(unsigned char)(strong_trailer?1:0);
                    ++sp;
                    if (strong_player) ++player_depth;
                    if (strong_trailer) ++trailer_depth;
                }
            }
        } else if (tag_is(p,te,"iframe",&closing,&attrs) && !closing) {
            int is_trailer=0, is_player=0;
            const char *provider;
            find_tag_attribute(attrs,te,"src",src,sizeof(src));
            if (!src[0])
                find_tag_attribute(attrs,te,"data-src",src,sizeof(src));
            find_tag_attribute(attrs,te,"id",id,sizeof(id));
            find_tag_attribute(attrs,te,"class",cls,sizeof(cls));
            find_tag_attribute(attrs,te,"title",title,sizeof(title));
            if (src[0]) {
                if (!resolve_url(base,src,absolute,sizeof(absolute))) {
                    strncpy(absolute,src,sizeof(absolute)-1);
                    absolute[sizeof(absolute)-1]=0;
                }
                is_trailer=trailer_depth>0 || contains_ci(absolute,"trailer") ||
                    contains_ci(id,"trailer") || contains_ci(cls,"trailer") ||
                    contains_ci(title,"trailer");
                provider=iframe_provider(absolute);
                if (!is_trailer &&
                    (player_depth>0 || probable_media_iframe(attrs,te,absolute,provider)))
                    is_player=1;
                if (is_trailer || is_player) {
                    if (_stricmp(provider,"Unknown player"))
                        strncpy(label,provider,sizeof(label)-1);
                    else if (title[0])
                        strncpy(label,title,sizeof(label)-1);
                    else
                        strcpy(label,is_trailer?"Trailer":"Player");
                    label[sizeof(label)-1]=0;
                    page_media_add(list,
                        is_trailer?PAGE_MEDIA_TRAILER:PAGE_MEDIA_PLAYER,
                        1,label,absolute,NULL);
                }
            }
        }
        p=te+1;
    }
}


static int youtube_video_id(const char *url, char *id, size_t id_size)
{
    const char *p, *e;
    size_t n;
    if (!url || !id || id_size < 2) return 0;
    id[0]=0;
    if (!(contains_ci(url,"youtube.com") || contains_ci(url,"youtu.be")))
        return 0;
    p=strstr(url,"/embed/");
    if (p) p+=7;
    else {
        p=strstr(url,"/watch/");
        if (p) p+=7;
        else {
            p=strstr(url,"youtu.be/");
            if (p) p+=9;
            else {
                p=strstr(url,"v=");
                if (p) p+=2;
            }
        }
    }
    if (!p || !*p) return 0;
    e=p;
    while (*e && *e!='?' && *e!='&' && *e!='/' && *e!='#' &&
           *e!='"' && *e!='\'' && *e!=' ') ++e;
    n=(size_t)(e-p);
    if (!n || n>=id_size) return 0;
    memcpy(id,p,n); id[n]=0;
    return 1;
}

static int launch_nospipe_search(const char *query)
{
#ifdef _WIN32
    char jar[MAX_PATH], command[4096], dir[MAX_PATH], *slash;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    int created;

    if (!query || !query[0]) return 0;
    strcpy(jar,"C:\\Program Files\\NOS-Pipe\\NOS-Pipe.jar");
    if (!file_exists(jar)) {
        module_side_path(dir,sizeof(dir),"NOS-Gate.exe");
        slash=strrchr(dir,'\\');
        if (slash) {
            *slash=0;
            slash=strrchr(dir,'\\');
            if (slash)
                sprintf(jar,"%.900s\\NOS-Pipe\\NOS-Pipe.jar",dir);
        }
    }
    if (!file_exists(jar)) return 0;

    sprintf(command,
        "javaw -cp \"%.900s\" notpipe.gui.NOSPipeGui \"%.512s\"",
        jar,query);
    memset(&si,0,sizeof(si)); si.cb=sizeof(si);
    memset(&pi,0,sizeof(pi));
    created=CreateProcess(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi);
    if (created) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }
#endif
    return 0;
}

static void serve_nospipe_youtube(SOCKET s, const char *encoded)
{
    char query[512], page[1200], safe[600];
    url_decode(query, sizeof(query), encoded ? encoded : "");
    html_escape_text(query,safe,sizeof(safe));
    if (launch_nospipe_search(query)) {
        sprintf(page,
            "<html><body><b>NOS-Pipe:</b> search sent: %.500s"
            "<p><a href=\"javascript:history.back()\">Back</a></p></body></html>",
            safe);
        send_response(s,"200 OK","text/html; charset=UTF-8",page,strlen(page));
    } else {
        sprintf(page,
            "<html><body><b>NOS-Pipe not found.</b>"
            "<p>Expected: C:\\Program Files\\NOS-Pipe\\NOS-Pipe.jar"
            " or sibling NOS-Pipe folder.</p>"
            "<p>Search text: %.500s</p></body></html>",safe);
        send_response(s,"200 OK","text/html; charset=UTF-8",page,strlen(page));
    }
}

static int append_visible_media_control(char **out, char **write,
                                        size_t *capacity,
                                        const page_media_list *list,
                                        const char *resource_route)
{
    int i, player_no=0, trailer_no=0;
    char line[512], safe[256], note[256];
    static const char begin[] =
        "<table data-nos-media=\"1\" width=\"100%\" border=\"1\" cellpadding=\"4\" cellspacing=\"0\" "
        "bgcolor=\"#F0F0F0\" style=\"display:block!important;visibility:visible!important;"
        "position:relative!important;z-index:32767!important;clear:both!important;float:none!important;"
        "width:100%!important;height:auto!important;margin:0!important;padding:0!important;\">"
        "<tr><td bgcolor=\"#F0F0F0\" style=\"display:block!important;visibility:visible!important;"
        "position:static!important;float:none!important;width:auto!important;height:auto!important;"
        "padding:4px!important;margin:0!important;\"><font face=\"Arial\" size=\"2\">"
        "<b>NOS VIDEO:</b> ";
    static const char end[] = "</font></td></tr></table>";

    if (!list || list->count<=0) return 1;
    if (!append_output(out,write,capacity,begin,sizeof(begin)-1)) return 0;

    for (i=0; i<list->count; ++i) {
        const page_media_item *item=&list->item[i];
        if (item->type==PAGE_MEDIA_TRAILER) ++trailer_no;
        else ++player_no;
        html_escape_text(item->label[0]?item->label:
                         (item->type==PAGE_MEDIA_TRAILER?"Trailer":"Player"),
                         safe,sizeof(safe));

        if (item->active && item->url[0]) {
            char youtube_id[128];
            sprintf(line,"<span style=\"white-space:nowrap\">[<a target=\"content\" href=\"");
            if (!append_output(out,write,capacity,line,strlen(line)))
                return 0;
            if (youtube_video_id(item->url,youtube_id,sizeof(youtube_id))) {
                if (!append_output(out,write,capacity,
                                   "/nospipe/youtube/",17) ||
                    !append_output(out,write,capacity,
                                   youtube_id,strlen(youtube_id)))
                    return 0;
            } else if (!append_routed(out,write,capacity,item->url,resource_route))
                return 0;
            if (item->type==PAGE_MEDIA_TRAILER)
                sprintf(line,"\">Trailer %d: %.180s</a>]</span> ",trailer_no,safe);
            else
                sprintf(line,"\">Player %d: %.180s</a>]</span> ",player_no,safe);
            if (!append_output(out,write,capacity,line,strlen(line))) return 0;
        } else {
            if (item->type==PAGE_MEDIA_TRAILER)
                sprintf(line,"<span>[Trailer %d: %.180s (dynamic)]</span> ",
                        trailer_no,safe);
            else
                sprintf(line,"<span>[Player %d: %.180s (dynamic)]</span> ",
                        player_no,safe);
            if (!append_output(out,write,capacity,line,strlen(line))) return 0;
        }
    }
    if (!append_output(out,write,capacity,end,sizeof(end)-1)) return 0;
    sprintf(note,"visible control items=%d",list->count);
    log_line("MEDIA-CONTROL",note);
    return 1;
}

static char *rewrite_html(const char *html, size_t length, const char *base,
                          const char *resource_route, int mode, size_t *out_length)
{
    const char *p = html, *end = html + length, *tag_end, *raw_end;
    const char *attributes;
    char *out, *w, note[256];
    size_t capacity = length * 2 + 4096;
    int closing, raw_script = 0, raw_style = 0;
    int is_script, is_style, is_link, is_svg, is_video, is_iframe;
    int is_source, is_img, is_input, is_button, is_form, is_body, is_head;
    int processed, super_lite, youtube_lite;
    int removed_svg = 0, removed_media = 0, removed_images = 0, removed_empty_links = 0;
    int layout_diag_images = 0;
    int geometry_fixed = 0, geometry_skipped = 0, geometry_probes = 0;
    int repaired_search = 0, restored_buttons = 0, normalized_forms = 0;
    int inside_search_form = 0, search_form_have_submit = 0;
    int google_search_added = 0;
    int base_target_added = 0;
    char *legacy_html = NULL;
    size_t legacy_length = 0;
    int legacy_groups = 0;
    const char *nav_route = mode_entry_route(mode);
    search_form_info form_info;
    page_media_list page_media;
    int media_control_added = 0;
    memset(&page_media,0,sizeof(page_media));
    processed = mode != MODE_ORIGINAL;
    super_lite = mode == MODE_SUPERLITE;
    youtube_lite = super_lite && youtube_page(base);
    if (processed) {
        detect_media_containers(html, length, mode);
        detect_media_structure(html, length, mode);
        collect_page_media(html, length, base, &page_media);
        card_candidate_diag(html, end, mode);
        legacy_html = legacy_card_layout(html, length, mode, &legacy_length, &legacy_groups);
        if (legacy_html && legacy_groups > 0) {
            html = legacy_html;
            length = legacy_length;
            p = html;
            end = html + length;
            capacity = length * 2 + 4096;
        } else if (legacy_html) {
            free(legacy_html);
            legacy_html = NULL;
        }
    }
    out = (char *)malloc(capacity);
    if (!out) return NULL;
    w = out;
    while (p < end) {
        if (raw_script || raw_style) {
            const char *needle = raw_script ? "</script" : "</style";
            raw_end = p;
            while (raw_end < end && !starts_ci_bounded(raw_end, end, needle)) ++raw_end;
            if (!append_output(&out, &w, &capacity, p, (size_t)(raw_end - p))) goto failed;
            p = raw_end; raw_script = raw_style = 0;
            continue;
        }
        if (*p != '<') {
            const char *text_end = (const char *)memchr(p, '<', (size_t)(end - p));
            if (!text_end) text_end = end;
            if (!append_output(&out, &w, &capacity, p, (size_t)(text_end - p))) goto failed;
            p = text_end; continue;
        }
        if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char *comment_end = p + 4;
            while (comment_end + 2 < end && memcmp(comment_end, "-->", 3)) ++comment_end;
            if (comment_end + 2 < end) comment_end += 3; else comment_end = end;
            if (!processed && !append_output(&out, &w, &capacity, p,
                                        (size_t)(comment_end - p))) goto failed;
            p = comment_end; continue;
        }
        tag_end = find_tag_end(p + 1, end);
        if (!tag_end) {
            if (!append_output(&out, &w, &capacity, p, (size_t)(end - p))) goto failed;
            p = end; break;
        }
        is_script = tag_is(p, tag_end, "script", &closing, &attributes);
        is_script = is_script && !closing;
        {
            char nos_layout_value[16];
            if (is_script && find_tag_attribute(attributes, tag_end,
                                                "data-nos-layout",
                                                nos_layout_value,
                                                sizeof(nos_layout_value)) &&
                !_stricmp(nos_layout_value, "1"))
                is_script = 0; /* keep only NOS-Gate's generated layout script */
        }
        is_style = tag_is(p, tag_end, "style", &closing, &attributes);
        is_style = is_style && !closing;
        is_link = tag_is(p, tag_end, "link", &closing, &attributes);
        is_link = is_link && !closing;
        is_svg = tag_is(p, tag_end, "svg", &closing, &attributes);
        is_svg = is_svg && !closing;
        is_video = tag_is(p, tag_end, "video", &closing, &attributes);
        is_video = is_video && !closing;
        is_iframe = tag_is(p, tag_end, "iframe", &closing, &attributes);
        is_iframe = is_iframe && !closing;
        is_source = tag_is(p, tag_end, "source", &closing, &attributes);
        is_source = is_source && !closing;
        is_img = tag_is(p, tag_end, "img", &closing, &attributes);
        if (mode == MODE_LIGHT && is_img && !closing)
            lite_layout_diag_img(p, tag_end, html, end, base, &layout_diag_images);

        is_img = is_img && !closing;
        is_input = tag_is(p, tag_end, "input", &closing, &attributes);
        is_input = is_input && !closing;
        is_button = tag_is(p, tag_end, "button", &closing, &attributes);
        is_button = is_button && !closing;
        is_form = tag_is(p, tag_end, "form", &closing, &attributes);
        is_body = tag_is(p, tag_end, "body", &closing, &attributes);
        is_head = tag_is(p, tag_end, "head", &closing, &attributes);
        if (processed && is_link) {
            char href[512], absolute[1024];
            if (opensearch_link(p, tag_end, href, sizeof(href))) {
                if (!resolve_url(base, href, absolute, sizeof(absolute)))
                    strncpy(absolute, href, sizeof(absolute) - 1);
                absolute[sizeof(absolute) - 1] = 0;
                sprintf(note, "descriptor %.200s", absolute);
                log_line("SEARCH-OPEN", note);
            }
        }
        if (processed && (is_script || (super_lite && is_style))) {
            const char *needle = is_script ? "</script" : "</style";
            raw_end = tag_end + 1;
            while (raw_end < end && !starts_ci_bounded(raw_end, end, needle)) ++raw_end;
            if (raw_end < end) {
                const char *close_end = find_tag_end(raw_end + 1, end);
                p = close_end ? close_end + 1 : end;
            } else p = end;
            continue;
        }
        if (super_lite && is_link) { p = tag_end + 1; continue; }
        if (mode == MODE_LIGHT) {
            const char *skip_to = NULL;
            if (lite_dangerous_empty_anchor(p, tag_end, end, &skip_to)) {
                ++removed_empty_links;
                p = skip_to;
                continue;
            }
        }
        if (processed && is_iframe) {
            char media_url[4096], note[4600];
            const char *provider;
            int candidate;
            if (iframe_url(attributes, tag_end, base, media_url,
                           sizeof(media_url))) {
                remember_iframe_referer(media_url, base);
                sprintf(note, "iframe=%.2200s parent=%.2200s",
                        media_url, base);
                log_line("REFERER-MAP-SAVE", note);
                provider = iframe_provider(media_url);
                candidate = probable_media_iframe(attributes, tag_end,
                                                  media_url, provider);
                sprintf(note, "provider=%.40s candidate=%s url=%.4000s",
                        provider, candidate ? "yes" : "no", media_url);
                log_line("IFRAME-DETECT", note);
                if (candidate) {
                    static const char media_before[] =
                        "<p><b>Video iframe: ";
                    static const char media_middle[] =
                        "</b> <a target=\"content\" href=\"";
                    static const char controls_after[] =
                        "\">Open video controls</a></p>";
                    static const char player_after[] =
                        "\">Open player page</a></p>";
                    if (!append_output(&out, &w, &capacity, media_before,
                                       sizeof(media_before) - 1) ||
                        !append_output(&out, &w, &capacity, provider,
                                       strlen(provider)) ||
                        !append_output(&out, &w, &capacity, media_middle,
                                       sizeof(media_middle) - 1) ||
                        !append_routed(&out, &w, &capacity, media_url,
                                       resource_route) ||
                        !append_output(&out, &w, &capacity,
                            synchroncode_player(media_url) ? controls_after :
                                                            player_after,
                            synchroncode_player(media_url) ?
                                sizeof(controls_after) - 1 :
                                sizeof(player_after) - 1)) goto failed;
                    sprintf(note, "provider=%.40s link added for %.4000s",
                            provider, media_url);
                    log_line("MEDIA-DETECT", note);
                }
            }
        }
        if (processed && (is_svg || is_video || is_iframe)) {
            const char *needle = is_svg ? "</svg" :
                                 (is_video ? "</video" : "</iframe");
            if (tag_self_closing(p, tag_end)) p = tag_end + 1;
            else {
                raw_end = tag_end + 1;
                while (raw_end < end && !starts_ci_bounded(raw_end, end, needle)) ++raw_end;
                if (raw_end < end) {
                    const char *close_end = find_tag_end(raw_end + 1, end);
                    p = close_end ? close_end + 1 : end;
                } else p = end;
            }
            if (is_svg) ++removed_svg; else ++removed_media;
            continue;
        }
        if (processed && is_source) {
            ++removed_media; p = tag_end + 1; continue;
        }
        if (processed && is_img &&
            unrecoverable_old_ie_image(attributes, tag_end)) {
            ++removed_images; p = tag_end + 1; continue;
        }
        if (youtube_lite && youtube_search_input(p, tag_end)) {
            static const char search_form[] =
                "<form method=\"GET\" action=\"/lite/https%3A%2F%2Fwww.youtube.com%2Fresults\">"
                "<input name=\"search_query\" size=\"36\"> "
                "<input type=\"submit\" value=\"Search\"></form>";
            if (!append_output(&out, &w, &capacity, search_form,
                               sizeof(search_form) - 1)) goto failed;
            ++repaired_search;
            p = tag_end + 1;
            continue;
        }
        if (processed && is_form && !closing) {
            inspect_search_form(p, tag_end, end, &form_info);
            inside_search_form = form_info.found;
            search_form_have_submit = form_info.have_submit;
            if (inside_search_form) {
                sprintf(note, "method=%.12s action=%.120s field=%.60s hidden=%d submit=%s",
                        form_info.method,
                        form_info.action[0] ? form_info.action : "(current page)",
                        form_info.field, form_info.hidden_count,
                        form_info.have_submit ? "yes" : "no");
                log_line("SEARCH-DETECT", note);
                ++normalized_forms;
            }
        }
        if (processed && inside_search_form && is_input) {
            int image_submit = 0;
            if (form_submit_control(p, tag_end, &image_submit) && image_submit) {
                static const char text_submit[] =
                    "<input type=\"submit\" value=\"Search\">";
                if (!append_output(&out, &w, &capacity, text_submit,
                                   sizeof(text_submit) - 1)) goto failed;
                ++restored_buttons;
                p = tag_end + 1;
                continue;
            }
        }
        if (processed && is_form && closing && inside_search_form &&
            !search_form_have_submit) {
            static const char added_submit[] =
                " <input type=\"submit\" value=\"Search\">";
            if (!append_output(&out, &w, &capacity, added_submit,
                               sizeof(added_submit) - 1)) goto failed;
            ++restored_buttons;
        }
        if (processed && is_body && closing && google_page(base) &&
            !normalized_forms && !google_search_added) {
            if (!append_google_search(&out, &w, &capacity, nav_route)) goto failed;
            google_search_added = 1;
            log_line("SEARCH-KNOWN", "Google GET /search field=q fallback added");
        }
        if (!closing && is_body && !base_target_added) {
            static const char missing_head_base[] =
                "<head><base target=\"_top\"></head>";
            if (!append_output(&out, &w, &capacity, missing_head_base,
                               sizeof(missing_head_base) - 1)) goto failed;
            base_target_added = 1;
        }
        {
            int forced_img_w = 0, forced_img_h = 0;
            if (mode == MODE_LIGHT && is_img && !closing) {
                char geometry_url[4096];
            if (lite_geometry_candidate(attributes, tag_end, base,
                                        geometry_url, sizeof(geometry_url))) {
                if (geometry_probes < 8) {
                    ++geometry_probes;
                    if (lite_probe_image_geometry(geometry_url, base,
                                                  &forced_img_w, &forced_img_h))
                        ++geometry_fixed;
                    else ++geometry_skipped;
                } else ++geometry_skipped;
            }
            }
            if (!rewrite_tag(p, tag_end, base, nav_route, resource_route,
                             processed, super_lite, forced_img_w, forced_img_h,
                             &out, &w, &capacity)) goto failed;
        }
        if (processed && is_body && !closing && !media_control_added &&
            page_media.count > 0) {
            if (!append_visible_media_control(&out,&w,&capacity,&page_media,
                                              resource_route)) goto failed;
            media_control_added = 1;
        }
        if (!closing && is_head && !base_target_added) {
            static const char top_base[] = "<base target=\"_top\">";
            if (!append_output(&out, &w, &capacity, top_base,
                               sizeof(top_base) - 1)) goto failed;
            base_target_added = 1;
        }
        if (processed && is_button &&
            (inside_search_form || search_submit_button(p, tag_end))) {
            raw_end = tag_end + 1;
            while (raw_end < end &&
                   !starts_ci_bounded(raw_end, end, "</button")) ++raw_end;
            if (raw_end < end &&
                !html_range_has_visible_text(tag_end + 1, raw_end)) {
                if (!append_output(&out, &w, &capacity, "Search", 6))
                    goto failed;
                ++restored_buttons;
            }
        }
        if (is_script) raw_script = 1;
        if (is_style) raw_style = 1;
        if (processed && is_form && closing) {
            inside_search_form = 0;
            search_form_have_submit = 0;
        }
        p = tag_end + 1;
    }
    if (processed && google_page(base) && !normalized_forms && !google_search_added) {
        if (!append_google_search(&out, &w, &capacity, nav_route)) goto failed;
        google_search_added = 1;
        log_line("SEARCH-KNOWN", "Google GET /search field=q fallback added");
    }
    *w = 0; *out_length = (size_t)(w - out);
    if (processed) {
        sprintf(note, "%s cleanup: SVG=%d, media/source=%d, SVG images=%d, empty/script links=%d, %lu -> %lu bytes",
                mode_name(mode),
                removed_svg, removed_media, removed_images, removed_empty_links,
                (unsigned long)length, (unsigned long)*out_length);
        log_line("LITE-CLEANUP", note);
        if (mode == MODE_LIGHT) {
            sprintf(note, "missing-size IMG candidates=%d; diagnostic only, HTML unchanged",
                    layout_diag_images);
            log_line("LITE-LAYOUT-DIAG", note);
            sprintf(note, "fixed=%d skipped=%d probes=%d; full layout diagnostics retained",
                    geometry_fixed, geometry_skipped, geometry_probes);
            log_line("IMAGE-GEOMETRY", note);
        }
    }
    if (repaired_search) {
        sprintf(note, "Replaced %d script-only YouTube search input(s) with legacy GET form",
                repaired_search);
        log_line("LITE-SEARCH", note);
    }
    if (restored_buttons) {
        sprintf(note, "restored visible text on %d empty search button(s)",
                restored_buttons);
        log_line("LITE-BUTTON", note);
    }
    if (normalized_forms) {
        sprintf(note, "recognized %d native search form(s); method, action and hidden fields preserved",
                normalized_forms);
        log_line("SEARCH-NORMALIZE", note);
    }
    if (legacy_html) free(legacy_html);
    return out;
failed:
    if (legacy_html) free(legacy_html);
    free(out); return NULL;
}

static int fetch_following_redirects(const char *start, const char *method,
                                     const char *request_body,
                                     size_t request_body_size,
                                     const char *request_content_type,
                                     nw_http_result *result,
                                     char *final_url, size_t final_size,
                                     char *error, size_t error_size)
{
    volatile int cancel = 0;
    int redirects, redirect_status;
    const char *current_method = method;
    const char *current_body = request_body;
    size_t current_body_size = request_body_size;
    char next[4096], line[4600], referer[4096];
    char cookie[16384];
    strncpy(final_url, start, final_size - 1); final_url[final_size - 1] = 0;
    if (iframe_referer_for(start, referer, sizeof(referer))) {
        sprintf(line, "iframe=%.2200s parent=%.2200s", start, referer);
        log_line("REFERER-MAP-USE", line);
    } else {
        strncpy(referer, last_page_url, sizeof(referer) - 1);
        referer[sizeof(referer) - 1] = 0;
        if (referer[0]) log_line("REFERER-FALLBACK", referer);
    }
    for (redirects = 0; redirects <= MAX_REDIRECTS; ++redirects) {
        sprintf(line, "%s %.4000s", current_method, final_url); log_line("REQUEST", line);
        cookies_for(final_url, cookie, sizeof(cookie));
        if (cookie[0]) {
            sprintf(line, "sent %d pair(s)", cookie_pair_count(cookie));
            log_line("COOKIE", line);
        }
        if (referer[0]) log_line("REFERER", referer);
        if (!nw_http_request(final_url, current_method, current_body,
                             current_body_size, request_content_type,
                             referer[0] ? referer : NULL, cookie,
                             result, &cancel, error, error_size)) return 0;
        log_response_headers(result);
        store_cookies(final_url, result->set_cookie, result->set_cookie_count);
        sprintf(line, "step %d HTTP %d | %.4000s",
                redirects + 1, result->status, final_url);
        log_line("RESPONSE", line);
        if (result->status < 300 || result->status >= 400 || !result->location[0]) return 1;
        if (!resolve_url(final_url, result->location, next, sizeof(next))) {
            sprintf(line, "step %d HTTP %d | Location=%.3500s | INVALID",
                    redirects + 1, result->status, result->location);
            log_line("REDIRECT", line);
            strncpy(error, "Invalid redirect address", error_size - 1); error[error_size - 1] = 0;
            nw_http_result_free(result); return 0;
        }
        sprintf(line, "step %d HTTP %d | Location=%.1400s | Next=%.2800s",
                redirects + 1, result->status, result->location, next);
        log_line("REDIRECT", line);
        redirect_status = result->status;
        nw_http_result_free(result);
        strncpy(referer, final_url, sizeof(referer) - 1); referer[sizeof(referer) - 1] = 0;
        strncpy(final_url, next, final_size - 1); final_url[final_size - 1] = 0;
        if (redirect_status != 307 && redirect_status != 308) {
            current_method = "GET"; current_body = NULL; current_body_size = 0;
        }
    }
    strncpy(error, "Too many redirects", error_size - 1); error[error_size - 1] = 0;
    return 0;
}

static int supported_old_ie_image_type(const char *type)
{
    return starts_ci(type, "image/jpeg") || starts_ci(type, "image/jpg") ||
           starts_ci(type, "image/png") || starts_ci(type, "image/gif") ||
           starts_ci(type, "image/bmp");
}

static int unsupported_old_ie_image_type(const char *type)
{
    return starts_ci(type, "image/webp") || starts_ci(type, "image/avif");
}

static char *find_extension_ci(char *text, const char *extension)
{
    size_t n = strlen(extension);
    char *p = text;
    while (*p) {
        if (!_strnicmp(p, extension, n)) return p;
        ++p;
    }
    return NULL;
}

static int old_ie_image_candidate(const char *url, char *candidate,
                                  size_t candidate_size)
{
    char *query, *extension;
    if (strlen(url) >= candidate_size) return 0;
    strcpy(candidate, url);
    query = strchr(candidate, '?');
    if (query) *query = 0;
    extension = find_extension_ci(candidate, ".webp");
    if (extension && (!extension[5] || extension[5] == '/' ||
                      extension[5] == '&')) {
        strcpy(extension, ".jpg");
        return _stricmp(candidate, url) != 0;
    }
    if (!query) return 0;
    if (find_extension_ci(candidate, ".jpg") ||
        find_extension_ci(candidate, ".jpeg") ||
        find_extension_ci(candidate, ".png") ||
        find_extension_ci(candidate, ".gif")) return 1;
    return 0;
}

static image_retry_host *image_retry_host_for(const char *url)
{
    char host[256];
    int i, empty = -1;
    if (!url_host(url, host, sizeof(host))) return NULL;
    for (i = 0; i < MAX_IMAGE_RETRY_HOSTS; ++i) {
        if (!image_retry_hosts[i].host[0]) {
            if (empty < 0) empty = i;
        } else if (!_stricmp(image_retry_hosts[i].host, host))
            return &image_retry_hosts[i];
    }
    if (empty < 0) return NULL;
    strncpy(image_retry_hosts[empty].host, host,
            sizeof(image_retry_hosts[empty].host) - 1);
    image_retry_hosts[empty].host[sizeof(image_retry_hosts[empty].host) - 1] = 0;
    return &image_retry_hosts[empty];
}

static void rescue_lite_image(nw_http_result *result, char *final_url,
                              size_t final_size)
{
    nw_http_result retry;
    image_retry_host *host_retry;
    char candidate[REQUEST_SIZE], retry_url[REQUEST_SIZE];
    char error[512], note[1200];
    int explicit_webp;
    if (!unsupported_old_ie_image_type(result->content_type)) return;
    explicit_webp = find_extension_ci(final_url, ".webp") != NULL;
    host_retry = explicit_webp ? image_retry_host_for(final_url) : NULL;
    if (host_retry && host_retry->disabled) return;
    if (!old_ie_image_candidate(final_url, candidate, sizeof(candidate))) {
        sprintf(note, "unsupported %.100s; no safe alternate for %.900s",
                result->content_type, final_url);
        log_line("IMAGE-UNSUPPORTED", note);
        return;
    }
    memset(&retry, 0, sizeof(retry));
    sprintf(note, "%.550s -> %.550s", final_url, candidate);
    log_line("IMAGE-RETRY", note);
    if (fetch_following_redirects(candidate, "GET", NULL, 0, NULL,
                                  &retry, retry_url, sizeof(retry_url),
                                  error, sizeof(error)) &&
        retry.status >= 200 && retry.status < 300 &&
        supported_old_ie_image_type(retry.content_type)) {
        nw_http_result_free(result);
        *result = retry;
        strncpy(final_url, retry_url, final_size - 1);
        final_url[final_size - 1] = 0;
        sprintf(note, "accepted %.100s, %lu bytes, %.900s",
                result->content_type, (unsigned long)result->body_size,
                final_url);
        log_line("IMAGE-RESCUE", note);
        if (host_retry) {
            host_retry->rejected_webp_jpg = 0;
            host_retry->disabled = 0;
        }
    } else {
        if (retry.body) nw_http_result_free(&retry);
        sprintf(note, "alternate rejected; original %.100s retained",
                result->content_type);
        log_line("IMAGE-RESCUE", note);
        if (host_retry && ++host_retry->rejected_webp_jpg >= 3) {
            host_retry->disabled = 1;
            sprintf(note, "%.255s: three .webp -> .jpg attempts failed; further extension guesses disabled until restart",
                    host_retry->host);
            log_line("IMAGE-HOST-NO-JPEG", note);
        }
    }
}

static int decode_routed_address(const char *encoded, char *url, size_t url_size)
{
    const char *extra = strchr(encoded, '&');
    size_t encoded_length = extra ? (size_t)(extra - encoded) : strlen(encoded);
    char encoded_url[REQUEST_SIZE];
    size_t used;
    if (encoded_length >= sizeof(encoded_url)) return 0;
    memcpy(encoded_url, encoded, encoded_length);
    encoded_url[encoded_length] = 0;
    url_decode(url, url_size, encoded_url);
    if (extra && extra[1]) {
        used = strlen(url);
        if (used + strlen(extra + 1) + 2 >= url_size) return 0;
        url[used++] = strchr(url, '?') ? '&' : '?';
        strcpy(url + used, extra + 1);
    }
    return starts_ci(url, "http://") || starts_ci(url, "https://");
}

static void serve_mode_bar(SOCKET s, int mode, const char *encoded_url)
{
    char *page;
    size_t page_size;
    char original_link[REQUEST_SIZE + 160];
    char light_link[REQUEST_SIZE + 160];
    char superlite_link[REQUEST_SIZE + 160];

    if (!encoded_url) encoded_url = "";
    if (encoded_url[0]) {
        sprintf(original_link,
                "<a target=\"_top\" href=\"/go/%s\">ORIGINAL</a>",
                encoded_url);
        sprintf(light_link,
                "<a target=\"_top\" href=\"/light/%s\">LITE</a>",
                encoded_url);
        sprintf(superlite_link,
                "<a target=\"_top\" href=\"/lite/%s\">SUPERLITE</a>",
                encoded_url);
    } else {
        strcpy(original_link, "<a target=\"_top\" href=\"/\">ORIGINAL</a>");
        strcpy(light_link, "<a target=\"_top\" href=\"/\">LITE</a>");
        strcpy(superlite_link, "<a target=\"_top\" href=\"/\">SUPERLITE</a>");
    }

    page_size = strlen(original_link) + strlen(light_link) +
                strlen(superlite_link) + 1200;
    page = (char *)malloc(page_size);
    if (!page) {
        serve_error(s, "Not enough memory for mode bar");
        return;
    }
    sprintf(page,
        "<html><head><title>NOS-Gate controls</title></head>"
        "<body bgcolor=\"#eeeeee\" text=\"#000000\" nowrap "
        "onload=\"if(parent.frames['content']) parent.frames['content'].focus()\" "
        "topmargin=\"0\" bottommargin=\"0\" marginheight=\"0\" "
        "leftmargin=\"4\" rightmargin=\"0\" marginwidth=\"4\">"
        "<font face=\"Arial\" size=\"2\">"
        "<a target=\"_top\" href=\"/\">START</a> | "
        "<a target=\"content\" href=\"/search\">SEARCH</a> | %s | %s | %s | "
        "<a target=\"_top\" href=\"/save\">SAVE</a> | "
        "<a target=\"_top\" href=\"/cookies/clear\">CLEAR COOKIES</a> | "
        "<a target=\"_top\" href=\"/quit\">OFF</a></font></body></html>",
        mode == MODE_ORIGINAL ? "<b>ORIGINAL</b>" : original_link,
        mode == MODE_LIGHT ? "<b>LITE</b>" : light_link,
        mode == MODE_SUPERLITE ? "<b>SUPERLITE</b>" : superlite_link);
    send_response(s, "200 OK", "text/html; charset=windows-1251",
                  page, strlen(page));
    free(page);
}

static int query_parameter(const char *target, const char *key,
                           char *value, size_t value_size)
{
    const char *p = strchr(target, '?');
    size_t key_length = strlen(key), length;
    if (!p || !value_size) return 0;
    ++p;
    while (*p) {
        if (!_strnicmp(p, key, key_length) && p[key_length] == '=') {
            p += key_length + 1;
            length = strcspn(p, "&");
            if (length >= value_size) length = value_size - 1;
            memcpy(value, p, length); value[length] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (!p) break;
        ++p;
    }
    value[0] = 0;
    return 0;
}

static int media_request_ids(const char *target, int *item_id, int *source_id)
{
    char value[32];
    *item_id = -1; *source_id = -1;
    if (!query_parameter(target, "id", value, sizeof(value))) return 0;
    *item_id = atoi(value);
    if (query_parameter(target, "source", value, sizeof(value)))
        *source_id = atoi(value);
    return media_catalog_by_id(*item_id) != NULL;
}

static void serve_search_page(SOCKET s)
{
    char page[2600], host[256];
    if (!last_page_url[0] || !url_host(last_page_url, host, sizeof(host)))
        host[0] = 0;
    sprintf(page,
        "<html><head><title>NOS-Gate Search</title>"
        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">"
        "</head>"
        "<body bgcolor=\"#ffffff\" text=\"#000000\">"
        "<h2>NOS-Gate Search</h2>"
        "<form method=\"GET\" action=\"/search/run\" target=\"_top\">"
        "<input type=\"hidden\" name=\"site\" value=\"%.200s\">"
        "<input name=\"q\" size=\"45\"> "
        "<input type=\"submit\" value=\"Search\"><br>"
        "<input type=\"radio\" name=\"scope\" value=\"site\" checked>"
        "Current site (%.200s) | "
        "<input type=\"radio\" name=\"scope\" value=\"web\">Whole Internet<br>"
        "Provider: DuckDuckGo Lite"
        "</form><p>Simple search without modern JavaScript.</p>"
        "<p>Backspace returns to the previous page.</p>"
        "</body></html>", host, host[0] ? host : "unavailable");
    send_response(s, "200 OK", "text/html; charset=UTF-8",
                  page, strlen(page));
}

static void serve_search_request(SOCKET s, const char *target)
{
    char raw_query[4096], raw_site[768], scope[32];
    char decoded[4096], host[256];
    char prefix[512], encoded_prefix[1536], search_url[REQUEST_SIZE], note[512];
    const char *base_url, *tail;
    char *w;
    size_t need;
    if (!query_parameter(target, "q", raw_query, sizeof(raw_query)) ||
        !raw_query[0]) {
        serve_error(s, "Enter a search query"); return;
    }
    query_parameter(target, "scope", scope, sizeof(scope));
    query_parameter(target, "site", raw_site, sizeof(raw_site));
    url_decode(decoded, sizeof(decoded), raw_query);
    url_decode(host, sizeof(host), raw_site);
    if (!strcmp(scope, "site") && host[0])
        sprintf(prefix, "site:%.240s ", host);
    else {
        scope[0] = 0;
        prefix[0] = 0;
    }
    base_url = "https://lite.duckduckgo.com/lite/?q=";
    tail = "";
    w = url_encode_to(encoded_prefix, prefix); *w = 0;
    need = strlen(base_url) + strlen(encoded_prefix) + strlen(raw_query) +
           strlen(tail) + 1;
    if (need >= sizeof(search_url)) {
        serve_error(s, "Search query is too long"); return;
    }
    sprintf(search_url, "%s%s%s%s", base_url, encoded_prefix, raw_query, tail);
    sprintf(note, "provider=ddg scope=%s host=%.160s query-bytes=%lu",
            scope[0] ? "site" : "web",
            scope[0] ? host : "(all)", (unsigned long)strlen(decoded));
    log_line("SEARCH-FALLBACK", note);
    serve_frame_url(s, search_url, remembered_mode_for_url(search_url));
}

static void serve_frame_url(SOCKET s, const char *url, int mode)
{
    char *encoded, *page, *w;
    size_t encoded_size, page_size;
    const char *content_route = mode_content_route(mode);
    const char *bar = mode == MODE_ORIGINAL ? "/bar/go?url=" :
                      (mode == MODE_SUPERLITE ? "/bar/lite?url=" : "/bar/light?url=");
    char note[512];
    encoded_size = encoded_length(url) + 1;
    page_size = encoded_size * 2 + 1024;
    encoded = (char *)malloc(encoded_size);
    page = (char *)malloc(page_size);
    if (!encoded || !page) {
        if (encoded) free(encoded);
        if (page) free(page);
        serve_error(s, "Not enough memory for navigation frame");
        return;
    }
    w = url_encode_to(encoded, url); *w = 0;
    sprintf(page,
        "<html><head><title>NOS-Gate</title></head>"
        "<frameset rows=\"30,*\" frameborder=\"0\" border=\"0\" "
        "onload=\"window.frames['content'].focus()\">"
        "<frame src=\"%s%s\" name=\"nosbar\" scrolling=\"no\" noresize tabindex=\"-1\">"
        "<frame src=\"%s%s\" name=\"content\" tabindex=\"0\" "
        "onload=\"this.contentWindow.focus()\">"
        "<noframes><body><a href=\"%s%s\">Open page</a></body></noframes>"
        "</frameset></html>", bar, encoded, content_route, encoded,
        content_route, encoded);
    strncpy(last_page_url, url, sizeof(last_page_url) - 1);
    last_page_url[sizeof(last_page_url) - 1] = 0;
    last_page_mode = mode;
    sprintf(note, "%s %.400s", mode_name(mode), url);
    log_line("FRAME", note);
    send_response(s, "200 OK", "text/html; charset=UTF-8", page, strlen(page));
    free(page); free(encoded);
}

static void serve_frame_encoded(SOCKET s, const char *encoded, int mode,
                                int remember)
{
    char url[REQUEST_SIZE], routed[REQUEST_SIZE];
    if (!prepare_routed_target(encoded, routed, sizeof(routed)) ||
        !decode_routed_address(routed, url, sizeof(url))) {
        serve_error(s, "Address must start with http:// or https://");
        return;
    }
    /* 0.5.1 Final: a top-level YouTube video page is not rendered by
       NOS-Gate. Hand its video ID directly to NOS-Pipe as search text.
       This covers watch?v=ID, /watch/ID, /embed/ID and youtu.be/ID. */
    {
        char youtube_id[128];
        if (youtube_video_id(url, youtube_id, sizeof(youtube_id))) {
            char encoded_id[384], *ew;
            ew = url_encode_to(encoded_id, youtube_id);
            *ew = 0;
            serve_nospipe_youtube(s, encoded_id);
            return;
        }
    }
    if (remember) remember_mode_for_url(url, mode);
    serve_frame_url(s, url, mode);
}


static void serve_home(SOCKET s)
{
    static const char page[] =
        "<html><head><title>NOS-Gate 0.5.1 Final</title></head>"
        "<body bgcolor=\"#ffffff\" text=\"#000000\"><center>"
        "<h1>NOS-Gate 0.5.1 Final</h1>"
        "<p>Internet gateway for old browsers</p>"
        "<form method=\"GET\" action=\"/open\">"
        "<input name=\"url\" size=\"60\" value=\"https://\"><br>"
        "<input type=\"radio\" name=\"mode\" value=\"remember\" checked>Remembered/default "
        "<input type=\"radio\" name=\"mode\" value=\"go\">Original "
        "<input type=\"radio\" name=\"mode\" value=\"light\">Lite "
        "<input type=\"radio\" name=\"mode\" value=\"lite\">Superlite<br>"
        "<input type=\"submit\" value=\"Open\"></form>"
        "<p><a href=\"/search\">Search</a> | "
        "<a href=\"/cookies/clear\">Clear cookies</a> | "
        "<a href=\"/quit\">Stop NOS-Gate</a></p>"
        "</center>"
        "<div align=\"left\"><hr><b>TESTED SITES</b><br>"
        "<a href=\"/light/https%3A%2F%2F4pda.to%2Fforum%2F\">4PDA</a><br>"
        "<a href=\"/light/https%3A%2F%2Fwww.youtube.com%2F\">YouTube</a><br>"
        "<a href=\"/light/https%3A%2F%2Fyummyanime.tv%2F\">YummyAnime</a><br>"
        "<a href=\"/light/https%3A%2F%2Fgidonline.fun%2F\">GidOnline</a><br>"
        "<a href=\"/light/https%3A%2F%2Ffrogfind.com%2F\">FrogFind</a><br>"
        "<a href=\"/light/https%3A%2F%2Fworldofspectrum.org%2F\">World of Spectrum .org</a><br>"
        "<a href=\"/light/https%3A%2F%2Fworldofspectrum.net%2F\">World of Spectrum .net</a>"
        "</div></body></html>";
    send_response(s, "200 OK", "text/html; charset=UTF-8", page, strlen(page));
}

static void serve_open_request(SOCKET s, const char *target)
{
    char raw_url[REQUEST_SIZE], raw_mode[32], url[REQUEST_SIZE], mode_name_value[32];
    int mode;
    if (!query_parameter(target, "url", raw_url, sizeof(raw_url)) || !raw_url[0]) {
        serve_error(s, "Enter an address"); return;
    }
    url_decode(url, sizeof(url), raw_url);
    if (!starts_ci(url, "http://") && !starts_ci(url, "https://")) {
        serve_error(s, "Address must start with http:// or https://"); return;
    }
    mode_name_value[0] = 0;
    if (query_parameter(target, "mode", raw_mode, sizeof(raw_mode)))
        url_decode(mode_name_value, sizeof(mode_name_value), raw_mode);
    if (!_stricmp(mode_name_value, "go")) mode = MODE_ORIGINAL;
    else if (!_stricmp(mode_name_value, "light")) mode = MODE_LIGHT;
    else if (!_stricmp(mode_name_value, "lite")) mode = MODE_SUPERLITE;
    else mode = remembered_mode_for_url(url);
    if (_stricmp(mode_name_value, "remember")) remember_mode_for_url(url, mode);
    serve_frame_url(s, url, mode);
}

static void serve_error(SOCKET s, const char *message)
{
    char page[1400];
    sprintf(page,
        "<html><head><title>NOS-Gate error</title></head><body>"
        "<h2>NOS-Gate error</h2><p>%.1000s</p><p><a href=\"/\">Home</a></p>"
        "</body></html>", message);
    send_html(s, page);
}

static void expand_hls_variants(media_catalog *catalog, const char *player_url)
{
    nw_http_result result;
    char final_url[4096], error[512], note[512];
    int i, first_new, original_count, added = 0;
    if (!catalog) return;
    original_count = catalog->source_count;
    for (i = 0; i < original_count; ++i) {
        if (catalog->source[i].type != MEDIA_SOURCE_HLS) continue;
        remember_iframe_referer(catalog->source[i].url, player_url);
        if (!fetch_following_redirects(catalog->source[i].url, "GET", NULL, 0,
                                       NULL, &result, final_url,
                                       sizeof(final_url), error,
                                       sizeof(error))) {
            sprintf(note, "master fetch failed: %.400s", error);
            log_line("HLS-MASTER", note);
            continue;
        }
        first_new = catalog->source_count;
        if (result.status >= 200 && result.status < 300)
            added += hls_master_add_variants(catalog, result.body, final_url);
        while (first_new < catalog->source_count) {
            remember_iframe_referer(catalog->source[first_new].url,
                                    player_url);
            if (catalog->source[first_new].audio_url[0])
                remember_iframe_referer(catalog->source[first_new].audio_url,
                                        player_url);
            ++first_new;
        }
        sprintf(note, "HTTP %d type=%.120s bytes=%lu variants=%d",
                result.status, result.content_type,
                (unsigned long)result.body_size, added);
        log_line("HLS-MASTER", note);
        nw_http_result_free(&result);
    }
    media_catalog_select_360(catalog);
}

static int serve_explicit_iframe_media(SOCKET s, const char *html,
                                      const char *player_url)
{
    char safe_title[2048], source_list[7000], line[700];
    char play_form[512], download_link[512];
    char *page = NULL;
    size_t page_size;
    media_catalog *catalog;
    media_source *selected;
    const char *transport_note;
    int i, item_id;
    const char *provider = iframe_provider(player_url);
    catalog = explicit_iframe_adapter(html, player_url, provider, &item_id);
    if (!catalog) {
        sprintf(line, "generic iframe adapter found no direct source provider=%.80s url=%.400s",
                provider, player_url);
        log_line("MEDIA-GENERIC", line);
        return 0;
    }
    sprintf(line, "generic iframe adapter accepted provider=%.80s url=%.400s",
            catalog->provider, player_url);
    log_line("MEDIA-GENERIC", line);
    expand_hls_variants(catalog, player_url);
    selected = media_source_by_id(catalog, -1);
    html_escape_text(catalog->title, safe_title, sizeof(safe_title));
    strcpy(source_list, "<p><b>Detected sources:</b></p><ul>");
    for (i = 0; i < catalog->source_count; ++i) {
        const char *type = catalog->source[i].type == MEDIA_SOURCE_MP4 ?
                           "MP4" : "HLS";
        sprintf(line, "<li>%s: %.63s%s%s"
                "<form method=\"GET\" action=\"/media/play\">"
                "<input type=\"hidden\" name=\"id\" value=\"%d\">"
                "<input type=\"hidden\" name=\"source\" value=\"%d\">"
                "<input type=\"submit\" value=\"Play this source\"></form></li>",
                type,
                catalog->source[i].label,
                catalog->source[i].audio_url[0] ? " + AUDIO" : "",
                i == catalog->selected_source ? " (default)" : "",
                item_id, i);
        if (strlen(source_list) + strlen(line) + 6 < sizeof(source_list))
            strcat(source_list, line);
    }
    strcat(source_list, "</ul>");
    play_form[0] = download_link[0] = 0;
    if (selected) {
        sprintf(play_form,
            "<form method=\"GET\" action=\"/media/play\">"
            "<input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<input type=\"hidden\" name=\"source\" value=\"%d\">"
            "<input type=\"submit\" value=\"Play selected source\"></form>",
            item_id, catalog->selected_source);
        if (selected->type == MEDIA_SOURCE_MP4)
            sprintf(download_link,
                "<p><a target=\"content\" href=\"/media/download?id=%d&amp;source=%d\">"
                "Download selected source</a></p>",
                item_id, catalog->selected_source);
        else
            strcpy(download_link,
                "<p>Selected HLS quality is playable; direct HLS download is not yet available.</p>");
    }
    transport_note = selected && selected->type == MEDIA_SOURCE_HLS ?
        "<p>MPlayer receives a local HLS playlist. NOS-Gate rewrites and relays its HTTPS resources.</p>" :
        "<p>MPlayer receives a short local HTTP address. NOS-Gate relays the remote HTTPS stream without a 4 MB buffer.</p>";
    page_size = strlen(safe_title) + strlen(source_list) + 2200;
    page = (char *)malloc(page_size);
    if (!page) {
        serve_error(s, "Not enough memory for media controls"); return 1;
    }
    sprintf(page,
        "<html><head><title>NOS-Gate video</title>"
        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">"
        "</head><body bgcolor=\"#ffffff\" text=\"#000000\">"
        "<h2>Video: %s</h2><p>Provider: %s</p>%s%s%s%s"
        "<p><a href=\"javascript:history.back()\">Back</a></p>"
        "</body></html>", safe_title, catalog->provider, source_list,
        selected ? play_form :
            "<p><b>No playable media source was found.</b></p>",
        selected ? download_link :
            "<p>No supported MP4 or HLS source was detected.</p>",
        transport_note);
    if (!selected)
        log_line("MEDIA", "Media Core found no playable source");
    else if (selected->type == MEDIA_SOURCE_MP4)
        log_line("MEDIA", "Media Core selected an MP4 source");
    else
        log_line("MEDIA", "Media Core selected an HLS quality variant");
    send_response(s, "200 OK", "text/html; charset=UTF-8", page, strlen(page));
    free(page);
    return 1;
}

static void serve_media_play(SOCKET s, int item_id, int source_id)
{
    char player[128], page[3000], safe_title[2048], local_url[4096], note[256];
    media_catalog *catalog = media_catalog_by_id(item_id);
    media_source *source = media_source_by_id(catalog, source_id);
    if (!catalog || !source) {
        log_line("MEDIA-PLAY-BLOCKED", "No supported source selected");
        serve_error(s, "No supported media source is available.");
        return;
    }
    html_escape_text(catalog->title, safe_title, sizeof(safe_title));
    if (source->type == MEDIA_SOURCE_MP4)
        sprintf(local_url, "http://127.0.0.1:8080/media/stream?id=%d&source=%d",
                item_id, source_id);
    else
        sprintf(local_url, "http://127.0.0.1:8080/media/hls/index.m3u8?id=%d&source=%d",
                item_id, source_id);
    if (!launch_media_url(local_url, source->type == MEDIA_SOURCE_HLS, player,
                          sizeof(player))) {
        log_line("MEDIA-PLAY", "No installed player accepted the local relay address");
        serve_error(s, "No supported player was found"); return;
    }
    sprintf(page,
        "<html><head><title>NOS-Gate video</title></head><body>"
        "<h2>Player started</h2><p>%s</p><p>Using: %s</p>"
        "<p><a href=\"javascript:history.back()\">Back to video controls</a></p>"
        "</body></html>", safe_title, player);
    sprintf(note, "id=%d source=%d q=%dp type=%s player=%.128s",
            item_id, source_id, source->quality,
            source->type == MEDIA_SOURCE_MP4 ? "MP4" : "HLS", player);
    log_line("MEDIA-PLAY", note);
    send_response(s, "200 OK", "text/html; charset=UTF-8", page, strlen(page));
}


static void serve_save_current(SOCKET s)
{
    nw_http_result result;
    char final_url[REQUEST_SIZE], error[512], path[MAX_PATH];
    char header[4608], *slash;
    HANDLE file;
    DWORD written;
    size_t header_len;

    if (!last_page_url[0]) {
        serve_error(s, "No current remote page to save");
        return;
    }

    if (!fetch_following_redirects(last_page_url, "GET", NULL, 0, NULL,
                                   &result, final_url, sizeof(final_url),
                                   error, sizeof(error))) {
        serve_error(s, error);
        return;
    }

    if (!GetModuleFileName(NULL, path, sizeof(path)))
        strcpy(path, "nos-gate-save.txt");
    else {
        slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "nos-gate-save.txt");
        else strcpy(path, "nos-gate-save.txt");
    }

    file = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        nw_http_result_free(&result);
        serve_error(s, "Cannot create nos-gate-save.txt");
        return;
    }

    sprintf(header,
            "NOS-Gate diagnostic page save\r\n"
            "URL: %.4000s\r\n"
            "HTTP: %d\r\n"
            "Content-Type: %.120s\r\n"
            "Body-Bytes: %lu\r\n"
            "----- RAW RESPONSE BODY -----\r\n",
            final_url, result.status, result.content_type,
            (unsigned long)result.body_size);
    header_len = strlen(header);
    WriteFile(file, header, (DWORD)header_len, &written, NULL);
    if (result.body_size)
        WriteFile(file, result.body, (DWORD)result.body_size, &written, NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
    nw_http_result_free(&result);

    send_html(s,
        "<html><body><h2>Saved.</h2>"
        "<p>Current remote page saved as <b>nos-gate-save.txt</b> "
        "next to NOS-Gate.exe.</p>"
        "<p><a href=\"javascript:history.back()\">Back</a></p></body></html>");
}

static void serve_remote(SOCKET s, const char *encoded, const char *method,
                         const char *request_body, size_t request_body_size,
                         const char *request_content_type, int mode)
{
    char encoded_url[REQUEST_SIZE], url[REQUEST_SIZE], final_url[REQUEST_SIZE];
    char error[512], details[512];
    const char *extra;
    size_t encoded_url_length;
    nw_http_result result;
    char *rewritten = NULL;
    size_t rewritten_length = 0;
    int processed = mode != MODE_ORIGINAL;
    const char *route = mode_content_route(mode);
    extra = strchr(encoded, '&');
    encoded_url_length = extra ? (size_t)(extra - encoded) : strlen(encoded);
    if (encoded_url_length >= sizeof(encoded_url)) {
        serve_error(s, "Encoded address is too long"); return;
    }
    memcpy(encoded_url, encoded, encoded_url_length); encoded_url[encoded_url_length] = 0;
    url_decode(url, sizeof(url), encoded_url);
    if (extra && extra[1]) {
        size_t used = strlen(url);
        if (used + strlen(extra + 1) + 2 >= sizeof(url)) {
            serve_error(s, "Search address is too long"); return;
        }
        url[used++] = strchr(url, '?') ? '&' : '?';
        strcpy(url + used, extra + 1);
    }
    if (!starts_ci(url, "http://") && !starts_ci(url, "https://")) {
        serve_error(s, "Address must start with http:// or https://"); return;
    }
    if (!fetch_following_redirects(url, method, request_body, request_body_size,
                                   request_content_type, &result,
                                   final_url, sizeof(final_url),
                                   error, sizeof(error))) {
        log_line("ERROR", error); serve_error(s, error); return;
    }
    if (processed) rescue_lite_image(&result, final_url, sizeof(final_url));
    sprintf(details, "HTTP %d, %lu bytes, %.200s", result.status,
            (unsigned long)result.body_size, result.content_type);
    log_line("HTTP", details);
    if (result.status >= 200 && result.status < 300 &&
        remembered_explicit_iframe(final_url) &&
        (starts_ci(result.content_type, "text/html") ||
         starts_ci(result.content_type, "application/xhtml")) &&
        serve_explicit_iframe_media(s, result.body, final_url)) {
        nw_http_result_free(&result); return;
    }
    if (starts_ci(result.content_type, "text/html") ||
        starts_ci(result.content_type, "application/xhtml")) {
        if (mode == MODE_LIGHT)
            log_line("MODE", "LIGHT: CSS kept; scripts, events and heavy media removed");
        else if (mode == MODE_SUPERLITE)
            log_line("MODE", "SUPERLITE: scripts, CSS, events and heavy media removed");
        else log_line("MODE", "ORIGINAL: page preserved; addresses routed through NOS-Gate");
    }
    if (result.status < 200 || result.status >= 300) {
        const char *error_type = result.content_type[0] ? result.content_type : "text/html";
        save_error_body(result.body, result.body_size);
        if (starts_ci(result.content_type, "text/html") ||
            starts_ci(result.content_type, "application/xhtml")) {
            rewritten = rewrite_html(result.body, result.body_size, final_url,
                                     route, mode,
                                     &rewritten_length);
            if (rewritten) {
                send_response(s, "200 OK", "text/html; charset=UTF-8", rewritten, rewritten_length);
                free(rewritten);
            } else send_response(s, "200 OK", "text/html; charset=UTF-8", result.body, result.body_size);
        } else send_response(s, "200 OK", error_type, result.body, result.body_size);
        nw_http_result_free(&result); return;
    }
    if (starts_ci(result.content_type, "text/html") ||
        starts_ci(result.content_type, "application/xhtml")) {
        strncpy(last_page_url, final_url, sizeof(last_page_url) - 1);
        last_page_url[sizeof(last_page_url) - 1] = 0;
        last_page_mode = mode;
        if (mode == MODE_SUPERLITE && youtube_page(final_url) &&
            contains_ci(final_url, "/results"))
            rewritten = youtube_results_html(result.body, result.body_size,
                                              final_url, &rewritten_length);
        else if (mode == MODE_SUPERLITE && youtube_page(final_url) &&
                 contains_ci(final_url, "/watch"))
            rewritten = youtube_watch_html(result.body, result.body_size,
                                            final_url, &rewritten_length);
        else
            rewritten = rewrite_html(result.body, result.body_size, final_url,
                                     route, mode,
                                     &rewritten_length);
    }
    if (rewritten) {
        const char *browser_type = result.content_type;
        if (starts_ci(result.content_type, "application/xhtml"))
            browser_type = "text/html; charset=UTF-8";
        send_response(s, "200 OK", browser_type[0] ? browser_type : "text/html",
                      rewritten, rewritten_length);
        free(rewritten);
    } else send_response(s, "200 OK",
        result.content_type[0] ? result.content_type : "application/octet-stream",
        result.body, result.body_size);
    nw_http_result_free(&result);
}

static int prepare_routed_target(const char *source, char *target,
                                 size_t target_size)
{
    char *query = strchr(source, '?');
    size_t encoded_part = query ? (size_t)(query - source) : strlen(source);
    if (encoded_part + (query ? strlen(query + 1) + 2 : 1) >= target_size)
        return 0;
    memcpy(target, source, encoded_part);
    if (query && query[1]) {
        target[encoded_part++] = '&';
        strcpy(target + encoded_part, query + 1);
    } else target[encoded_part] = 0;
    return 1;
}

typedef struct media_relay_context {
    SOCKET client;
    int download;
    int headers_sent;
    unsigned long bytes_sent;
    int status;
    char content_type[128];
    char content_length[64];
} media_relay_context;

static int media_relay_header(void *opaque, int status,
    const char *content_type, const char *content_length,
    const char *content_disposition, const char *transfer_encoding,
    const char *content_range, const char *accept_ranges)
{
    media_relay_context *context = (media_relay_context *)opaque;
    char header[2048], status_text[64], length_line[128];
    char transfer_line[128], disposition_line[640], range_line[192];
    char note[640];
    context->status = status;
    strncpy(context->content_type,
            content_type && content_type[0] ? content_type : "(none)",
            sizeof(context->content_type) - 1);
    context->content_type[sizeof(context->content_type) - 1] = 0;
    strncpy(context->content_length,
            content_length && content_length[0] ? content_length : "(unknown)",
            sizeof(context->content_length) - 1);
    context->content_length[sizeof(context->content_length) - 1] = 0;
    sprintf(note, "HTTP %d type=%.120s length=%.60s range=%.120s",
            status, context->content_type, context->content_length,
            content_range && content_range[0] ? content_range : "(none)");
    log_line("MEDIA-RELAY-HEADER", note);
    if (status == 206) strcpy(status_text, "206 Partial Content");
    else if (status >= 200 && status < 300) strcpy(status_text, "200 OK");
    else sprintf(status_text, "%d Remote response", status);
    length_line[0] = transfer_line[0] = disposition_line[0] = range_line[0] = 0;
    if (content_length && content_length[0])
        sprintf(length_line, "Content-Length: %.63s\r\n", content_length);
    if (transfer_encoding && transfer_encoding[0])
        sprintf(transfer_line, "Transfer-Encoding: %.63s\r\n", transfer_encoding);
    if (context->download)
        strcpy(disposition_line,
               "Content-Disposition: attachment; filename=NOS-Gate-video.mp4\r\n");
    else if (content_disposition && content_disposition[0])
        sprintf(disposition_line, "Content-Disposition: %.500s\r\n",
                content_disposition);
    if (content_range && content_range[0])
        sprintf(range_line, "Content-Range: %.127s\r\n", content_range);
    sprintf(header,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\n%s%s%s%s"
        "Accept-Ranges: %s\r\nConnection: close\r\n\r\n",
        status_text,
        content_type && content_type[0] ? content_type : "application/octet-stream",
        length_line, transfer_line, disposition_line, range_line,
        accept_ranges && accept_ranges[0] ? accept_ranges : "bytes");
    context->headers_sent = send_all(context->client, header, strlen(header));
    return context->headers_sent;
}

static int media_relay_data(void *opaque, const char *data, size_t length)
{
    media_relay_context *context = (media_relay_context *)opaque;
    if (!send_all(context->client, data, length)) return 0;
    context->bytes_sent += (unsigned long)length;
    return 1;
}

static void serve_media_relay(SOCKET client, int download, const char *range,
                              int item_id, int source_id,
                              const char *override_url,
                              const char *override_referer,
                              int hls_resource)
{
    char media_cookie[16384];
    media_relay_context context;
    media_catalog *catalog;
    media_source *source;
    char current[4096], next[4096], redirect[2048], error[512], note[512];
    char referer[4096];
    char host[256];
    int step, result;
    volatile int cancel = 0;
    host[0] = error[0] = 0;
    catalog = media_catalog_by_id(item_id);
    source = media_source_by_id(catalog, source_id);
    if (!source || (!hls_resource && source->type != MEDIA_SOURCE_MP4)) {
        log_line("MEDIA-RELAY-BLOCKED", "No supported direct MP4 source selected");
        serve_error(client, "No supported direct MP4 source has been selected");
        return;
    }
    strncpy(current, override_url ? override_url : source->url,
            sizeof(current) - 1);
    current[sizeof(current) - 1] = 0;
    strncpy(referer, override_referer ? override_referer : catalog->referer,
            sizeof(referer) - 1);
    referer[sizeof(referer) - 1] = 0;
    memset(&context, 0, sizeof(context));
    context.client = client; context.download = download;
    sprintf(note, "id=%d source=%d q=%dp kind=%s url=%.200s referer=%.200s range=%.60s",
            item_id, source_id, source->quality,
            hls_resource ? "HLS-resource" : "MP4", current, referer,
            range && range[0] ? range : "(none)");
    log_line("MEDIA-RELAY-START", note);
    for (step = 0; step <= MAX_REDIRECTS; ++step) {
        cookies_for(current, media_cookie, sizeof(media_cookie));
        result = nw_http_stream_request(current, referer, media_cookie[0] ? media_cookie : NULL,
                    range, redirect, sizeof(redirect), media_relay_header,
                    media_relay_data, &context, &cancel,
                    error, sizeof(error));
        if (result == 1) {
            url_host(current, host, sizeof(host));
            sprintf(note, "id=%d source=%d q=%dp type=%d %s via %.160s, HTTP %d %.100s, %lu bytes",
                    item_id, source_id, source->quality, source->type,
                    download ? "download" : "play", host, context.status,
                    context.content_type, context.bytes_sent);
            log_line("MEDIA-RELAY-END", note);
            return;
        }
        if (result == 2 && step < MAX_REDIRECTS) {
            if (!resolve_url(current, redirect, next, sizeof(next))) {
                strcpy(error, "Invalid media relay redirect"); break;
            }
            strncpy(referer, current, sizeof(referer) - 1);
            referer[sizeof(referer) - 1] = 0;
            strncpy(current, next, sizeof(current) - 1);
            current[sizeof(current) - 1] = 0;
            sprintf(note, "redirect %.220s -> %.220s", redirect, current);
            log_line("MEDIA-RELAY", note);
            continue;
        }
        break;
    }
    if (!error[0])
        strcpy(error, context.headers_sent ?
               "Media client disconnected before the relay completed" :
               "Media relay ended without a diagnostic message");
    log_line("MEDIA-RELAY-ERROR", error);
    if (!context.headers_sent) serve_error(client, error);
}

static void serve_hls_playlist_url(SOCKET client, int item_id, int source_id,
                                   const char *url, const char *referer)
{
    nw_http_result result;
    char final_url[4096], error[512], note[512];
    char *rewritten;
    const char *preview, *preview_end;
    size_t rewritten_length = 0;
    int preview_count;
    if (referer && referer[0]) remember_iframe_referer(url, referer);
    if (!fetch_following_redirects(url, "GET", NULL, 0, NULL, &result,
                                   final_url, sizeof(final_url), error,
                                   sizeof(error))) {
        sprintf(note, "playlist fetch failed: %.400s", error);
        log_line("HLS-RELAY-ERROR", note);
        serve_error(client, error); return;
    }
    sprintf(note, "id=%d source=%d HTTP=%d type=%.100s bytes=%lu url=%.180s",
            item_id, source_id, result.status, result.content_type,
            (unsigned long)result.body_size, final_url);
    log_line("HLS-PLAYLIST", note);
    if (result.status < 200 || result.status >= 300) {
        nw_http_result_free(&result);
        serve_error(client, "HLS playlist server returned an error"); return;
    }
    rewritten = rewrite_hls_playlist(result.body, result.body_size,
                                     final_url, item_id, source_id,
                                     &rewritten_length);
    nw_http_result_free(&result);
    if (!rewritten) {
        serve_error(client, "Cannot rewrite HLS playlist"); return;
    }
    sprintf(note, "id=%d source=%d local-bytes=%lu",
            item_id, source_id, (unsigned long)rewritten_length);
    log_line("HLS-PLAYLIST-LOCAL", note);
    preview = rewritten;
    preview_count = 0;
    while (preview_count < 3 &&
           (preview = strstr(preview, "http://127.0.0.1:")) != NULL) {
        preview_end = strstr(preview, "\r\n");
        if (!preview_end) preview_end = preview + strlen(preview);
        sprintf(note, "%d %.460s", preview_count + 1, preview);
        if ((size_t)(preview_end - preview) < 460)
            note[(preview_end - preview) + 2] = 0;
        log_line("HLS-LOCAL-URL", note);
        preview = preview_end;
        ++preview_count;
    }
    send_response(client, "200 OK", "application/x-mpegURL",
                  rewritten, rewritten_length);
    free(rewritten);
}

static void serve_hls_root(SOCKET client, const char *target,
                           int item_id, int source_id)
{
    media_catalog *catalog = media_catalog_by_id(item_id);
    media_source *source = media_source_by_id(catalog, source_id);
    char master[1400], note[256];
    int width;
    if (!catalog || !source || source->type != MEDIA_SOURCE_HLS) {
        serve_error(client, "Unknown or expired HLS source"); return;
    }
    if (strstr(target, "/video.m3u8")) {
        serve_hls_playlist_url(client, item_id, source_id, source->url,
                               catalog->referer);
        return;
    }
    if (strstr(target, "/audio.m3u8")) {
        if (!source->audio_url[0]) {
            serve_error(client, "This HLS source has no external audio");
            return;
        }
        serve_hls_playlist_url(client, item_id, source_id, source->audio_url,
                               catalog->referer);
        return;
    }
    if (source->audio_url[0]) {
        width = source->quality >= 720 ? 1280 :
                (source->quality >= 480 ? 854 : 640);
        sprintf(master,
            "#EXTM3U\r\n#EXT-X-VERSION:3\r\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"nos-audio\",NAME=\"Audio\","
            "DEFAULT=YES,AUTOSELECT=YES,URI=\"http://127.0.0.1:8080/media/hls/audio.m3u8?id=%d&source=%d\"\r\n"
            "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=1200000,RESOLUTION=%dx%d,AUDIO=\"nos-audio\"\r\n"
            "http://127.0.0.1:8080/media/hls/video.m3u8?id=%d&source=%d\r\n",
            item_id, source_id, width, source->quality,
            item_id, source_id);
        sprintf(note, "id=%d source=%d q=%dp mode=VIDEO+AUDIO",
                item_id, source_id, source->quality);
        log_line("HLS-LOCAL-MASTER", note);
        send_response(client, "200 OK", "application/x-mpegURL",
                      master, strlen(master));
        return;
    }
    serve_hls_playlist_url(client, item_id, source_id, source->url,
                           catalog->referer);
}

static void serve_hls_resource(SOCKET client, const char *target,
                               const char *range, int item_id, int source_id)
{
    media_catalog *catalog = media_catalog_by_id(item_id);
    media_source *source = media_source_by_id(catalog, source_id);
    char raw_url[REQUEST_SIZE], raw_referer[REQUEST_SIZE];
    char url[4096], referer[4096];
    if (!catalog || !source || source->type != MEDIA_SOURCE_HLS) {
        serve_error(client, "Unknown or expired HLS resource"); return;
    }
    if (!query_parameter(target, "url", raw_url, sizeof(raw_url))) {
        serve_error(client, "Missing HLS resource address"); return;
    }
    url_decode(url, sizeof(url), raw_url);
    referer[0] = 0;
    if (query_parameter(target, "ref", raw_referer, sizeof(raw_referer)))
        url_decode(referer, sizeof(referer), raw_referer);
    if (!starts_ci(url, "http://") && !starts_ci(url, "https://")) {
        serve_error(client, "Invalid HLS resource address"); return;
    }
    if (!referer[0]) {
        strncpy(referer, catalog->referer, sizeof(referer) - 1);
        referer[sizeof(referer) - 1] = 0;
    }
    if (hls_playlist_url(url))
        serve_hls_playlist_url(client, item_id, source_id, url, referer);
    else
        serve_media_relay(client, 0, range, item_id, source_id,
                          url, referer, 1);
}

static void handle_client(SOCKET client)
{
    char request[REQUEST_SIZE], method[16], target[REQUEST_SIZE], *space;
    char routed_target[REQUEST_SIZE];
    char content_type[128], range[1024];
    char *headers_end, *body, *line, *line_end;
    int count, used = 0, content_length = 0, header_size, body_size;
    int media_item_id, media_source_id;
    while (used + 1 < sizeof(request) &&
           (count = recv(client, request + used, sizeof(request) - used - 1, 0)) > 0) {
        used += count; request[used] = 0;
        headers_end = strstr(request, "\r\n\r\n");
        if (headers_end) {
            line = request;
            while ((line = strstr(line, "\r\n")) != NULL) {
                line += 2;
                if (!_strnicmp(line, "Content-Length:", 15))
                    content_length = atoi(line + 15);
            }
            header_size = (int)(headers_end + 4 - request);
            if (used >= header_size + content_length) break;
        }
    }
    if (used <= 0) return;
    method[0] = target[0] = 0;
    sscanf(request, "%15s %8191s", method, target);
    {
        char browser_request[REQUEST_SIZE + 32];
        sprintf(browser_request, "%s %.8191s", method, target);
        log_line("BROWSER", browser_request);
    }
    if (_stricmp(method, "GET") && _stricmp(method, "POST")) {
        send_response(client, "405 Method Not Allowed", "text/plain",
                      "GET and POST only\r\n", 19); return;
    }
    headers_end = strstr(request, "\r\n\r\n");
    if (!headers_end) { serve_error(client, "Incomplete browser request"); return; }
    body = headers_end + 4; body_size = used - (int)(body - request);
    if (content_length > body_size) { serve_error(client, "Form request is too large"); return; }
    body_size = content_length;
    strcpy(content_type, "application/x-www-form-urlencoded");
    range[0] = 0;
    line = request;
    while ((line = strstr(line, "\r\n")) != NULL && line < headers_end) {
        line += 2;
        if (!_strnicmp(line, "Content-Type:", 13)) {
            size_t n;
            line += 13; while (*line == ' ' || *line == '\t') ++line;
            line_end = strstr(line, "\r\n");
            n = line_end ? (size_t)(line_end - line) : 0;
            if (n >= sizeof(content_type)) n = sizeof(content_type) - 1;
            memcpy(content_type, line, n); content_type[n] = 0;
        }
        if (!_strnicmp(line, "Range:", 6)) {
            size_t n;
            line += 6; while (*line == ' ' || *line == '\t') ++line;
            line_end = strstr(line, "\r\n");
            n = line_end ? (size_t)(line_end - line) : 0;
            if (n >= sizeof(range)) n = sizeof(range) - 1;
            memcpy(range, line, n); range[n] = 0;
        }
    }
    space = strchr(target, '#'); if (space) *space = 0;
    if (!strcmp(target, "/") || !strcmp(target, "/index.html")) serve_home(client);
    else if (!strcmp(target, "/search")) serve_search_page(client);
    else if (!strncmp(target, "/search/run?", 12))
        serve_search_request(client, target);
    else if (!strncmp(target, "/nospipe/youtube/", 17))
        serve_nospipe_youtube(client, target + 17);
    else if (!strncmp(target, "/media/play", 11)) {
        if (media_request_ids(target, &media_item_id, &media_source_id))
            serve_media_play(client, media_item_id, media_source_id);
        else serve_error(client, "Unknown or expired media control ID");
    }
    else if (!strncmp(target, "/media/stream", 13)) {
        if (media_request_ids(target, &media_item_id, &media_source_id))
            serve_media_relay(client, 0, range, media_item_id, media_source_id,
                              NULL, NULL, 0);
        else serve_error(client, "Unknown or expired media stream ID");
    }
    else if (!strncmp(target, "/media/download", 15)) {
        if (media_request_ids(target, &media_item_id, &media_source_id))
            serve_media_relay(client, 1, range, media_item_id, media_source_id,
                              NULL, NULL, 0);
        else serve_error(client, "Unknown or expired media download ID");
    }
    else if (!strncmp(target, "/media/hls-resource", 19)) {
        if (media_request_ids(target, &media_item_id, &media_source_id))
            serve_hls_resource(client, target, range,
                               media_item_id, media_source_id);
        else serve_error(client, "Unknown or expired HLS resource ID");
    }
    else if (!strncmp(target, "/media/hls", 10)) {
        if (media_request_ids(target, &media_item_id, &media_source_id))
            serve_hls_root(client, target, media_item_id, media_source_id);
        else serve_error(client, "Unknown or expired HLS source ID");
    }
    else if (!strncmp(target, "/open?", 6)) serve_open_request(client, target);
    else if (!strcmp(target, "/save")) {
        serve_save_current(client);
    }
    else if (!strcmp(target, "/cookies/clear")) {
        clear_cookies();
        send_html(client, "<html><body><h2>Cookies cleared.</h2><p><a href=\"/\">Home</a></p></body></html>");
    }
    else if (!strncmp(target, "/bar/go?url=", 12))
        serve_mode_bar(client, MODE_ORIGINAL, target + 12);
    else if (!strncmp(target, "/bar/light?url=", 15))
        serve_mode_bar(client, MODE_LIGHT, target + 15);
    else if (!strncmp(target, "/bar/lite?url=", 14))
        serve_mode_bar(client, MODE_SUPERLITE, target + 14);
    else if (!strcmp(target, "/bar/go")) serve_mode_bar(client, MODE_ORIGINAL, "");
    else if (!strcmp(target, "/bar/light")) serve_mode_bar(client, MODE_LIGHT, "");
    else if (!strcmp(target, "/bar/lite")) serve_mode_bar(client, MODE_SUPERLITE, "");
    else if (!strcmp(target, "/switch/go") ||
             !strcmp(target, "/switch/light") ||
             !strcmp(target, "/switch/lite")) {
        int selected = !strcmp(target, "/switch/go") ? MODE_ORIGINAL :
                       (!strcmp(target, "/switch/lite") ? MODE_SUPERLITE : MODE_LIGHT);
        if (!last_page_url[0]) serve_home(client);
        else {
            remember_mode_for_url(last_page_url, selected);
            serve_frame_url(client, last_page_url, selected);
        }
    }
    else if (!strncmp(target, "/go?url=", 8))
        serve_frame_encoded(client, target + 8, MODE_ORIGINAL, 1);
    else if (!strncmp(target, "/light?url=", 11))
        serve_frame_encoded(client, target + 11, MODE_LIGHT, 1);
    else if (!strncmp(target, "/lite?url=", 10))
        serve_frame_encoded(client, target + 10, MODE_SUPERLITE, 1);
    else if (!strncmp(target, "/go/", 4))
        serve_frame_encoded(client, target + 4, MODE_ORIGINAL, 0);
    else if (!strncmp(target, "/light/", 7))
        serve_frame_encoded(client, target + 7, MODE_LIGHT, 0);
    else if (!strncmp(target, "/lite/", 6))
        serve_frame_encoded(client, target + 6, MODE_SUPERLITE, 0);
    else if (!strncmp(target, "/page/go/", 9)) {
        if (!prepare_routed_target(target + 9, routed_target, sizeof(routed_target)))
            serve_error(client, "Routed address is too long");
        else serve_remote(client, routed_target, method, body, (size_t)body_size,
                          content_type, MODE_ORIGINAL);
    }
    else if (!strncmp(target, "/page/light/", 12)) {
        if (!prepare_routed_target(target + 12, routed_target, sizeof(routed_target)))
            serve_error(client, "Routed address is too long");
        else serve_remote(client, routed_target, method, body, (size_t)body_size,
                          content_type, MODE_LIGHT);
    }
    else if (!strncmp(target, "/page/lite/", 11)) {
        if (!prepare_routed_target(target + 11, routed_target, sizeof(routed_target)))
            serve_error(client, "Routed address is too long");
        else serve_remote(client, routed_target, method, body, (size_t)body_size,
                          content_type, MODE_SUPERLITE);
    }
    else if (!strcmp(target, "/quit")) {
        send_html(client, "<html><body><h2>NOS-Gate stopped.</h2></body></html>");
        gate_running = 0;
        /* WinSock 1.1 accept() is blocking. Closing the listening socket from
           this worker wakes the main thread immediately so OFF really exits. */
        if (gate_server_socket != INVALID_SOCKET) {
            closesocket(gate_server_socket);
            gate_server_socket = INVALID_SOCKET;
        }
    } else if (last_page_url[0] && target[0] == '/') {
        char absolute[4096];
        if (!resolve_url(last_page_url, target, absolute, sizeof(absolute)) ||
            encoded_length(absolute) + 1 >= sizeof(routed_target))
            send_response(client, "404 Not Found", "text/plain", "Not found\r\n", 11);
        else {
            *url_encode_to(routed_target, absolute) = 0;
            log_line("RESOURCE", absolute);
            serve_remote(client, routed_target, method, body, (size_t)body_size,
                         content_type, last_page_mode);
        }
    } else send_response(client, "404 Not Found", "text/plain", "Not found\r\n", 11);
}

static DWORD WINAPI client_worker(LPVOID parameter)
{
    SOCKET client = (SOCKET)parameter;
    LONG active;
    char worker_info[160];
    active = InterlockedIncrement(&gate_active_workers);
    sprintf(worker_info, "thread=%lu active=%ld socket=%lu",
            (unsigned long)GetCurrentThreadId(), (long)active,
            (unsigned long)client);
    log_line("WORKER-START", worker_info);
    handle_client(client);
    shutdown(client, 1);
    closesocket(client);
    active = InterlockedDecrement(&gate_active_workers);
    sprintf(worker_info, "thread=%lu active=%ld socket=%lu",
            (unsigned long)GetCurrentThreadId(), (long)active,
            (unsigned long)client);
    log_line("WORKER-END", worker_info);
    ReleaseSemaphore(gate_worker_slots, 1, NULL);
    return 0;
}

static void open_browser(void)
{
    HINSTANCE result = ShellExecute(NULL, "open", "http://127.0.0.1:8080/",
                                    NULL, NULL, SW_SHOWNORMAL);
    if ((UINT)result <= 32) log_line("ERROR", "Cannot start default browser");
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    SOCKET server, client;
    struct sockaddr_in address;
    HANDLE instance_mutex, worker;
    (void)instance; (void)previous; (void)command; (void)show;
    open_log();
    instance_mutex = CreateMutex(NULL, FALSE, "NOS-Gate_0.3-single-instance");
    if (!instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        log_line("ERROR", "Another NOS-Gate instance is already running");
        open_browser();
        if (instance_mutex) CloseHandle(instance_mutex);
        return 0;
    }
    load_cookies();
    if (!nw_http_startup()) {
        MessageBox(NULL, "Winsock 1.1 is not available", "NOS-Gate 0.5.1 Final", MB_OK | MB_ICONERROR);
        return 1;
    }
    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        MessageBox(NULL, "Cannot create local server socket", "NOS-Gate 0.5.1 Final", MB_OK | MB_ICONERROR);
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(GATE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(server, 8) == SOCKET_ERROR) {
        closesocket(server); nw_http_cleanup();
        MessageBox(NULL, "Port 8080 is busy or unavailable", "NOS-Gate 0.5.1 Final", MB_OK | MB_ICONERROR);
        return 1;
    }
    gate_server_socket = server;
    InitializeCriticalSection(&gate_state_lock);
    gate_state_lock_ready = 1;
    gate_worker_slots = CreateSemaphore(NULL, GATE_WORKER_LIMIT, GATE_WORKER_LIMIT, NULL);
    if (!gate_worker_slots) {
        closesocket(server); nw_http_cleanup();
        MessageBox(NULL, "Cannot create worker semaphore", "NOS-Gate 0.5.1 Final", MB_OK | MB_ICONERROR);
        return 1;
    }
    log_line("WORKER-LIMIT", "global HTTP workers=8 (guarded test)");
    log_line("READY", "http://127.0.0.1:8080/");
    open_browser();
    while (gate_running) {
        if (WaitForSingleObject(gate_worker_slots, INFINITE) != WAIT_OBJECT_0)
            break;
        client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) {
            ReleaseSemaphore(gate_worker_slots, 1, NULL);
            break;
        }
        worker = CreateThread(NULL, 0, client_worker, (LPVOID)client, 0, NULL);
        if (!worker) {
            handle_client(client); shutdown(client, 1); closesocket(client);
            ReleaseSemaphore(gate_worker_slots, 1, NULL);
        } else CloseHandle(worker);
    }
    {
        int worker_slot;
        for (worker_slot = 0; worker_slot < GATE_WORKER_LIMIT; ++worker_slot)
            WaitForSingleObject(gate_worker_slots, INFINITE);
        ReleaseSemaphore(gate_worker_slots, GATE_WORKER_LIMIT, NULL);
    }
    CloseHandle(gate_worker_slots);
    if (gate_server_socket != INVALID_SOCKET) {
        closesocket(gate_server_socket);
        gate_server_socket = INVALID_SOCKET;
    }
    if (gate_state_lock_ready) {
        gate_state_lock_ready = 0;
        DeleteCriticalSection(&gate_state_lock);
    }
    nw_http_cleanup();
    log_line("STOP", "NOS-Gate closed");
    CloseHandle(instance_mutex);
    return 0;
}
