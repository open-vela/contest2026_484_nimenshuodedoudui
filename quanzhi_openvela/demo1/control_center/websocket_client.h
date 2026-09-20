#ifndef __WEBSOCKET_CLIENT_H__
#define __WEBSOCKET_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// Define the callback function type
typedef void (*ws_recv_callback_t)(const char *buffer, size_t size);

typedef struct websocket_data_t {
    char *hostname;
    char *port;
    char *path; 
    char *hello;
    char *headers;
} websocket_data_t;

/**
 * 发送二进制数据
 */
int websocket_send_binary(const char *data, int size);

/**
 * 发送文本数据
 */
int websocket_send_text(const char *data, int size);

/**
 * 设置回调函数和数据
 */
int websocket_set_callbacks(ws_recv_callback_t bin_cb, ws_recv_callback_t txt_cb, 
                           websocket_data_t *ws_data);

/**
 * 启动WebSocket线程
 */
int websocket_start(void);

/**
 * 停止WebSocket连接
 */
int websocket_stop(void);

#ifdef __cplusplus
}
#endif

#endif