#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
typedef void *httpd_handle_t;
enum http_method { HTTP_DELETE = 0, HTTP_GET = 1 };
typedef enum http_method httpd_method_t;
typedef void (*httpd_free_ctx_fn_t)(void *ctx);
typedef void (*httpd_work_fn_t)(void *arg);
typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    const char uri[513];
    size_t content_len;
    void *aux;
    void *user_ctx;
    void *sess_ctx;
    httpd_free_ctx_fn_t free_ctx;
    bool ignore_sess_ctx_changes;
} httpd_req_t;
typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
    bool is_websocket;
    bool handle_ws_control_frames;
    const char *supported_subprotocol;
#ifdef CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    esp_err_t (*ws_pre_handshake_cb)(httpd_req_t *req);
#endif
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
    esp_err_t (*ws_post_handshake_cb)(httpd_req_t *req);
#endif
} httpd_uri_t;
typedef enum {
    HTTPD_WS_TYPE_CONTINUE = 0x0,
    HTTPD_WS_TYPE_TEXT = 0x1,
    HTTPD_WS_TYPE_BINARY = 0x2,
    HTTPD_WS_TYPE_CLOSE = 0x8,
    HTTPD_WS_TYPE_PING = 0x9,
    HTTPD_WS_TYPE_PONG = 0xA,
} httpd_ws_type_t;
typedef enum {
    HTTPD_WS_CLIENT_INVALID = 0x0,
    HTTPD_WS_CLIENT_HTTP = 0x1,
    HTTPD_WS_CLIENT_WEBSOCKET = 0x2,
} httpd_ws_client_info_t;
typedef struct httpd_ws_frame {
    bool final;
    bool fragmented;
    httpd_ws_type_t type;
    uint8_t *payload;
    size_t len;
} httpd_ws_frame_t;
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
int httpd_req_to_sockfd(httpd_req_t *r);
esp_err_t httpd_ws_recv_frame(httpd_req_t *req, httpd_ws_frame_t *pkt, size_t max_len);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame);
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t hd, int fd);
esp_err_t httpd_queue_work(httpd_handle_t handle, httpd_work_fn_t work, void *arg);
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd);
esp_err_t httpd_get_client_list(httpd_handle_t handle, size_t *fds, int *client_fds);
void *httpd_sess_get_ctx(httpd_handle_t handle, int sockfd);
void httpd_sess_set_ctx(httpd_handle_t handle, int sockfd, void *ctx, httpd_free_ctx_fn_t free_fn);
