# Windows GUI client (Dear ImGui + Win32 + DirectX 11).
# Build on Windows with Visual Studio 2022:
#   cmake -B build -S client
#   cmake --build build --config Release
# Dear ImGui is fetched automatically; the result is a single log_client.exe
# with no runtime dependencies beyond d3d11.dll (ships with Windows).

cmake_minimum_required(VERSION 3.16)
project(log_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8)
FetchContent_MakeAvailable(imgui)

add_executable(log_client WIN32
    main.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_include_directories(log_client PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/..)      # for common/Protocol.h

target_compile_definitions(log_client PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(log_client PRIVATE /utf-8)
endif()
target_link_libraries(log_client PRIVATE d3d11 dxgi ws2_32 comdlg32)

# Static CRT so the exe runs on machines without the VS redistributable.
set_property(TARGET log_client PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
