cmake_minimum_required(VERSION 3.14)
project(vms_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(oatpp 1.4.0 REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0)

add_executable(vms_server src/main.cpp)

target_include_directories(vms_server PRIVATE
    # oatpp 1.4.0 installs headers at this double-nested path
    /usr/local/include/oatpp-1.4.0/oatpp
    ${GST_INCLUDE_DIRS}
    src)

target_link_libraries(vms_server
    PUBLIC
        oatpp::oatpp
        ${GST_LIBRARIES})

# GCC < 9 needs an explicit -lstdc++fs for std::filesystem
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
    CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(vms_server PUBLIC stdc++fs)
endif()

add_custom_command(TARGET vms_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:vms_server>/hls
    COMMENT "Creating HLS output directory")
