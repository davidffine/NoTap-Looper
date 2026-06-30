cmake_minimum_required(VERSION 3.22.1)
project("looper_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(oboe REQUIRED CONFIG)
find_package(aubio REQUIRED)

add_library(
    looper_core
    SHARED
    src/LooperEngine.cpp
    src/OboeBridge.cpp
    src/JNIBridge.cpp
    src/miniaudio_impl.cpp
)

target_include_directories(looper_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
        looper_core
        log
        android
        oboe::oboe
        aubio
)

target_link_options(looper_core PRIVATE "-Wl,-z,max-page-size=16384")