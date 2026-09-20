// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (c) 2008-2023 100askTeam : Dongshan WEI <weidongshan@100ask.net> 
 * Discourse:  https://forums.100ask.net
 */
 
/*  Copyright (C) 2008-2023 深圳百问网科技有限公司
 *  All rights reserved
 *
 * 免责声明: 百问网编写的文档, 仅供学员学习使用, 可以转发或引用(请保留作者信息),禁止用于商业用途！
 * 免责声明: 百问网编写的程序, 用于商业用途请遵循GPL许可, 百问网不承担任何后果！
 * 
 * 本程序遵循GPL V3协议, 请遵循协议
 * 百问网学习平台   : https://www.100ask.net
 * 百问网交流社区   : https://forums.100ask.net
 * 百问网官方B站    : https://space.bilibili.com/275908810
 * 本程序所用开发板 : Linux开发板
 * 百问网官方淘宝   : https://100ask.taobao.com
 * 联系我们(E-mail) : weidongshan@100ask.net
 *
 *          版权所有，盗版必究。
 *  
 * 修改历史     版本号           作者        修改内容
 *-----------------------------------------------------
 * 2025.03.20      v01         百问科技      创建文件
 *-----------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <cJSON.h>
#include "websocket_client.h"

#include <nuttx/pthread.h>

#include <syslog.h>
#define printf(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

static struct lws *g_wsi = NULL;
static websocket_data_t *g_ws_data = NULL;
static ws_recv_callback_t g_ws_recv_bin_cb = NULL;
static ws_recv_callback_t g_ws_recv_txt_cb = NULL;
static volatile int g_iHasShaked = 0;
static volatile int g_iHasConnected = 0;
static volatile int g_iHasDisconnected = 0;
static struct lws_context *g_context = NULL;
static pthread_t g_ws_thread;
static volatile int g_should_reconnect = 1;
static int g_reconnect_counter = 0;

// 提前声明set_headers函数
static int set_headers(struct lws *wsi, const char *headers_json, unsigned char **p, unsigned char *end);

/**
 * 处理接收到的消息
 */
