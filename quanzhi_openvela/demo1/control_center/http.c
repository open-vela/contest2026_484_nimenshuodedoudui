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
#include <curl/curl.h>
#include "cJSON.h"
#include "http.h"

#include <syslog.h>
#define printf(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

// 回调函数，用于处理HTTP响应数据
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    char **response_ptr = (char**)userp;
    
    // 重新分配内存以容纳新数据
    *response_ptr = realloc(*response_ptr, strlen(*response_ptr) + total_size + 1);
    if (*response_ptr == NULL) {
        return 0;
    }
    
    strncat(*response_ptr, (char*)contents, total_size);
    return total_size;
}

/**
 * 激活设备
 * 
 * @param pHttpData http连接数据
 * @param codebuf 存储激活码
 * @return 0-已经激活, 1-得到了激活码(等待激活), -1-失败
 */
int active_device(p_http_data_t pHttpData, char *codebuf) {
    int ret = -1;

    CURL* curl;
    CURLcode res;
    char error_buffer[CURL_ERROR_SIZE];
    char *readBuffer = NULL;

    const char *url = pHttpData->url;
    const char *postFields = pHttpData->post;
    const char *headers = pHttpData->headers;

    // 初始化读取缓冲区
    syslog(LOG_INFO, "http: before malloc\n");
    readBuffer = malloc(1);
    if (readBuffer == NULL) {
        return -1;
    }
    syslog(LOG_INFO, "http: after malloc\n");
    readBuffer[0] = '\0';

    /* curl global init is unsafe on this NuttX build. */
    syslog(LOG_INFO, "http: before curl_easy_init\n");
    curl = curl_easy_init();
    syslog(LOG_INFO, "http: after curl_easy_init=%p\n", curl);
    if(curl) {
        error_buffer[0] = '0';
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        // 设置URL
        curl_easy_setopt(curl, CURLOPT_URL, url);

        // 设置POST数据
        printf("PostFields: %s\n", postFields);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields);

        // 设置请求头
        struct curl_slist *curlheaders = NULL;
        
        printf("headers: %s\n", headers);
        cJSON *headerJson = cJSON_Parse(headers);
        if (headerJson == NULL) {
            fprintf(stderr, "Failed to parse headers JSON\n");
            curl_easy_cleanup(curl);
            /* no curl_global_cleanup */
            free(readBuffer);
            return -1;
        }

        cJSON *item = NULL;
        cJSON_ArrayForEach(item, headerJson) {
            if (cJSON_IsString(item)) {
                char headerLine[256]; 
                snprintf(headerLine, sizeof(headerLine), "%s: %s", item->string, item->valuestring);
                printf("parsed head: %s\n", headerLine);
                curlheaders = curl_slist_append(curlheaders, headerLine);
            }
        }

        cJSON_Delete(headerJson);

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlheaders);

        // 设置回调函数以处理响应数据
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        // 禁用对服务器SSL证书的验证
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        // 禁用对SSL证书中主机名的验证
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        // 执行请求
        res = curl_easy_perform(curl);

        // 检查请求是否成功
        if(res != CURLE_OK) {
            syslog(LOG_INFO, "http: curl failed code=%d text=%s detail=%s\n",
                   (int)res, curl_easy_strerror(res),
                   error_buffer[0] ? error_buffer : "(none)");
        } else {
            printf("Response: %s\n", readBuffer);

            // 解析响应数据
            /* 
             * 响应数据格式如下: 有activation字段表示未激活,否则表示激活了
                {
                    "mqtt": {
                        "endpoint": "post-cn-apg3xckag01.mqtt.aliyuncs.com",
                        "client_id": "GID_test@@@00_0c_29_bd_43_04",
                        "username": "Signature|LTAI5tF8J3CrdWmRiuTjxHbF|post-cn-apg3xckag01",
                        "password": "ObF5W8laHbuh9Qr5Ok07V9LhLLg=",
                        "publish_topic": "device-server",
                        "subscribe_topic": "devices/00_0c_29_bd_43_04"
                    },
                    "server_time": {
                        "timestamp": 1741940708193,
                        "timezone_offset": 480
                    },
                    "firmware": {
                        "url": ""
                    },
                    "activation": {
                        "code": "600206",
                        "message": "xiaozhi.me\n600206"
                    }
                }            
            */
            cJSON *responseJson = cJSON_Parse(readBuffer);
            if (responseJson == NULL) {
                fprintf(stderr, "Failed to parse response JSON\n");
                ret = -1;
            } else {
                cJSON *activation = cJSON_GetObjectItem(responseJson, "activation");
                if (activation != NULL) {
                    cJSON *code = cJSON_GetObjectItem(activation, "code");
                    if (code != NULL && cJSON_IsString(code)) {
                        printf("Activation Code: %s\n", code->valuestring);
                        strncpy(codebuf, code->valuestring, 20); // 存储激活码到codebuf
                        codebuf[19] = '\0'; // 确保字符串终止
                        ret = 1; // 表示未激活
                    } else {
                        fprintf(stderr, "No activation code found\n");
                        ret = -1;
                    }
                } else {
                    fprintf(stderr, "Device has been Activated\n");
                    ret = 0;
                }
                cJSON_Delete(responseJson);
            }
        }

        // 清理请求头
        curl_slist_free_all(curlheaders);

        // 清理
        curl_easy_cleanup(curl);
    }
    /* no curl_global_cleanup */

    // 释放读取缓冲区
    if (readBuffer != NULL) {
        free(readBuffer);
    }

    return ret;
}