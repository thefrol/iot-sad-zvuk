cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Версия прошивки = версия релиза; CI перебивает её тегом (-DPROJECT_VER)
project(iot-sad-zvuk VERSION 0.3.0)
