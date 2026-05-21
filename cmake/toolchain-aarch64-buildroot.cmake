# ATK RK356x 交叉编译
# cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-buildroot.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER /opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-g++)
set(CMAKE_SYSROOT /opt/atk-dlrk356x-toolchain/aarch64-buildroot-linux-gnu/sysroot)

set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
