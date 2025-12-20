# Copyright (c) 2020 ARM Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Pull in FreeRTOS coreSNTP submodule
mbed_setup_submodule(coreSNTP CHECK_FILE coreSntpFilePaths.cmake)

# Build coreSNTP as its own library
include(${CMAKE_CURRENT_SOURCE_DIR}/coreSNTP/coreSntpFilePaths.cmake)
add_library(freertos-coreSNTP STATIC ${CORE_SNTP_SOURCES})
target_include_directories(freertos-coreSNTP PUBLIC ${CORE_SNTP_INCLUDE_PUBLIC_DIRS})


add_library(mbed-ntp-client STATIC NTPClient.cpp)

target_include_directories(mbed-ntp-client
    PUBLIC
        .
)

target_link_libraries(mbed-ntp-client
    PUBLIC
        freertos-coreSNTP
        mbed-netsocket
)

# Pull in the config header we provide for coreSNTP
target_link_libraries(freertos-coreSNTP PUBLIC mbed-ntp-client)
