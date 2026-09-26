#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bearssl.h>
#include "http_win32.h"
#include "trust_anchors.c"

typedef struct nw_socket_context {
    SOCKET socket_handle;
    volatile int *cancel;
} nw_socket_context;

static int parse_url(const char *url, char *host, size_t host_size,
                     char *path, size_t path_size, unsigned short *port,
                     int *use_tls)
{
    const char *p, *slash, *colon;
    size_t n;
    if (!strncmp(url, "http://", 7)) { p = url + 7; *use_tls = 0; }
    else if (!strncmp(url, "https://", 8)) { p = url + 8; *use_tls = 1; }
    else return 0;
    slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);
    colon = memchr(p, ':', (size_t)(slash - p));
    n = (size_t)((colon ? colon : slash) - p);
    if (!n || n >= host_size) return 0;
    memcpy(host, p, n); host[n] = 0;
    *port = colon ? (unsigned short)atoi(colon + 1) : (unsigned short)(*use_tls ? 443 : 80);
    if (*slash) {
        if (strlen(slash) >= path_size) return 0;
        strcpy(path, slash);
    } else strcpy(path, "/");
    return 1;
}

static int tls_socket_read(void *context, unsigned char *buffer, size_t length)
{
    nw_socket_context *socket_context = (nw_socket_context *)context;
    int count;
    if (*socket_context->cancel) return -1;
    count = recv(socket_context->socket_handle, (char *)buffer, (int)length, 0);
    return count > 0 ? count : -1;
}

static int tls_socket_write(void *context, const unsigned char *buffer, size_t length)
{
    nw_socket_context *socket_context = (nw_socket_context *)context;
    int count;
    if (*socket_context->cancel) return -1;
    count = send(socket_context->socket_handle, (const char *)buffer, (int)length, 0);
    return count > 0 ? count : -1;
}

static int append_bytes(char **raw, size_t *used, size_t *capacity,
                        const char *buffer, size_t count)
{
    char *next;
    if (*used + count + 1 > 4194304UL) return -1;
    while (*used + count + 1 > *capacity) *capacity *= 2;
    next = (char *)realloc(*raw, *capacity);
    if (!next) return 0;
    *raw = next;
    memcpy(*raw + *used, buffer, count); *used += count;
    return 1;
}

static int socket_send_all(SOCKET socket_handle, const char *data, size_t length)
{
    int count;
    while (length) {
        count = send(socket_handle, data, length > 30000 ? 30000 : (int)length, 0);
        if (count <= 0) return 0;
        data += count; length -= (size_t)count;
    }
    return 1;
}

int nw_http_startup(void)
{
    WSADATA data;
    return WSAStartup(MAKEWORD(1, 1), &data) == 0;
}

void nw_http_cleanup(void) { WSACleanup(); }

void nw_http_result_free(nw_http_result *r)
{
    free(r->body); memset(r, 0, sizeof(*r));
}

static void read_header_value(const char *headers, const char *name,
                              char *output, size_t output_size)
{
    const char *line = headers;
    const char *end;
    size_t name_length = strlen(name), length;
    if (!output_size) return;
    output[0] = 0;
    while (*line) {
        end = strstr(line, "\r\n");
        if (!end) break;
        if ((size_t)(end - line) > name_length &&
            !_strnicmp(line, name, name_length) && line[name_length] == ':') {
            line += name_length + 1;
            while (*line == ' ' || *line == '\t') ++line;
            length = (size_t)(end - line);
            if (length >= output_size) length = output_size - 1;
            memcpy(output, line, length); output[length] = 0; return;
        }
        line = end + 2;
    }
}

