# ESP-IDF 工程顶层 CMake 文件

# 以下 5 行是 ESP-IDF 工程的固定模板，顺序不要调整
cmake_minimum_required(VERSION 3.16)
set(PARTITION_TABLE_FILENAME "partitions.csv")

# 把 components/Middlewares 加到额外组件搜索路径（默认只会搜 components/）
set(EXTRA_COMPONENT_DIRS components/Middlewares)

# 开启编译器诊断颜色（看编译报错时方便区分）
add_compile_options(-fdiagnostics-color=always)

# 引入 ESP-IDF 的项目 CMake 模块
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 项目名（会用来命名最终固件文件）
project(hello_world)
