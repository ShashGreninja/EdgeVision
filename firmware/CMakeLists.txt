cmake_minimum_required(VERSION 3.20)
project(edgevision_firmware C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

# ---- FreeRTOS kernel (Windows simulator port) --------------------------------
# The kernel's CMake expects a `freertos_config` target that points at our
# FreeRTOSConfig.h.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/config)

set(FREERTOS_PORT "MSVC_MINGW" CACHE STRING "" FORCE)
set(FREERTOS_HEAP "4" CACHE STRING "" FORCE)

FetchContent_Declare(freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.3.1  # first release with vPortGenerateSimulatedInterruptFromWindowsThread()
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(freertos_kernel)

# ---- OpenCV (used only by the virtual hardware, never by firmware tasks) ------
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)

# ---- Firmware -----------------------------------------------------------------
add_executable(edgevision
    main.c
    common/log.c
    hal/hal_sensor.cpp
    hal/hal_input.c
    drivers/frame_pool.c
    drivers/camera_driver.c
    pipeline/motion_task.c
    services/telemetry.c
)

target_include_directories(edgevision PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(edgevision PRIVATE -Wall -Wextra)
target_link_libraries(edgevision PRIVATE freertos_kernel freertos_config ${OpenCV_LIBS} winmm ws2_32)
