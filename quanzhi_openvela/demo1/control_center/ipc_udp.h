#ifndef TRANSFER_CENTER_H
#define TRANSFER_CENTER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*transfer_callback_t)(char *buffer, size_t size, void *user_data);

/**
 * 传输的各方被称为endpoint
 * 数据结构体，包含套接字、端口、服务器地址和回调函数
 */
typedef struct ipc_endpoint_t {  
    void *priv;
    void *user_data;
    transfer_callback_t cb;  // 接收到远端的客户端发来的信息后使用它来处理
    int (*send)(struct ipc_endpoint_t *self, const char *data, int len); // 发送数据的函数指针
    int (*recv)(struct ipc_endpoint_t *self, unsigned char *data, int maxlen, int *retlen); // 接收数据的函数指针
} ipc_endpoint_t, *p_ipc_endpoint_t;

// 创建一个UDP类型的IPC端点
p_ipc_endpoint_t ipc_endpoint_create_udp(int port_local, int port_remote, transfer_callback_t cb, void *user_data);

// 销毁IPC端点，释放相关资源
void ipc_endpoint_destroy_udp(p_ipc_endpoint_t pendpoint);

#ifdef __cplusplus
}
#endif

#endif // TRANSFER_H