static int read_set_cookies(const char *headers, char *output, size_t output_size)
{
    const char *line = headers, *end, *value;
    size_t used = 0, length;
    int count = 0;
    if (!output_size) return 0;
    output[0] = 0;
    while (*line) {
        end = strstr(line, "\r\n");
        if (!end) break;
        if ((size_t)(end - line) > 11 && !_strnicmp(line, "Set-Cookie:", 11)) {
            ++count;
            value = line + 11;
            while (*value == ' ' || *value == '\t') ++value;
            length = (size_t)(end - value);
            if (length && used + length + (used ? 1 : 0) + 1 < output_size) {
                if (used) output[used++] = '\n';
                memcpy(output + used, value, length); used += length;
                output[used] = 0;
            }
        }
        line = end + 2;
    }
    return count;
}

static void set_error(char *out, size_t size, const char *message)
{
    if (!size) return;
    strncpy(out, message, size - 1); out[size - 1] = 0;
}

int nw_http_request(const char *url, const char *method, const char *request_body,
                    size_t request_body_size, const char *request_content_type,
                    const char *referer, const char *cookie,
                    nw_http_result *r, volatile int *cancel,
                    char *error, size_t error_size)
{
    char host[256], path[2048], request[24576], buffer[4096];
    char optional[17400];
    unsigned short port;
    struct hostent *entry;
    struct sockaddr_in address;
    SOCKET socket_handle = INVALID_SOCKET;
    char *raw = NULL, *header_end;
    size_t used = 0, capacity = 16384, body_offset;
    int count = 0, append_result, use_tls;
    br_ssl_client_context ssl_client;
    br_x509_minimal_context x509_context;
    br_sslio_context ssl_io;
    unsigned char ssl_buffer[BR_SSL_BUFSIZE_BIDI];
    nw_socket_context socket_context;
    memset(r, 0, sizeof(*r));
    if (!parse_url(url, host, sizeof(host), path, sizeof(path), &port, &use_tls)) {
        set_error(error, error_size, "Address must start with http:// or https://"); return 0;
    }
    entry = gethostbyname(host);
    if (!entry) { set_error(error, error_size, "DNS lookup failed"); return 0; }
    socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) { set_error(error, error_size, "Cannot create socket"); return 0; }
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_port = htons(port); memcpy(&address.sin_addr, entry->h_addr, entry->h_length);
    if (connect(socket_handle, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        set_error(error, error_size, "Connection failed"); closesocket(socket_handle); return 0;
    }
    if (!method || (_stricmp(method, "GET") && _stricmp(method, "POST"))) {
        set_error(error, error_size, "Unsupported HTTP method"); closesocket(socket_handle); return 0;
    }
    optional[0] = 0;
    if (referer && referer[0]) sprintf(optional + strlen(optional), "Referer: %.2048s\r\n", referer);
    if (cookie && cookie[0]) sprintf(optional + strlen(optional), "Cookie: %.16300s\r\n", cookie);
    if (!_stricmp(method, "POST"))
        sprintf(request, "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/96.0.4664.45 Safari/537.36 Edg/96.0.1054.34 NOS-Gate/0.3.12\r\nAccept: text/html,image/jpeg,image/png,image/gif,*/*\r\nAccept-Encoding: identity\r\n%sContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n", path, host, optional, request_content_type && request_content_type[0] ? request_content_type : "application/x-www-form-urlencoded", (unsigned long)request_body_size);
    else
        sprintf(request, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/96.0.4664.45 Safari/537.36 Edg/96.0.1054.34 NOS-Gate/0.3.12\r\nAccept: text/html,image/jpeg,image/png,image/gif,*/*\r\nAccept-Encoding: identity\r\n%sConnection: close\r\n\r\n", path, host, optional);
    raw = (char *)malloc(capacity);
    if (!raw) { set_error(error, error_size, "Not enough memory"); closesocket(socket_handle); return 0; }
    if (use_tls) {
        int tls_error;
        socket_context.socket_handle = socket_handle; socket_context.cancel = cancel;
        nw_init_trust_anchors();
        br_ssl_client_init_full(&ssl_client, &x509_context, TAs, TAs_NUM);
        br_ssl_engine_set_versions(&ssl_client.eng, BR_TLS12, BR_TLS12);
        br_ssl_engine_set_buffer(&ssl_client.eng, ssl_buffer, sizeof(ssl_buffer), 1);
        if (!br_ssl_client_reset(&ssl_client, host, 0)) {
            set_error(error, error_size, "Cannot initialize TLS"); free(raw); closesocket(socket_handle); return 0;
        }
        br_sslio_init(&ssl_io, &ssl_client.eng, tls_socket_read, &socket_context,
                      tls_socket_write, &socket_context);
        if (br_sslio_write_all(&ssl_io, request, strlen(request)) < 0 ||
            (request_body_size && br_sslio_write_all(&ssl_io, request_body,
                                                      request_body_size) < 0) ||
            br_sslio_flush(&ssl_io) < 0) {
            char tls_message[80];
            tls_error = br_ssl_engine_last_error(&ssl_client.eng);
            sprintf(tls_message, "TLS send/handshake error %d", tls_error);
            set_error(error, error_size, tls_message);
            free(raw); closesocket(socket_handle); return 0;
        }
        while (!*cancel && (count = br_sslio_read(&ssl_io, buffer, sizeof(buffer))) > 0) {
            append_result = append_bytes(&raw, &used, &capacity, buffer, (size_t)count);
            if (append_result <= 0) {
                set_error(error, error_size, append_result < 0 ? "Response exceeds 4 MB limit" : "Not enough memory");
                free(raw); closesocket(socket_handle); return 0;
            }
        }
        tls_error = br_ssl_engine_last_error(&ssl_client.eng);
        if (count < 0 && tls_error != BR_ERR_OK && used == 0) {
            char tls_message[80];
            sprintf(tls_message, "TLS error %d", tls_error);
            set_error(error, error_size, tls_message);
            free(raw); closesocket(socket_handle); return 0;
        }
    } else {
        if (!socket_send_all(socket_handle, request, strlen(request)) ||
            (request_body_size && !socket_send_all(socket_handle, request_body,
                                                    request_body_size))) {
            set_error(error, error_size, "Send failed"); free(raw); closesocket(socket_handle); return 0;
        }
        while (!*cancel && (count = recv(socket_handle, buffer, sizeof(buffer), 0)) > 0) {
            append_result = append_bytes(&raw, &used, &capacity, buffer, (size_t)count);
            if (append_result <= 0) {
                set_error(error, error_size, append_result < 0 ? "Response exceeds 4 MB limit" : "Not enough memory");
                free(raw); closesocket(socket_handle); return 0;
            }
        }
    }
    closesocket(socket_handle);
    if (*cancel) { free(raw); set_error(error, error_size, "Cancelled"); return 0; }
    raw[used] = 0; header_end = strstr(raw, "\r\n\r\n");
    if (!header_end) { free(raw); set_error(error, error_size, "Invalid HTTP response"); return 0; }
    sscanf(raw, "HTTP/%*s %d", &r->status);
    read_header_value(raw, "Location", r->location, sizeof(r->location));
    read_header_value(raw, "Content-Type", r->content_type, sizeof(r->content_type));
    r->set_cookie_count = read_set_cookies(raw, r->set_cookie, sizeof(r->set_cookie));
    read_header_value(raw, "Server", r->server, sizeof(r->server));
    read_header_value(raw, "Via", r->via, sizeof(r->via));
    read_header_value(raw, "CF-Ray", r->cf_ray, sizeof(r->cf_ray));
    read_header_value(raw, "cf-mitigated", r->cf_mitigated, sizeof(r->cf_mitigated));
    read_header_value(raw, "Retry-After", r->retry_after, sizeof(r->retry_after));
    body_offset = (size_t)(header_end + 4 - raw);
    r->body_size = used - body_offset;
    r->body = (char *)malloc(r->body_size + 1);
    if (!r->body) { free(raw); set_error(error, error_size, "Not enough memory"); return 0; }
    memcpy(r->body, raw + body_offset, r->body_size); r->body[r->body_size] = 0;
    free(raw); return 1;
}

typedef struct nw_stream_state {
    char header[65536];
    size_t header_used;
    int header_done;
    char *redirect;
    size_t redirect_size;
    nw_http_stream_header_fn header_fn;
    nw_http_stream_data_fn data_fn;
    void *context;
} nw_stream_state;

static int stream_consume(nw_stream_state *state, const char *data, size_t length,
                          char *error, size_t error_size)
{
    char *header_end;
    size_t body_offset;
    int status = 0;
    char content_type[128], content_length[64];
    char disposition[512], transfer[64], location[2048];
    char content_range[128], accept_ranges[64];
    if (state->header_done)
        return state->data_fn(state->context, data, length) ? 1 : -1;
    if (state->header_used + length + 1 > sizeof(state->header)) {
        set_error(error, error_size, "HTTP response headers are too large");
        return -1;
    }
    memcpy(state->header + state->header_used, data, length);
    state->header_used += length;
    state->header[state->header_used] = 0;
    header_end = strstr(state->header, "\r\n\r\n");
    if (!header_end) return 1;
    sscanf(state->header, "HTTP/%*s %d", &status);
    read_header_value(state->header, "Location", location, sizeof(location));
    if (status >= 300 && status < 400 && location[0]) {
        strncpy(state->redirect, location, state->redirect_size - 1);
        state->redirect[state->redirect_size - 1] = 0;
        return 2;
    }
    read_header_value(state->header, "Content-Type", content_type,
                      sizeof(content_type));
    read_header_value(state->header, "Content-Length", content_length,
                      sizeof(content_length));
    read_header_value(state->header, "Content-Disposition", disposition,
                      sizeof(disposition));
    read_header_value(state->header, "Transfer-Encoding", transfer,
                      sizeof(transfer));
    read_header_value(state->header, "Content-Range", content_range,
                      sizeof(content_range));
    read_header_value(state->header, "Accept-Ranges", accept_ranges,
                      sizeof(accept_ranges));
    if (!state->header_fn(state->context, status, content_type, content_length,
                          disposition, transfer, content_range,
                          accept_ranges)) {
        set_error(error, error_size, "Cannot send relay response headers");
        return -1;
    }
    state->header_done = 1;
    body_offset = (size_t)(header_end + 4 - state->header);
    if (state->header_used > body_offset &&
        !state->data_fn(state->context, state->header + body_offset,
                        state->header_used - body_offset)) return -1;
    return 1;
}

int nw_http_stream_request(const char *url, const char *referer,
                           const char *cookie, const char *range,
                           char *redirect, size_t redirect_size,
                           nw_http_stream_header_fn header_fn,
                           nw_http_stream_data_fn data_fn, void *context,
                           volatile int *cancel, char *error, size_t error_size)
{
    char host[256], path[2048], request[24576], buffer[16384], optional[17400];
    unsigned short port;
    struct hostent *entry;
    struct sockaddr_in address;
    SOCKET socket_handle = INVALID_SOCKET;
    int count = 0, use_tls, consume_result = 1, tls_error;
    br_ssl_client_context ssl_client;
    br_x509_minimal_context x509_context;
    br_sslio_context ssl_io;
    unsigned char ssl_buffer[BR_SSL_BUFSIZE_BIDI];
    nw_socket_context socket_context;
    nw_stream_state state;
    if (!redirect_size || !header_fn || !data_fn) {
        set_error(error, error_size, "Invalid relay parameters"); return 0;
    }
    redirect[0] = 0;
    if (!parse_url(url, host, sizeof(host), path, sizeof(path), &port, &use_tls)) {
        set_error(error, error_size, "Relay address must start with http:// or https://");
        return 0;
    }
    entry = gethostbyname(host);
    if (!entry) { set_error(error, error_size, "Relay DNS lookup failed"); return 0; }
    socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        set_error(error, error_size, "Cannot create relay socket"); return 0;
    }
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_port = htons(port);
    memcpy(&address.sin_addr, entry->h_addr, entry->h_length);
    if (connect(socket_handle, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        set_error(error, error_size, "Relay connection failed");
        closesocket(socket_handle); return 0;
    }
    optional[0] = 0;
    if (referer && referer[0])
        sprintf(optional + strlen(optional), "Referer: %.2048s\r\n", referer);
    if (cookie && cookie[0])
        sprintf(optional + strlen(optional), "Cookie: %.14000s\r\n", cookie);
    if (range && range[0])
        sprintf(optional + strlen(optional), "Range: %.1024s\r\n", range);
    sprintf(request,
        "GET %s HTTP/1.0\r\nHost: %s\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/96.0.4664.45 "
        "Safari/537.36 Edg/96.0.1054.34 NOS-Gate/0.3.12\r\n"
        "Accept: */*\r\nAccept-Encoding: identity\r\n%sConnection: close\r\n\r\n",
        path, host, optional);
    memset(&state, 0, sizeof(state));
    state.redirect = redirect; state.redirect_size = redirect_size;
    state.header_fn = header_fn; state.data_fn = data_fn; state.context = context;
    if (use_tls) {
        socket_context.socket_handle = socket_handle; socket_context.cancel = cancel;
        nw_init_trust_anchors();
        br_ssl_client_init_full(&ssl_client, &x509_context, TAs, TAs_NUM);
        br_ssl_engine_set_versions(&ssl_client.eng, BR_TLS12, BR_TLS12);
        br_ssl_engine_set_buffer(&ssl_client.eng, ssl_buffer, sizeof(ssl_buffer), 1);
        if (!br_ssl_client_reset(&ssl_client, host, 0)) {
            set_error(error, error_size, "Cannot initialize relay TLS");
            closesocket(socket_handle); return 0;
        }
        br_sslio_init(&ssl_io, &ssl_client.eng, tls_socket_read, &socket_context,
                      tls_socket_write, &socket_context);
        if (br_sslio_write_all(&ssl_io, request, strlen(request)) < 0 ||
            br_sslio_flush(&ssl_io) < 0) {
            tls_error = br_ssl_engine_last_error(&ssl_client.eng);
            sprintf(buffer, "Relay TLS send/handshake error %d", tls_error);
            set_error(error, error_size, buffer);
            closesocket(socket_handle); return 0;
        }
        while (!*cancel &&
               (count = br_sslio_read(&ssl_io, (unsigned char *)buffer,
                                      sizeof(buffer))) > 0) {
            consume_result = stream_consume(&state, buffer, (size_t)count,
                                            error, error_size);
            if (consume_result != 1) break;
        }
        tls_error = br_ssl_engine_last_error(&ssl_client.eng);
        if (count < 0 && tls_error != BR_ERR_OK && !state.header_done &&
            consume_result == 1) {
            sprintf(buffer, "Relay TLS error %d", tls_error);
            set_error(error, error_size, buffer); consume_result = -1;
        }
    } else {
        if (!socket_send_all(socket_handle, request, strlen(request))) {
            set_error(error, error_size, "Relay send failed");
            closesocket(socket_handle); return 0;
        }
        while (!*cancel &&
               (count = recv(socket_handle, buffer, sizeof(buffer), 0)) > 0) {
            consume_result = stream_consume(&state, buffer, (size_t)count,
                                            error, error_size);
            if (consume_result != 1) break;
        }
    }
    closesocket(socket_handle);
    if (consume_result == 2) return 2;
    if (*cancel) { set_error(error, error_size, "Relay cancelled"); return 0; }
    if (consume_result < 0) return 0;
    if (!state.header_done) {
        set_error(error, error_size, "Invalid relay HTTP response"); return 0;
    }
    return 1;
}

int nw_http_get(const char *url, nw_http_result *r, volatile int *cancel,
                char *error, size_t error_size)
{
    return nw_http_request(url, "GET", NULL, 0, NULL, NULL, NULL, r, cancel,
                           error, error_size);
}
