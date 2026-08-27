cmake_minimum_required(VERSION 3.16)
project(XMirror LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

# build xmirror as a static library
add_subdirectory(libxmirror)

# the WIN32 flag here tells Windows this is a GUI app, not a Console app
add_executable(XMirror WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/airplayworker.cpp
    src/mdns_responder.cpp
    src/resources.rc
)

target_link_libraries(XMirror PRIVATE
    Qt6::Core
    Qt6::Widgets
    xmirror
    psapi          # GetProcessMemoryInfo, for the --soak-test harness
)

target_include_directories(XMirror PRIVATE
    ${CMAKE_SOURCE_DIR}/libxmirror
)
