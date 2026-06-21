cmake_minimum_required(VERSION 3.20)
project(V_surface CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Fetch GLFW -------------------------------------------------------
include(FetchContent)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ---- Fetch Dear ImGui -------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6)
FetchContent_Populate(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp)

target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_link_libraries(imgui_lib PUBLIC glfw)

# ---- Main executable --------------------------------------------------
add_executable(V_surface V_surface.cpp)
target_link_libraries(V_surface PRIVATE imgui_lib glfw)

if(WIN32)
    target_link_libraries(V_surface PRIVATE opengl32 gdi32 user32 shell32)
elseif(APPLE)
    find_library(OPENGL_LIB OpenGL REQUIRED)
    target_link_libraries(V_surface PRIVATE ${OPENGL_LIB})
else()
    target_link_libraries(V_surface PRIVATE GL)
endif()

# Release optimisations
if(MSVC)
    target_compile_options(V_surface PRIVATE /O2 /fp:fast)
else()
    target_compile_options(V_surface PRIVATE -O3 -march=native -ffast-math)
endif()
