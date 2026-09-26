#ifndef NOSWEB_HTTP_WIN32_H
#define NOSWEB_HTTP_WIN32_H

#include <stddef.h>

typedef struct nw_http_result {
    char *body;
    size_t body_size;
    int status;
    char content_type[128];
    char location[2048];
    char set_cookie[16384];
    int set_cookie_count;
    char server[256];
    char via[256];
    char cf_ray[128];
    char cf_mitigated[128];
    char retry_after[128];
} nw_http_result;

typedef int (*nw_http_stream_header_fn)(void *context, int status,
    const char *content_type, const char *content_length,
    const char *content_disposition, const char *transfer_encoding,
    const char *content_range, const char *accept_ranges);
typedef int (*nw_http_stream_data_fn)(void *context, const char *data,
                                      size_t length);

int nw_http_startup(void);
void nw_http_cleanup(void);
int nw_http_get(const char *url, nw_http_result *result,
                volatile int *cancel_flag, char *error, size_t error_size);
int nw_http_request(const char *url, const char *method, const char *request_body,
                    size_t request_body_size, const char *request_content_type,
                    const char *referer, const char *cookie,
                    nw_http_result *result, volatile int *cancel_flag,
                    char *error, size_t error_size);
int nw_http_stream_request(const char *url, const char *referer,
                           const char *cookie, const char *range,
                           char *redirect, size_t redirect_size,
                           nw_http_stream_header_fn header_fn,
                           nw_http_stream_data_fn data_fn, void *context,
                           volatile int *cancel_flag,
                           char *error, size_t error_size);
void nw_http_result_free(nw_http_result *result);

#endif
