# Копирует свежую сборку в firmware/ (эти файлы хранятся в git). Запускается после каждой
# сборки из CMakeLists.txt: cmake -D BUILD_DIR=... -D FIRMWARE_DIR=... -D VERSION=... -P update.cmake
foreach(file
        esp32_eth01_controller.bin
        bootloader/bootloader.bin
        partition_table/partition-table.bin
        ota_data_initial.bin)
    get_filename_component(name "${file}" NAME)
    # Файл перезаписывается, только если изменился
    configure_file("${BUILD_DIR}/${file}" "${FIRMWARE_DIR}/${name}" COPYONLY)
endforeach()

file(WRITE "${FIRMWARE_DIR}/VERSION.tmp" "${VERSION}\n")
configure_file("${FIRMWARE_DIR}/VERSION.tmp" "${FIRMWARE_DIR}/VERSION" COPYONLY)
file(REMOVE "${FIRMWARE_DIR}/VERSION.tmp")
message(STATUS "Firmware ${VERSION} copied to firmware/")
