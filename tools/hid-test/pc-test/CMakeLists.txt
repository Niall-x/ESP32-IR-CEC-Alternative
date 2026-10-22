cmake_minimum_required(VERSION 3.16)
project(hid-test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# hidapi-hidraw uses the Linux hidraw kernel interface directly. The imported
# target carries the library path, which some distros need.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED IMPORTED_TARGET hidapi-hidraw)

add_executable(hid-test src/main.cpp)
target_link_libraries(hid-test PRIVATE PkgConfig::HIDAPI)
