cmake_minimum_required(VERSION 3.25)

# set project
project(coco-boot CXX)

# configure languages
set(CMAKE_CXX_STANDARD 23)

# build type
message("*** Type: ${CMAKE_BUILD_TYPE}") #${CMAKE_CONFIGURATION_TYPES}")
add_compile_definitions($<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Release>:NDEBUG>) # assert() is disabled when NDEBUG is defined

# compiler
message("*** Compiler: ${CMAKE_CXX_COMPILER_ID}")
message("*** Crosscompiling: ${CMAKE_CROSSCOMPILING}")

# enable address sanitizer
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")

# debug
#set(CMAKE_CXX_FLAGS_RELEASE "-g -Og")
#set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON" FORCE)

# platform
#message("*** OS: ${OS}")
message("*** Platform: ${PLATFORM}")

# dependencies
find_package(coco CONFIG)
find_package(coco-loop CONFIG)
find_package(coco-device CONFIG)
find_package(coco-devboards CONFIG)
find_package(coco-usb CONFIG)

# source
add_subdirectory(coco)

# test executables
#add_subdirectory(test)
