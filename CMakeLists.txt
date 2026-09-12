cmake_minimum_required(VERSION 3.16)
project(XMirror LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# One version for the app and the MSI: VERSION holds major.minor.patch and is
# also read by build.ps1 for the installer. A tag-driven release build should
# write the tag's version into VERSION rather than override it here, so the two
# cannot disagree. MSI major upgrades ignore a fourth field, so every release
# must change one of these three.
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" XMIRROR_VERSION LIMIT_COUNT 1)
if(NOT XMIRROR_VERSION MATCHES "^[0-9]+\.[0-9]+\.[0-9]+$")
    message(FATAL_ERROR "XMIRROR_VERSION must be major.minor.patch, got '${XMIRROR_VERSION}'")
endif()
message(STATUS "XMirror version ${XMIRROR_VERSION}")

find_package(Qt6 COMPONENTS Core Widgets Network REQUIRED)

# build xmirror as a static library
add_subdirectory(libxmirror)

# the WIN32 flag here tells Windows this is a GUI app, not a Console app
add_executable(XMirror WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/fluenttheme.cpp
    src/fluentswitch.cpp
    src/updatechecker.cpp
    src/mirrorshape.cpp
    src/airplayworker.cpp
    src/mdns_responder.cpp
    src/resources.rc
)

target_link_libraries(XMirror PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    xmirror
    psapi          # GetProcessMemoryInfo, for the --soak-test harness
    msi            # MsiEnumRelatedProducts, to tell an MSI install from the portable ZIP
)

target_include_directories(XMirror PRIVATE
    ${CMAKE_SOURCE_DIR}/libxmirror
)

target_compile_definitions(XMirror PRIVATE XMIRROR_VERSION="${XMIRROR_VERSION}")

# Standalone test programs, off by default so a normal or packaging build never
# needs what they do: updatechecker_test talks to GitHub, and mirrorshape_test
# opens a real Direct3D 11 video window, so it needs a desktop session.
#   cmake -DXMIRROR_BUILD_TESTS=ON ...   then   ctest --output-on-failure
option(XMIRROR_BUILD_TESTS "Build the standalone test programs" OFF)
if(XMIRROR_BUILD_TESTS)
    enable_testing()

    add_executable(updatechecker_test tests/updatechecker_test.cpp src/updatechecker.cpp)
    target_include_directories(updatechecker_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(updatechecker_test PRIVATE Qt6::Core Qt6::Network msi)
    # The test's version comparisons are written against a fixed version, so
    # they do not change meaning every time VERSION is bumped.
    target_compile_definitions(updatechecker_test PRIVATE XMIRROR_VERSION="2.1.0")
    add_test(NAME updatechecker COMMAND updatechecker_test)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XMIRROR_TEST_GST REQUIRED IMPORTED_TARGET gstreamer-1.0)
    add_executable(mirrorshape_test tests/mirrorshape_test.cpp src/mirrorshape.cpp)
    target_include_directories(mirrorshape_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(mirrorshape_test PRIVATE PkgConfig::XMIRROR_TEST_GST user32 gdi32)
    add_test(NAME mirrorshape COMMAND mirrorshape_test)
endif()
