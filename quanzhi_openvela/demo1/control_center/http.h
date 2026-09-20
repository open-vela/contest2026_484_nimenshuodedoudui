#ifndef _HTTP_H_
#define _HTTP_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct http_data_t {
    const char *url;
    const char *post;
    const char *headers;
} http_data_t, *p_http_data_t;

/**
 * 激活设备
 * 
 * @param pHttpData http连接数据
 * @param codebuf 存储激活码
 * @return 0-已经激活, 1-得到了激活码(等待激活), -1-失败
 */
int active_device(p_http_data_t pHttpData, char *codebuf);

#ifdef __cplusplus
}
#endif

#endif