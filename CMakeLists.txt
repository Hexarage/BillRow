cmake_minimum_required(VERSION 3.16)
project(CppStuff
    VERSION 0.0.1
    LANGUAGES CXX
    DESCRIPTION "A C++ toy project with automatic file discovery in cmake"
)

#Global settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#Build type default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

#Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)


#Options
option(BUILD_TESTS "Build Unit Tests" OFF)
option(BUILD_SHARED_LIBS "Build Shared Libraries" OFF)
option(ENABLE_WARNINGS "Enable Compiler Warnings" ON)

#Compiler Warnings
if(ENABLE_WARNINGS)
    if(MSVC)
        add_compile_options(/W4 /WX-)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic)
    endif()
endif()

#Recursively find all cpp and hpp files in a directory
function(auto_discover_sources target_dir output_var)
    file(GLOB_RECURSE found_files
        "${target_dir}/*.cpp"
        "${target_dir}/*.cxx"
        "${target_dir}/*.cc"
        "${target_dir}/*.c" #Mmaybe shouldn't add these, but oh well
        )
        set(${output_var} ${found_files} PARENT_SCOPE)
endfunction()

function(auto_discover_headers target_dir output_var)
    file(GLOB_RECURSE found_files
        "${target_dir}/*.h"
        "${target_dir}/*.hpp"
        "${target_dir}/*.inl"
        )
    set(${output_var} ${found_files} PARENT_SCOPE)
endfunction()

#If not lazy (or official/production/large project) should in stead explicitly add files 
#helps CMake not miss files until reconfiguration
#Use:
#
#set(SOURCES
#    src/main.cpp
#    src/core/something.cpp
#    src/utils/other.cpp
#)

#Subdirectories
add_subdirectory(include)
add_subdirectory(src)
add_subdirectory(tests)