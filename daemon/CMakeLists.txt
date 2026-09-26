cmake_minimum_required(VERSION 3.16)

# One version for the whole device, kept in VERSION at the repository root.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" DAEMON_VERSION LIMIT_COUNT 1)

project(esp32-ir-remote-daemon VERSION ${DAEMON_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimised build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The tree builds warning-free. Not -Werror, so a newer compiler cannot stop a
# user building a release.
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Sources compiled on all platforms
set(SOURCES
    src/main.cpp
    src/HIDTransport.cpp
)

# Platform-specific sources and dependencies.
if(UNIX AND NOT APPLE)
    # Linux: power events from systemd-logind over D-Bus. sdbus-c++ 2.0 is
    # required; naming it makes a 1.x install fail here rather than later.
    list(APPEND SOURCES src/LinuxPowerMonitor.cpp)
    find_package(sdbus-c++ 2.0 REQUIRED)

    # libudev tells the daemon when the stick arrives, for the boot ON.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
elseif(WIN32)
    # Windows: power events via the Win32 Service API
    list(APPEND SOURCES src/WindowsPowerMonitor.cpp)
endif()

# hidapi talks to the stick on both platforms.
if(WIN32)
    # hidapi comes from vcpkg on Windows.
    find_package(hidapi CONFIG REQUIRED)

    # vcpkg puts the headers in include/hidapi/; the source includes
    # "hidapi/hidapi.h", so add the parent directory.
    get_target_property(_hidapi_inc hidapi::include INTERFACE_INCLUDE_DIRECTORIES)
    cmake_path(GET _hidapi_inc PARENT_PATH HIDAPI_INCLUDE_DIRS)
else()
    # The imported target carries the library path, which some distros need.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIDAPI REQUIRED IMPORTED_TARGET hidapi-hidraw)
endif()

add_executable(esp32-ir-daemon ${SOURCES})
target_include_directories(esp32-ir-daemon PRIVATE src ${HIDAPI_INCLUDE_DIRS})

if(WIN32)
    # Target version named explicitly, so an SDK upgrade cannot change what
    # compiles.
    target_link_libraries(esp32-ir-daemon PRIVATE hidapi::winapi)
    target_compile_definitions(esp32-ir-daemon PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
else()
    target_link_libraries(esp32-ir-daemon PRIVATE PkgConfig::HIDAPI)
endif()

# The Windows monitor uses a worker thread.
find_package(Threads REQUIRED)
target_link_libraries(esp32-ir-daemon PRIVATE Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(esp32-ir-daemon PRIVATE SDBusCpp::sdbus-c++ PkgConfig::UDEV)
endif()

# -----------------------------------------------------------------------------
# Install
#
# Linux only. Windows is installed by packaging/windows/install-service.ps1.
# -----------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)

    # Fills in the unit file's ExecStart path for the prefix in use.
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/esp32-ir-remote.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/esp32-ir-remote.service
        @ONLY
    )

    install(TARGETS esp32-ir-daemon
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Literal `lib`, never lib64: udev and systemd only read lib. Relative, so
    # CPack can rebase them when staging a package. The cost is that a hand
    # install must pass -DCMAKE_INSTALL_PREFIX=/usr.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/99-esp32-ir-remote.rules
            DESTINATION lib/udev/rules.d)

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/esp32-ir-remote.service
            DESTINATION lib/systemd/system)
endif()

# -----------------------------------------------------------------------------
# Packaging (CPack)
#
#   cpack -G DEB --config daemon/build/CPackConfig.cmake
#   cpack -G RPM --config daemon/build/CPackConfig.cmake
#
# Built in CI on a v* tag, for the GitHub releases page. Arch uses
# packaging/PKGBUILD instead.
# -----------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    set(CPACK_PACKAGE_NAME "esp32-ir-remote")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Mirrors PC power state to a TV over IR, as a CEC alternative")
    set(CPACK_PACKAGE_HOMEPAGE_URL
        "https://github.com/Niall-xox/ESP32-IR-CEC-Alternative")
    set(CPACK_PACKAGE_CONTACT "Niall <niall.fatah2003@gmail.com>")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_STRIP_FILES ON)

    # Both require sdbus-c++ 2.x, so a 1.x system fails at install time.
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsdbus-c++2 (>= 2.0), libhidapi-hidraw0, libudev1")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/prerm")

    set(CPACK_RPM_PACKAGE_LICENSE "GPLv3+")
    set(CPACK_RPM_PACKAGE_REQUIRES "sdbus-cpp >= 2.0, hidapi, systemd-libs")
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/prerm")

    # These directories belong to systemd; claiming them is an rpm conflict.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/lib/systemd;/usr/lib/systemd/system;/usr/lib/udev;/usr/lib/udev/rules.d")

    include(CPack)
endif()
