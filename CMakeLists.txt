# ESP-IDF 工程顶层 CMake 文件
#
# 【学弟必读】
# - ESP-IDF 使用 CMake 作为构建系统
# - idf_component.yml 声明第三方依赖（如 LVGL、RainMaker）
# - components/ 下是本项目的自有代码（BSP + Middlewares）
# - managed_components/ 是 idf.py 自动拉取的第三方库，不要手动修改
#

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
