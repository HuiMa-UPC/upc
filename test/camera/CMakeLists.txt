add_executable(test_camera
    main.cpp
    file_frame_source.cpp
    test_camera_service.cpp
)

target_compile_features(test_camera PRIVATE cxx_std_17)

target_include_directories(test_camera
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/common/process
)

target_link_libraries(test_camera
    PRIVATE
        upc::ipc
        upc::process
        upc::debug
)
