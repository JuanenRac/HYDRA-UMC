# =============================================================================
# CMakeLists.txt - hmi_qt6 kiosk shell
# PROJECT: HYDRA-UMC
# AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
# LICENSE: GPL-3.0 - see repo root LICENSE
#
# NOT YET VERIFIED - no Qt6 installed on this development machine. Verify
# `cmake -B build && cmake --build build` on a real Qt6 dev environment
# (target device or a Linux dev box with qt6-base-dev + qt6-webengine-dev)
# before trusting this configures/builds cleanly.
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(hydra_hmi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)

add_executable(hydra_hmi src/main.cpp src/kiosk_view.cpp src/kiosk_view.h)
target_link_libraries(hydra_hmi PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
