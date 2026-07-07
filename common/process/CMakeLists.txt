add_library(upc_process STATIC process_ctl.cpp log_rotating.cpp)
add_library(upc::process ALIAS upc_process)

target_include_directories(upc_process
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(upc_process PUBLIC cxx_std_17)
