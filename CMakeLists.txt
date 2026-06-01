cmake_minimum_required(VERSION 3.16)

# 选择交叉编译环境
set(TOOLCHAIN  toolchain-aarch64-buildroot)
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/${TOOLCHAIN}.cmake"
    CACHE FILEPATH "Cross toolchain (optional)")

project(rongsu_ui 
        VERSION 1.0.0
        LANGUAGES C CXX
)
# 设置C/C++标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 构建生成路径json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 打印配置摘要
message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Toolchain: ${TOOLCHAIN}")
message(STATUS "========================================")

# 公共编译选项
add_compile_options(-Wall -Wextra -O3)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

# 子模块
add_subdirectory(3rdparty/lv_port_linux-release-v9.5)
add_subdirectory(middleware)
add_subdirectory(services)
add_subdirectory(ui)

# 可执行文件
add_executable(${PROJECT_NAME}
    app/src/main.cpp
    app/src/platform.cpp
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_SOURCE_DIR}/app/inc"
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ui::port
        services::port
        middleware::port
        lvgl::port
        
)
