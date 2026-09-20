#ifndef __UUID_H__
#define __UUID_H__

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 获取无线网卡的 MAC 地址
 *
 * 该函数遍历 /sys/class/net/ 目录下的所有网络接口，
 * 查找名称以 "wlan" 或 "wlp" 开头的无线网卡接口，
 * 并读取其对应的 address 文件以获取 MAC 地址。
 *
 * @return 无线网卡的 MAC 地址，如果未找到则返回 NULL
 * @note 调用者需要负责使用 free() 释放返回的字符串
 */
char* get_wireless_mac_address(void);

/**
 * 生成 UUID
 *
 * 该函数使用随机数生成器生成一个随机的 UUID。
 * UUID 的格式为 8-4-4-4-12 的 32 位十六进制数字。
 *
 * @return 生成的 UUID 字符串
 * @note 调用者需要负责使用 free() 释放返回的字符串
 */
char* generate_uuid(void);

/**
 * 创建包含 MAC 地址和 UUID 的 JSON 对象
 *
 * @return cJSON 对象，包含 mac_address 和 uuid 字段
 * @note 调用者需要负责使用 cJSON_Delete() 释放返回的 JSON 对象
 */
cJSON* create_device_info_json(void);

/**
 * 示例使用函数
 */
void example_usage(void);

#ifdef __cplusplus
}
#endif

#endif