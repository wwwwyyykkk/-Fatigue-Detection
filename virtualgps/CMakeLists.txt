# 虚拟GPS模拟板 - 顶层 CMake 配置
cmake_minimum_required(VERSION 3.16)

# 设置项目名称
set(PROJECT_NAME "virtual_gps_board")

# 包含 ESP-IDF 构建系统
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 定义项目
project(${PROJECT_NAME})
