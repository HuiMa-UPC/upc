set(MIDDLEWARE_TARGET middleware)

set(MIDDLEWARE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/message_bus/msg_bus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/message_bus/msg_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/message_bus/ring_buffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_pool/mem_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_pool/buffer_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/thread_pool/thread_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/debug/mw_debug.cpp
)

add_library(${MIDDLEWARE_TARGET} STATIC ${MIDDLEWARE_SOURCES})
add_library(middleware::port ALIAS ${MIDDLEWARE_TARGET})

target_compile_features(${MIDDLEWARE_TARGET} PUBLIC cxx_std_17)

target_include_directories(${MIDDLEWARE_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/message_bus>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/memory_pool>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thread_pool>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/debug>
        $<INSTALL_INTERFACE:include/middleware/message_bus>
        $<INSTALL_INTERFACE:include/middleware/memory_pool>
        $<INSTALL_INTERFACE:include/middleware/thread_pool>
        $<INSTALL_INTERFACE:include/middleware/debug>
)

# 可通过 -D 覆盖，例如：cmake -DMSG_BUS_QUEUE_DEPTH=512
if(NOT DEFINED MSG_BUS_QUEUE_DEPTH)
    set(MSG_BUS_QUEUE_DEPTH 512)
endif()
if(NOT DEFINED MEM_POOL_MAX_BUFFERS)
    set(MEM_POOL_MAX_BUFFERS 256)
endif()
if(NOT DEFINED THREAD_POOL_MAX_THREADS)
    set(THREAD_POOL_MAX_THREADS 4)
endif()
if(NOT DEFINED THREAD_POOL_QUEUE_DEPTH)
    set(THREAD_POOL_QUEUE_DEPTH 64)
endif()

# 调试日志总开关：cmake -DMIDDLEWARE_DEBUG=ON
option(MIDDLEWARE_DEBUG "Print middleware debug trace to stderr" OFF)
option(MIDDLEWARE_DEBUG_VERBOSE_POOL "Log every MemPool/BufferPool alloc/free (noisy)" OFF)

target_compile_definitions(${MIDDLEWARE_TARGET}
    PUBLIC
        MSG_BUS_QUEUE_DEPTH=${MSG_BUS_QUEUE_DEPTH}
        MEM_POOL_MAX_BUFFERS=${MEM_POOL_MAX_BUFFERS}
        THREAD_POOL_MAX_THREADS=${THREAD_POOL_MAX_THREADS}
        THREAD_POOL_QUEUE_DEPTH=${THREAD_POOL_QUEUE_DEPTH}
)

if(MIDDLEWARE_DEBUG)
    target_compile_definitions(${MIDDLEWARE_TARGET} PUBLIC MIDDLEWARE_DEBUG=1)
    message(STATUS "middleware: debug logging ENABLED (stderr, MSGBUS only by default)")
    if(MIDDLEWARE_DEBUG_VERBOSE_POOL)
        target_compile_definitions(${MIDDLEWARE_TARGET} PUBLIC
            MW_DEBUG_MEMPOOL=1
            MW_DEBUG_BUFFERPOOL=1)
        message(STATUS "middleware: verbose pool debug ENABLED")
    endif()
else()
    target_compile_definitions(${MIDDLEWARE_TARGET} PUBLIC MIDDLEWARE_DEBUG=0)
endif()

target_compile_options(${MIDDLEWARE_TARGET}
    PRIVATE
        -Wall -Wextra -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wpedantic
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    target_link_libraries(${MIDDLEWARE_TARGET} PUBLIC Threads::Threads)
endif()

# 向父工程导出（供根 CMakeLists 可选使用）
set(MIDDLEWARE_TARGET ${MIDDLEWARE_TARGET} PARENT_SCOPE)

if(MIDDLEWARE_STANDALONE)
    message(STATUS "middleware v1.0.0 (standalone)")
endif()
