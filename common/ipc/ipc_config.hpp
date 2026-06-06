#ifndef UPC_IPC_CONFIG_HPP
#define UPC_IPC_CONFIG_HPP

/** Glavo 监听、UI 连接的 Unix 域套接字（环境变量 UPC_IPC_SOCKET 可覆盖） */
#ifndef UPC_IPC_SOCKET_DEFAULT
#define UPC_IPC_SOCKET_DEFAULT "/tmp/upc_glavo.sock"
#endif

/** 相机进程 IPC 套接字（环境变量 UPC_CAMERA_IPC_SOCKET 可覆盖） */
#ifndef UPC_CAMERA_IPC_SOCKET_DEFAULT
#define UPC_CAMERA_IPC_SOCKET_DEFAULT "/tmp/upc_camera.sock"
#endif

/** 相机预览默认 POSIX 共享内存名（shm_open 用，须以 / 开头） */
#ifndef UPC_CAMERA_SHM_DEFAULT
#define UPC_CAMERA_SHM_DEFAULT "/upc_camera_preview"
#endif

/** 默认环形槽数量（triple buffering） */
#ifndef UPC_IPC_SHM_DEFAULT_SLOT_COUNT
#define UPC_IPC_SHM_DEFAULT_SLOT_COUNT 3u
#endif

#endif
