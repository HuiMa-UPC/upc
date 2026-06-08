#ifndef UPC_IPC_CONFIG_HPP
#define UPC_IPC_CONFIG_HPP

/** Glavo 监听、UI 连接的 Unix 域套接字（环境变量 UPC_IPC_SOCKET 可覆盖） */
#ifndef UPC_IPC_SOCKET_DEFAULT
#define UPC_IPC_SOCKET_DEFAULT "/tmp/upc_glavo.sock"
#endif

/** UI hub 模式下，已断开的 peer 后台重连间隔（毫秒） */
#ifndef UPC_IPC_PEER_RECONNECT_INTERVAL_MS
#define UPC_IPC_PEER_RECONNECT_INTERVAL_MS 3000
#endif

/** rongsu.sh 等待后端 listen socket 出现的超时（毫秒） */
#ifndef UPC_IPC_PEER_STARTUP_WAIT_MS
#define UPC_IPC_PEER_STARTUP_WAIT_MS 10000
#endif

#endif
