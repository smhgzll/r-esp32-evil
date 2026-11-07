# r-esp32-evil firmware (ESP-IDF 5.5).
#
#   idf.py set-target esp32s3            (once)
#   idf.py build flash monitor           (board)
#   python3 ../tools/re1tool.py qemu     (QEMU profile, see docs/GUIDE.md)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(r_esp32_evil)
