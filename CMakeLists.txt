cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# esp-hub75 0.3.x trips this warning on IDF 6.1 (new field in gdma_channel_alloc_config_t)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

project(led-proj)
