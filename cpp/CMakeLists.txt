cmake_minimum_required(VERSION 3.22.1)
project("looper_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(oboe REQUIRED CONFIG)

# הוסף את קבצי המקור של KissFFT (לאחר שתוריד אותם לתיקיית src)
add_library(
        looper_core
        SHARED
        src/LooperEngine.cpp
        src/OboeBridge.cpp
        src/JNIBridge.cpp
        src/miniaudio_impl.cpp
        includes/kissfft/kiss_fft.c     # תוקן הנתיב
        includes/kissfft/kiss_fftr.c    # תוקן הנתיב
)

target_include_directories(looper_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/headers
        ${CMAKE_CURRENT_SOURCE_DIR}/includes/kissfft # הוספת נתיב ה-Header
)

target_link_libraries(
        looper_core
        log
        android
        oboe::oboe
)