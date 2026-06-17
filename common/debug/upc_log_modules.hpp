#ifndef UPC_LOG_MODULES_HPP
#define UPC_LOG_MODULES_HPP

/**
 * 总开关 UPC_DEBUG 由根 CMakeLists 注入（ON=1 / OFF=0）。
 * 子开关 UPC_MOD_* 仅在此设默认值；关闭某模块：-DUPC_MOD_MCU_LINK=0
 * 逻辑：UPC_DEBUG=OFF 时所有日志宏为空；ON 时再看对应 UPC_MOD_* 是否为 1。
 *
 * 注意：UPC_LOG_* 为打印宏，UPC_MOD_* 为模块子开关，勿混用同名。
 */

#ifndef UPC_DEBUG
#define UPC_DEBUG 0
#endif

#if UPC_DEBUG

#ifndef UPC_MOD_MSG_BUS
#define UPC_MOD_MSG_BUS 1
#endif
#ifndef UPC_MOD_IPC
#define UPC_MOD_IPC 1
#endif
#ifndef UPC_MOD_MEM_POOL
#define UPC_MOD_MEM_POOL 1
#endif
#ifndef UPC_MOD_BUFFER_POOL
#define UPC_MOD_BUFFER_POOL 1
#endif
#ifndef UPC_MOD_THREAD_POOL
#define UPC_MOD_THREAD_POOL 1
#endif
#ifndef UPC_MOD_GLAVO_SERVICE
#define UPC_MOD_GLAVO_SERVICE 1
#endif
#ifndef UPC_MOD_MCU_LINK
#define UPC_MOD_MCU_LINK 1
#endif
#ifndef UPC_MOD_CAL_PARSER
#define UPC_MOD_CAL_PARSER 1
#endif
#ifndef UPC_MOD_PARSER_CONTEXT
#define UPC_MOD_PARSER_CONTEXT 1
#endif
#ifndef UPC_MOD_GCODE_WORKER
#define UPC_MOD_GCODE_WORKER 1
#endif
#ifndef UPC_MOD_OVF_WORKER
#define UPC_MOD_OVF_WORKER 1
#endif
#ifndef UPC_MOD_SERVICE_BASE
#define UPC_MOD_SERVICE_BASE 1
#endif
#ifndef UPC_MOD_SYS_MONITOR
#define UPC_MOD_SYS_MONITOR 1
#endif
#ifndef UPC_MOD_CAMERA_SERVICE
#define UPC_MOD_CAMERA_SERVICE 1
#endif
#ifndef UPC_MOD_TEST_CAMERA
#define UPC_MOD_TEST_CAMERA 0
#endif
#ifndef UPC_MOD_CAMERA_PREVIEW
#define UPC_MOD_CAMERA_PREVIEW 0
#endif

#else /* !UPC_DEBUG */

#define UPC_MOD_MSG_BUS 0
#define UPC_MOD_IPC 0
#define UPC_MOD_MEM_POOL 0
#define UPC_MOD_BUFFER_POOL 0
#define UPC_MOD_THREAD_POOL 0
#define UPC_MOD_GLAVO_SERVICE 0
#define UPC_MOD_MCU_LINK 0
#define UPC_MOD_CAL_PARSER 0
#define UPC_MOD_PARSER_CONTEXT 0
#define UPC_MOD_GCODE_WORKER 0
#define UPC_MOD_OVF_WORKER 0
#define UPC_MOD_SERVICE_BASE 0
#define UPC_MOD_SYS_MONITOR 0
#define UPC_MOD_CAMERA_SERVICE 0
#define UPC_MOD_TEST_CAMERA 0
#define UPC_MOD_CAMERA_PREVIEW 0

#endif /* UPC_DEBUG */

#endif /* UPC_LOG_MODULES_HPP */
