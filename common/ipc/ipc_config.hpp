#ifndef UPC_IPC_CONFIG_HPP
#define UPC_IPC_CONFIG_HPP

/** Glavo 监听、UI 连接的 Unix 域套接字（环境变量 UPC_IPC_SOCKET 可覆盖） */
#ifndef UPC_IPC_SOCKET_DEFAULT
#define UPC_IPC_SOCKET_DEFAULT "/tmp/upc_glavo.sock"
#endif

#endif
