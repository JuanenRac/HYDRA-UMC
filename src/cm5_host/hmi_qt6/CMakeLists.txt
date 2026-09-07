# =============================================================================
# CMakeLists.txt - hmi_qt6 kiosk shell
# PROJECT: HYDRA-UMC
# AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
# LICENSE: GPL-3.0 - see repo root LICENSE
#
# Verified: `cmake -B build -G "Visual Studio 16 2019" -A x64
# -DCMAKE_PREFIX_PATH=<qt6-install>/6.7.3/msvc2019_64` then
# `cmake --build build --config Release` produces a real hydra_hmi.exe,
# unmodified, against real Qt 6.7.3 (win64_msvc2019_64). Still not yet
# verified against a Linux/CM5 toolchain (qt6-base-dev +
# qt6-webengine-dev) - real, separate future work.
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(hydra_hmi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)

add_executable(hydra_hmi src/main.cpp src/kiosk_view.cpp src/kiosk_view.h)
target_link_libraries(hydra_hmi PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
