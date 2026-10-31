# Only the ESP32-P4 has an ESP-Hosted co-processor. Its firmware is built
# from the sources that ship with the esp_hosted component and embedded in
# the application, so host and co-processor run the same ESP-Hosted release.
set(srcs)
if(CONFIG_COPROCESSOR_UPDATE_ENABLE)
    list(APPEND srcs "coprocessor_update.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bootloader_support esp_app_format nvs_flash)

if(CONFIG_COPROCESSOR_UPDATE_ENABLE)
    idf_component_get_property(hosted_dir espressif__esp_hosted COMPONENT_DIR)
    set(version_header "${hosted_dir}/slave/main/esp_hosted_coprocessor_fw_ver.h")
    file(READ "${version_header}" version_header_text)
    string(REGEX MATCH
           "MAJOR_1[ \t]+([0-9]+).*MINOR_1[ \t]+([0-9]+).*PATCH_1[ \t]+([0-9]+)"
           version_match "${version_header_text}")
    if(NOT version_match)
        message(FATAL_ERROR "No firmware version in ${version_header}")
    endif()
    set(fw_version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

    # The component manager rejects changes inside managed components, so the
    # co-processor project is built from a copy.
    set(fw_dir "${CMAKE_CURRENT_BINARY_DIR}/esp_hosted_${fw_version}")
    set(fw_bin "${fw_dir}/build/network_adapter.bin")
    idf_build_get_property(python PYTHON)
    idf_build_get_property(idf_path IDF_PATH)
    add_custom_command(
        OUTPUT "${fw_bin}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${hosted_dir}/slave" "${fw_dir}/slave"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${hosted_dir}/common" "${fw_dir}/common"
        COMMAND "${CMAKE_COMMAND}" -E env --unset=IDF_TARGET --unset=SDKCONFIG
                "${python}" "${idf_path}/tools/idf.py"
                -C "${fw_dir}/slave" -B "${fw_dir}/build"
                -D IDF_TARGET=esp32c6 -D "SDKCONFIG=${fw_dir}/sdkconfig"
                build
        DEPENDS "${version_header}"
        COMMENT "Building ESP-Hosted ${fw_version} firmware for the ESP32-C6"
        USES_TERMINAL
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${fw_bin}" BINARY)
endif()
