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
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

// Include cJSON library
#include "cJSON.h"

/**
 * 获取无线网卡的 MAC 地址
 *
 * 该函数遍历 /sys/class/net/ 目录下的所有网络接口，
 * 查找名称以 "wlan" 或 "wlp" 开头的无线网卡接口，
 * 并读取其对应的 address 文件以获取 MAC 地址。
 * 如果未找到无线网卡接口，则读取第一个可用的网卡接口。
 *
 * @return 无线网卡的 MAC 地址，如果未找到则返回空字符串
 * @note 调用者需要负责释放返回的字符串内存
 */
char* get_wireless_mac_address(void) {
    static char *mac_str = NULL;
    
    if (!mac_str) 
        mac_str = malloc(32);

    int sock;
    struct ifreq ifr;
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return NULL;
    }
    
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ-1);
    ifr.ifr_name[IFNAMSIZ-1] = '\0';
    
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        close(sock);
        return NULL;
    }
    
    unsigned char* mac = (unsigned char*)ifr.ifr_hwaddr.sa_data;
    sprintf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    close(sock);
    return mac_str;
}

/**
 * 生成 UUID
 *
 * 该函数使用 rand() 生成一个随机的 UUID。
 * UUID 的格式为 8-4-4-4-12 的 32 位十六进制数字。
 *
 * @return 生成的 UUID 字符串
 * @note 调用者需要负责释放返回的字符串内存
 */
char* generate_uuid(void) {
    // 初始化随机数种子
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }

    char *uuid = malloc(37); // 36字符 + 空终止符
    if (uuid == NULL) {
        return NULL;
    }

    const char *hex_chars = "0123456789abcdef";
    int i;

    // 生成 UUID 的各个部分
    for (i = 0; i < 8; i++) {
        uuid[i] = hex_chars[rand() % 16];
    }
    uuid[8] = '-';
    for (i = 9; i < 13; i++) {
        uuid[i] = hex_chars[rand() % 16];
    }
    uuid[13] = '-';
    uuid[14] = '4'; // UUID version 4
    for (i = 15; i < 18; i++) {
        uuid[i] = hex_chars[rand() % 16];
    }
    uuid[18] = '-';
    uuid[19] = hex_chars[(rand() % 4) + 8]; // 8, 9, a, b
    for (i = 20; i < 23; i++) {
        uuid[i] = hex_chars[rand() % 16];
    }
    uuid[23] = '-';
    for (i = 24; i < 36; i++) {
        uuid[i] = hex_chars[rand() % 16];
    }
    uuid[36] = '\0';

    return uuid;
}

/**
 * 创建包含 MAC 地址和 UUID 的 JSON 对象
 *
 * @return cJSON 对象，包含 mac_address 和 uuid 字段
 * @note 调用者需要负责使用 cJSON_Delete() 释放返回的 JSON 对象
 */
cJSON* create_device_info_json(void) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    // 获取 MAC 地址
    char *mac_address = get_wireless_mac_address();
    if (mac_address != NULL) {
        cJSON_AddStringToObject(root, "mac_address", mac_address);
        free(mac_address);
    } else {
        cJSON_AddNullToObject(root, "mac_address");
    }

    // 生成 UUID
    char *uuid = generate_uuid();
    if (uuid != NULL) {
        cJSON_AddStringToObject(root, "uuid", uuid);
        free(uuid);
    } else {
        cJSON_AddNullToObject(root, "uuid");
    }

    return root;
}

/**
 * 示例使用函数
 */
void example_usage(void) {
    // 获取无线网卡的 MAC 地址
    char *wireless_mac = get_wireless_mac_address();
    if (wireless_mac != NULL) {
        printf("Wireless MAC Address: %s\n", wireless_mac);
        free(wireless_mac);
    } else {
        fprintf(stderr, "Failed to get wireless MAC address\n");
    }

    // 生成多个 UUID 并打印
    for (int i = 0; i < 5; ++i) {
        char *uuid = generate_uuid();
        if (uuid != NULL) {
            printf("Generated UUID %d: %s\n", i + 1, uuid);
            free(uuid);
        }
    }

    // 创建并打印 JSON 对象
    cJSON *device_info = create_device_info_json();
    if (device_info != NULL) {
        char *json_str = cJSON_Print(device_info);
        if (json_str != NULL) {
            printf("Device Info JSON: %s\n", json_str);
            free(json_str);
        }
        cJSON_Delete(device_info);
    }
}

#if 0
int main(void) {
    example_usage();
    return 0;
}
#endif