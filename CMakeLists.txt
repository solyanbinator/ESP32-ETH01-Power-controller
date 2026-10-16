cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Собирать только main и его зависимости: Wi-Fi, Bluetooth и прочие
# неиспользуемые компоненты IDF не компилируются и не попадают в прошивку.
idf_build_set_property(MINIMAL_BUILD ON)

# Версия прошивки: показывается в веб-интерфейсе; после обновления по сети страница сверяет
# версию и время сборки, чтобы заметить откат на предыдущую прошивку
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.1.1")
endif()

project(esp32_eth01_controller)

# После каждой сборки готовая прошивка копируется в firmware/ (хранится в git)
idf_build_get_property(project_ver PROJECT_VER)
add_custom_target(copy_firmware ALL
    COMMAND ${CMAKE_COMMAND} -D BUILD_DIR=${CMAKE_BINARY_DIR} -D FIRMWARE_DIR=${CMAKE_SOURCE_DIR}/firmware
        -D VERSION=${project_ver} -P ${CMAKE_SOURCE_DIR}/firmware/update.cmake
    VERBATIM)
add_dependencies(copy_firmware app bootloader partition_table_bin blank_ota_data)
