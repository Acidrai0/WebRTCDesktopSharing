cmake_minimum_required(VERSION 3.15)
project(desktop_sharing_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build static libraries
set(BUILD_SHARED_LIBS OFF)

# Find required packages
find_package(Threads REQUIRED)

# Set paths for third-party libraries
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(THIRD_PARTY_INCLUDE_DIR ${THIRD_PARTY_DIR}/include)
set(THIRD_PARTY_LIB_DIR ${THIRD_PARTY_DIR}/lib)

# x264 library settings
set(X264_INCLUDE_DIR ${THIRD_PARTY_DIR}/x264/build/include)
set(X264_LIBRARY_PATH ${THIRD_PARTY_DIR}/x264/build/lib/libx264.a)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${THIRD_PARTY_INCLUDE_DIR}
    ${X264_INCLUDE_DIR}
    # Windows SDK DirectXMath include directory - needed for MinGW
    "C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/um"
    "C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/shared"
)

# Define source files
set(SOURCES
    src/main.cpp
    src/screen_capture.cpp
    src/encoder.cpp
    src/webrtc_session.cpp
    src/preview_window.cpp
)

# Define the executable
add_executable(desktop_sharing_client ${SOURCES})

# Link libraries
target_link_libraries(desktop_sharing_client PRIVATE
    Threads::Threads
    dxgi
    d3d11
    d3dcompiler
    ${X264_LIBRARY_PATH}
)

# Compiler flags
if(MINGW)
    target_compile_options(desktop_sharing_client PRIVATE
        -Wall
        -Wextra
        -static-libgcc
        -static-libstdc++
    )
    
    # Static linking for MinGW
    set_target_properties(desktop_sharing_client PROPERTIES
        LINK_FLAGS "-static"
    )
endif()

# Create directories for third-party libraries
file(MAKE_DIRECTORY ${THIRD_PARTY_DIR})
file(MAKE_DIRECTORY ${THIRD_PARTY_INCLUDE_DIR})
file(MAKE_DIRECTORY ${THIRD_PARTY_LIB_DIR})

# Output executable to bin directory
set_target_properties(desktop_sharing_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

# Create bin directory if it doesn't exist
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Check if x264 library exists, if not print instructions
if(NOT EXISTS ${X264_LIBRARY_PATH})
    message(WARNING "x264 library not found. Please place libx264.a in ${THIRD_PARTY_LIB_DIR} and x264.h in ${THIRD_PARTY_INCLUDE_DIR}")
    message(STATUS "You can install x264 using MSYS2 with: pacman -S mingw-w64-x86_64-x264")
    message(STATUS "Then copy the files to the appropriate directories in third_party/")
endif() 