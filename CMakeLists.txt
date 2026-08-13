cmake_minimum_required(VERSION 3.15)
project(SequencerApp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Tell CMake where to look for Homebrew packages on Apple Silicon Mac
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")

# -----------------------------------------------------------------
# 1. Cross-Platform Raylib Solver (System-first, Fallback to Fetch)
# -----------------------------------------------------------------
# First, try to find Raylib on the system (e.g., via Homebrew on Mac)
find_package(raylib QUIET)

if(NOT raylib_FOUND)
    message(STATUS "System Raylib not found. Falling back to downloading from GitHub...")
    include(FetchContent)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        5.0
    )
    # Optimize fallback build by disabling examples and tests
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    
set(GRAPHICS "GRAPHICS_API_OPENGL_21" CACHE STRING "" FORCE)

    FetchContent_MakeAvailable(raylib)
endif()

# -----------------------------------------------------------------
# 2. Declare the Executable
# -----------------------------------------------------------------
add_executable(seq1
    src/main.cpp
    src/Globals.cpp
    src/Audio_Engine.cpp
    src/Sampler_Engine.cpp
    src/Master_FX.cpp
    src/UI_Draw.cpp
    src/UI_Font.cpp
    src/UI_Screens.cpp
    src/Tape_Buffer_FX.cpp 
    src/RtMidi.cpp
    src/Midi_Manager.cpp
    src/OledDriver.cpp
)

# -----------------------------------------------------------------
# 3. Include Directories
# -----------------------------------------------------------------
# Ensures compiler resolves headers correctly, including the root for miniaudio.h
target_include_directories(seq1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src)

# -----------------------------------------------------------------
# 4. Platform-Specific Audio/MIDI Driver Configurations
# -----------------------------------------------------------------
if(APPLE)
    # macOS uses CoreMIDI and CoreAudio
    target_compile_definitions(seq1 PRIVATE __MACOSX_CORE__)
    target_link_libraries(seq1 PRIVATE 
        "-framework CoreMIDI" 
        "-framework CoreAudio" 
        "-framework CoreFoundation"
    )
elseif(UNIX AND NOT APPLE)
    # Linux (Raspberry Pi) uses ALSA
    target_compile_definitions(seq1 PRIVATE __LINUX_ALSA__)
    target_link_libraries(seq1 PRIVATE asound pthread)
    
    # --- ADD THIS LINE (Enables NEON vectorization and aggressive ARM math optimization) ---
    target_compile_options(seq1 PRIVATE -O3 -ffast-math -march=native)
endif()

# -----------------------------------------------------------------
# 5. Link Raylib to our executable
# -----------------------------------------------------------------
target_link_libraries(seq1 PRIVATE raylib)