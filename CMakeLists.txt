cmake_minimum_required(VERSION 3.20)

project(alx-home_webview LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/webview.cmake")

win32_library(TARGET_NAME alx-home_webview 
    FILES 
        src/engine_base.cpp
        src/backends/win32_edge.cpp
        src/user_script.cpp
)
add_library(alx-home::webview ALIAS alx-home_webview)

target_include_directories(alx-home_webview INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(alx-home_webview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/webview")

target_link_libraries(alx-home_webview PUBLIC ${WEBVIEW_DEPENDENCIES})
target_link_libraries(alx-home_webview PRIVATE Dwmapi alx-home::cpp_utils alx-home::json alx-home::promise)
