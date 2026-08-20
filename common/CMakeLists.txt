# common/ — 统一静态库 upc_common（IPC + Middleware + Debug + Process）
#
# 对外链接：
#   upc::common      — 完整 common 库（推荐新进程使用）
#   upc::ipc         — 兼容别名
#   middleware::port — 兼容别名
#   upc::debug       — 兼容别名
#   upc::process     — 兼容别名

set(UPC_COMMON_TARGET upc_common)

set(UPC_COMMON_SOURCES
    # process
    ${CMAKE_CURRENT_SOURCE_DIR}/process/process_ctl.cpp
    # debug
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/rs_log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/upc_sys_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/rs_msg_format.cpp
    # middleware
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/msg_bus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/msg_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/ring_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/mem_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/middleware/buffer_pool.cpp
    # ipc
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_endpoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_poller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_reg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_msg_bus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/msg_port.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/dma_heap_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_dma_frame_ring.cpp
    # fs_manifest（跨进程共用的本地清单 JSON：G-code / 打印历史）
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_manifest/gcode_manifest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_manifest/print_history_manifest.cpp
    # cal.csv Config 段解析（运行时可选覆盖）
    ${CMAKE_CURRENT_SOURCE_DIR}/config/cal_config.cpp
)

add_library(${UPC_COMMON_TARGET} STATIC ${UPC_COMMON_SOURCES})
add_library(upc::common ALIAS ${UPC_COMMON_TARGET})

# 兼容旧目标名（均为同一静态库）
add_library(upc_ipc ALIAS ${UPC_COMMON_TARGET})
add_library(upc::ipc ALIAS ${UPC_COMMON_TARGET})
add_library(middleware ALIAS ${UPC_COMMON_TARGET})
add_library(middleware::port ALIAS ${UPC_COMMON_TARGET})
add_library(upc_debug ALIAS ${UPC_COMMON_TARGET})
add_library(upc::debug ALIAS ${UPC_COMMON_TARGET})
add_library(upc_process ALIAS ${UPC_COMMON_TARGET})
add_library(upc::process ALIAS ${UPC_COMMON_TARGET})

target_compile_features(${UPC_COMMON_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_COMMON_TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/debug
        ${CMAKE_CURRENT_SOURCE_DIR}/process
        ${CMAKE_CURRENT_SOURCE_DIR}/ipc
        ${CMAKE_CURRENT_SOURCE_DIR}/middleware
        ${CMAKE_CURRENT_SOURCE_DIR}/fs_manifest
        ${CMAKE_CURRENT_SOURCE_DIR}/config
)

# 工具链 sysroot 缺 <linux/dma-heap.h>，用项目内 vendoring 的 UAPI 头兜底
target_include_directories(${UPC_COMMON_TARGET}
    SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ipc/compat
)

# UPC_DEBUG / UPC_LOG_ENABLED（原 upc_debug 子库逻辑）
if(NOT DEFINED UPC_DEBUG)
    set(UPC_DEBUG OFF)
endif()

if(NOT DEFINED UPC_LOG_ENABLED)
    if(UPC_DEBUG)
        set(UPC_LOG_ENABLED 1)
    else()
        set(UPC_LOG_ENABLED 0)
    endif()
endif()

if(UPC_DEBUG)
    target_compile_definitions(${UPC_COMMON_TARGET} PUBLIC UPC_DEBUG=1)
    message(STATUS "upc_common: logging ENABLED (/var/log/topic)")
else()
    target_compile_definitions(${UPC_COMMON_TARGET} PUBLIC UPC_DEBUG=0)
    message(STATUS "upc_common: logging DISABLED")
endif()

target_compile_definitions(${UPC_COMMON_TARGET} PUBLIC UPC_LOG_ENABLED=${UPC_LOG_ENABLED})

# middleware 队列/内存池参数（可通过 -D 覆盖）
if(NOT DEFINED MSG_BUS_QUEUE_DEPTH)
    set(MSG_BUS_QUEUE_DEPTH 512)
endif()
if(NOT DEFINED MEM_POOL_MAX_BUFFERS)
    set(MEM_POOL_MAX_BUFFERS 256)
endif()
target_compile_definitions(${UPC_COMMON_TARGET}
    PUBLIC
        MSG_BUS_QUEUE_DEPTH=${MSG_BUS_QUEUE_DEPTH}
        MEM_POOL_MAX_BUFFERS=${MEM_POOL_MAX_BUFFERS}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    target_link_libraries(${UPC_COMMON_TARGET} PUBLIC Threads::Threads rt)
endif()

target_compile_options(${UPC_COMMON_TARGET}
    PRIVATE
        -Wall -Wextra -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wpedantic
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

if(MIDDLEWARE_STANDALONE)
    message(STATUS "upc_common v1.0.0 (standalone)")
endif()

# 供旧脚本读取（若存在）
set(UPC_COMMON_TARGET ${UPC_COMMON_TARGET} PARENT_SCOPE)
set(UPC_IPC_TARGET ${UPC_COMMON_TARGET} PARENT_SCOPE)
set(MIDDLEWARE_TARGET ${UPC_COMMON_TARGET} PARENT_SCOPE)
set(UPC_DEBUG_TARGET ${UPC_COMMON_TARGET} PARENT_SCOPE)
