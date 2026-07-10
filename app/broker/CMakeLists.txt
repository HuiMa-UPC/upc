add_executable(rongsu_broker main.cpp)

target_compile_features(rongsu_broker PRIVATE cxx_std_17)

target_include_directories(rongsu_broker
    PRIVATE
        ${CMAKE_SOURCE_DIR}/common/process
        ${CMAKE_SOURCE_DIR}/common/ipc
)

target_link_libraries(rongsu_broker
    PRIVATE
        upc::common
)

target_compile_options(rongsu_broker
    PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
)
