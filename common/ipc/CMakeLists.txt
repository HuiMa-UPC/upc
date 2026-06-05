set(UPC_IPC_TARGET upc_ipc)

set(UPC_IPC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc_wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc_endpoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc_msg_bus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/msg_port.cpp
)

add_library(${UPC_IPC_TARGET} STATIC ${UPC_IPC_SOURCES})
add_library(upc::ipc ALIAS ${UPC_IPC_TARGET})

target_compile_features(${UPC_IPC_TARGET} PUBLIC cxx_std_17)

target_include_directories(${UPC_IPC_TARGET}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/common/middleware/message_bus
        ${CMAKE_SOURCE_DIR}/common/middleware/memory_pool
)

target_link_libraries(${UPC_IPC_TARGET}
    PUBLIC
        middleware::port
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    target_link_libraries(${UPC_IPC_TARGET} PUBLIC Threads::Threads)
endif()

target_compile_options(${UPC_IPC_TARGET}
    PRIVATE
        -Wall -Wextra
)

set(UPC_IPC_TARGET ${UPC_IPC_TARGET} PARENT_SCOPE)
