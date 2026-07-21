cmake_minimum_required(VERSION 3.16)

# 原生 ESP-IDF 固件工程入口；Arduino fallback 已移除。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "1.10")
project(sms_forwarding_idf)