static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                             void *user, void *in, size_t len)
{
    //printf("websocket_callback reason = %d\n", reason);
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("Connected\n");
            g_iHasConnected = 1;
            g_iHasDisconnected = 0;
            g_iHasShaked = 1;
            g_wsi = wsi;
            g_reconnect_counter = 0; // 重置重连计数器
            
            // 连接建立后立即发送hello消息
            if (g_ws_data && g_ws_data->hello) {
                int msg_len = strlen(g_ws_data->hello);
                unsigned char *buf = malloc(LWS_PRE + msg_len);
                if (buf) {
                    memcpy(buf + LWS_PRE, g_ws_data->hello, msg_len);
                    int ret = lws_write(wsi, buf + LWS_PRE, msg_len, LWS_WRITE_TEXT);
                    free(buf);
                    
                    if (ret < 0) {
                        printf("Failed to send hello message, ret: %d\n", ret);
                    } else {
                        printf("Send hello message: %s\n", g_ws_data->hello);
                    }
                } else {
                    printf("Failed to allocate memory for hello message\n");
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            {
                int is_binary = lws_frame_is_binary(wsi);
                
                if (is_binary && g_ws_recv_bin_cb) {
                    // 处理二进制数据
                    g_ws_recv_bin_cb((const char*)in, len);
                } else if (g_ws_recv_txt_cb) {
                    // 处理文本数据
                    printf("Received: %.*s\n", (int)len, (char*)in);
                    g_ws_recv_txt_cb((const char*)in, len);
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            // 在可写回调中不发送hello消息，只在连接建立时发送
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            printf("Connection error\n");
            if (in) {
                printf("Error message: %s\n", (char *)in);
            }
            g_iHasConnected = 0;
            g_iHasShaked = 0;
            g_wsi = NULL;
            g_iHasDisconnected = 1;
            if (g_context) {
                lws_cancel_service(g_context);
            }            
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            printf("Connection closed LWS_CALLBACK_CLIENT_CLOSED\n");
            g_iHasConnected = 0;
            g_iHasDisconnected = 1;
            g_iHasShaked = 0;
            g_wsi = NULL;
            // 如果应该重连，则安排重连
            if (g_should_reconnect) {
                // 延迟一小段时间后再重连
                sleep(2);
                // 触发重连
                if (g_context) {
                    lws_cancel_service(g_context);
                }
            }
            break;

        case LWS_CALLBACK_WSI_DESTROY:
            printf("WSI destroy LWS_CALLBACK_WSI_DESTROY\n");
            g_wsi = NULL;
            g_iHasDisconnected = 1;
            if (g_context) {
                lws_cancel_service(g_context);
            }             
            break;
            
        case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
            // 跳过服务器证书验证，方便测试
            break;
            
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            // 添加自定义头部信息
            if (g_ws_data && g_ws_data->headers) {
                unsigned char **p = (unsigned char **)in;
                unsigned char *end = (*p) + len;
                set_headers(wsi, g_ws_data->headers, p, end);
            }
            break;

        default:
            break;
    }

    return 0;
}

/**
 * WebSocket 协议数组
 */
static struct lws_protocols protocols[] = {
    {
        "websocket",
        websocket_callback,
        0,
        4096,
    },
    { NULL, NULL, 0, 0 } /* 结束 */
};

/**
 * 解析 headers JSON 字符串并设置到 lws
 */
static int set_headers(struct lws *wsi, const char *headers_json, unsigned char **p, unsigned char *end)
{
    cJSON *headers = cJSON_Parse(headers_json);
    if (!headers) {
        printf("Failed to parse headers JSON\n");
        return -1;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, headers) {
        if (cJSON_IsString(item)) {
            const char *key = item->string;
            const char *value = item->valuestring;
            printf("Add header: %s: %s\n", key, value);
            
            // 使用lws_add_http_header_by_name添加头部
            // 注意：key需要以冒号结尾
            char key_with_colon[128];
            snprintf(key_with_colon, sizeof(key_with_colon), "%s:", key);
            
            if (lws_add_http_header_by_name(wsi, (unsigned char*)key_with_colon, 
                                          (unsigned char*)value, strlen(value), p, end)) {
                printf("Failed to add header: %s\n", key);
            }
        }
    }

    cJSON_Delete(headers);
    return 0;
}

/**
 * WebSocket 线程函数
 */
static void *websocket_thread(void *arg)
{
    int retry_count = 0;
    const int max_retry_count = 10;

    g_wsi = NULL;
    g_iHasShaked = 0;
    g_iHasConnected = 0;
    g_iHasDisconnected = 0;
    g_context = NULL;
    g_should_reconnect = 1;
    g_reconnect_counter = 0;

    while (g_should_reconnect) {
        struct lws_context_creation_info info;
        memset(&info, 0, sizeof(info));

        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        info.gid = -1;
        info.uid = -1;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

        if (!g_context)
            g_context = lws_create_context(&info);
        if (!g_context) {
            printf("Failed to create WebSocket context\n");
            retry_count++;
            sleep(5);
            continue;
        }

        // 创建连接
        struct lws_client_connect_info connect_info;
        memset(&connect_info, 0, sizeof(connect_info));

        // 设置context字段，这是必须的
        connect_info.context = g_context;
        
        // 根据C++代码调整SSL设置：完全禁用证书验证
        connect_info.ssl_connection = LCCSCF_USE_SSL | 
                             LCCSCF_ALLOW_SELFSIGNED | 
                             LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                             LCCSCF_ALLOW_INSECURE |
                             LCCSCF_ALLOW_EXPIRED;
        // 设置连接信息
        if (g_ws_data && g_ws_data->hostname) {
            connect_info.address = g_ws_data->hostname;
        }
        
        if (g_ws_data && g_ws_data->port) {
            connect_info.port = atoi(g_ws_data->port);
        }
        
        if (g_ws_data && g_ws_data->path) {
            connect_info.path = g_ws_data->path;
        }

        connect_info.host = connect_info.address;
        connect_info.origin = connect_info.address;
        connect_info.protocol = protocols[0].name;

        g_iHasDisconnected = 0;
        struct lws *wsi = lws_client_connect_via_info(&connect_info);
        if (!wsi) {
            printf("Failed to create connection\n");
            lws_context_destroy(g_context);
            g_context = NULL;
            retry_count++;
            
            // 指数退避算法等待重试
            int wait_time = 1 << (retry_count > 5 ? 5 : retry_count);
            printf("Retrying connection in %d seconds...\n", wait_time);
            sleep(wait_time);
            continue;
        }

        printf("Connecting to wss://%s:%s%s\n", 
               g_ws_data->hostname, g_ws_data->port, g_ws_data->path);

        // 重置重试计数
        retry_count = 0;

        // 事件循环
        printf("begin lws_service\n");
        while (g_context) {
            int n = lws_service(g_context, 50); // 50ms timeout
            if (n < 0) {
                printf("lws_service error: %d\n", n);
                break;
            }

            if (g_iHasDisconnected)
            {
                printf("lws_service break\n");
                break;
            }
        }

        // 如果到达这里，说明连接被断开了
        if (g_should_reconnect) {
            retry_count++;
            printf("Connection lost, retry count: %d\n", retry_count);
            sleep(5);
        }
    }

    if (retry_count >= max_retry_count) {
        printf("Maximum retry attempts reached. Stopping WebSocket client.\n");
    }

    return NULL;
}

/**
 * 发送二进制数据
 */
int websocket_send_binary(const char *data, int size)
{
    if (!g_iHasConnected || !g_iHasShaked || !g_wsi) {
        return -1;
    }

    unsigned char *buf = malloc(LWS_PRE + size);
    if (!buf) {
        printf("Failed to allocate memory for binary data\n");
        return -1;
    }

    memcpy(buf + LWS_PRE, data, size);
    
    int ret = lws_write(g_wsi, buf + LWS_PRE, size, LWS_WRITE_BINARY);
    free(buf);
    
    if (ret < 0) {
        printf("Failed to send binary data\n");
    }
    
    return ret;
}

/**
 * 发送文本数据
 */
int websocket_send_text(const char *data, int size)
{
    if (!g_iHasConnected || !g_wsi) {
        printf("WebSocket not connected, cannot send text data\n");
        return -1;
    }

    unsigned char *buf = malloc(LWS_PRE + size);
    if (!buf) {
        printf("Failed to allocate memory for text data\n");
        return -1;
    }

    memcpy(buf + LWS_PRE, data, size);
    
    int ret = lws_write(g_wsi, buf + LWS_PRE, size, LWS_WRITE_TEXT);
    free(buf);
    
    if (ret < 0) {
        printf("Failed to send text data\n");
    } else {
        g_iHasShaked = 1;
    }
    
    return ret;
}

/**
 * 设置回调函数和数据
 */
int websocket_set_callbacks(ws_recv_callback_t bin_cb, ws_recv_callback_t txt_cb, 
                           websocket_data_t *ws_data)
{
    g_ws_recv_bin_cb = bin_cb;
    g_ws_recv_txt_cb = txt_cb;
    g_ws_data = ws_data;
    return 0;
}

/**
 * 启动WebSocket线程
 */
int websocket_start()
{
    pthread_attr_t default_attr = { \
        PTHREAD_DEFAULT_PRIORITY, /* priority */ \
        PTHREAD_DEFAULT_POLICY,   /* policy */ \
        PTHREAD_EXPLICIT_SCHED,   /* inheritsched */ \
        PTHREAD_CREATE_JOINABLE,  /* detachstate */ \
        0,                        /* affinity */ \
        NULL,                     /* stackaddr */ \
        409600,    /* stacksize */ \
    };

    g_should_reconnect = 1;

    if (pthread_create(&g_ws_thread, &default_attr, websocket_thread, NULL) != 0) {
        printf("Failed to create WebSocket thread\n");
        return -1;
    }
    
    return 0;
}

/**
 * 停止WebSocket连接
 */
int websocket_stop()
{
    g_should_reconnect = 0;
    
    if (g_context) {
        lws_context_destroy(g_context);
        g_context = NULL;
    }
    
    if (g_ws_thread) {
        pthread_join(g_ws_thread, NULL);
    }
    
    return 0;
}
