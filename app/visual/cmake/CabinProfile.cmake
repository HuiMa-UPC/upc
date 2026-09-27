# Build-host Python embeds JSON + PNG bytes into a private generated header.
# Nothing is installed or copied to /userdata by this helper.
set(_UPC_CABIN_PROFILE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

function(upc_embed_cabin_profile target)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(profile_source "${_UPC_CABIN_PROFILE_SOURCE_DIR}" ABSOLUTE)
    set(profile_assets "${profile_source}/calibration")
    set(profile_output_dir "${CMAKE_CURRENT_BINARY_DIR}/cabin-profile")
    set(profile_header "${profile_output_dir}/cabin_bundled_profile.hpp")
    string(MD5 profile_target_suffix "${profile_header}")
    set(profile_target "upc_cabin_profile_${profile_target_suffix}")
    if(NOT TARGET "${profile_target}")
        set(profile_generator "${profile_source}/tools/embed_cabin_profile.py")
        # The JSON owns the template list, including references outside the
        # original candidate subdirectory. Refresh the exact build dependencies
        # whenever it changes; missing templates must fail, not leave a stale
        # previously generated header in an otherwise successful build.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${profile_generator}"
            "${profile_assets}/cabin_door_candidate.json"
            "${profile_assets}/station_172_17_101_78_candidate.json")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${profile_generator}"
                --source-dir "${profile_assets}" --list-inputs
            RESULT_VARIABLE profile_inputs_result
            OUTPUT_VARIABLE profile_inputs_output
            ERROR_VARIABLE profile_inputs_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT profile_inputs_result STREQUAL "0")
            message(FATAL_ERROR "Cannot enumerate cabin profile inputs: ${profile_inputs_error}")
        endif()
        string(REPLACE "\r\n" "\n" profile_inputs_output "${profile_inputs_output}")
        string(REPLACE "\n" ";" profile_inputs "${profile_inputs_output}")
        if(NOT profile_inputs)
            message(FATAL_ERROR "Cabin profile input dependency list is empty")
        endif()
        add_custom_command(OUTPUT "${profile_header}"
            COMMAND "${Python3_EXECUTABLE}" "${profile_generator}"
                --source-dir "${profile_assets}" --output "${profile_header}"
            DEPENDS "${profile_generator}" ${profile_inputs}
            COMMENT "Embedding versioned cabin profile and marker templates"
            VERBATIM)
        add_custom_target("${profile_target}" DEPENDS "${profile_header}")
    endif()
    add_dependencies("${target}" "${profile_target}")
    target_include_directories("${target}" PRIVATE "${profile_output_dir}")
endfunction()
