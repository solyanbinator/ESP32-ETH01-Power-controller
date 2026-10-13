cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Собирать только main и его зависимости: Wi-Fi, Bluetooth и прочие
# неиспользуемые компоненты IDF не компилируются и не попадают в прошивку.
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32_eth01_controller)
