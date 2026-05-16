# 以下 5 行是 ESP-IDF 工程的固定模板。
# 为保证 CMake 正常工作，顺序不要调整。
cmake_minimum_required(VERSION 3.16)
set(PARTITION_TABLE_FILENAME "partitions.csv")


set(EXTRA_COMPONENT_DIRS components/Middlewares)
add_compile_options(-fdiagnostics-color=always)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_world)
