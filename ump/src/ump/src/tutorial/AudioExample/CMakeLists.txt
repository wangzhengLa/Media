# Copyright 2019 Proyectos y Sistemas de Mantenimiento SL (eProsima).
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)
project(DDSAudioExample VERSION 1 LANGUAGES CXX)

# Find requirements (顺序:先找底层依赖,再找 fastrtps)
if(NOT tinyxml2_FOUND)
    find_package(tinyxml2 REQUIRED)
endif()

if(NOT fastcdr_FOUND)
    find_package(fastcdr 2 REQUIRED)
endif()

if(NOT foonathan_memory_FOUND)
    find_package(foonathan_memory REQUIRED)
endif()

if(NOT fastrtps_FOUND)
    find_package(fastrtps REQUIRED)
endif()

# Check C++11 (optional, usually inherited from top-level)
include(CheckCXXCompilerFlag)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    check_cxx_compiler_flag(-std=c++11 SUPPORTS_CXX11)
    if(NOT SUPPORTS_CXX11)
        message(FATAL_ERROR "Compiler doesn't support C++11")
    endif()
endif()

message(STATUS "Configuring AudioExample...")

# Collect source files (all .cxx and .cpp in this directory)
file(GLOB AUDIO_EXAMPLE_SOURCES_CXX "*.cxx")
file(GLOB AUDIO_EXAMPLE_SOURCES_CPP "*.cpp")

# Create separate executables for publisher and subscriber
# (We assume the user wants two separate binaries)
add_executable(AudioPublisher
    AudioData.cxx
    AudioDataPubSubTypes.cxx
    AudioPublisher.cpp
)

add_executable(AudioSubscriber
    AudioData.cxx
    AudioDataPubSubTypes.cxx
    AudioSubscriber.cpp
)

# Link dependencies
target_link_libraries(AudioPublisher fastrtps fastcdr)
target_link_libraries(AudioSubscriber fastrtps fastcdr)

# Optionally add compiler definitions (like debug flags)
target_compile_definitions(AudioPublisher PRIVATE
    $<$<AND:$<NOT:$<BOOL:${WIN32}>>,$<STREQUAL:"${CMAKE_BUILD_TYPE}","Debug">>:__DEBUG>
)
target_compile_definitions(AudioSubscriber PRIVATE
    $<$<AND:$<NOT:$<BOOL:${WIN32}>>,$<STREQUAL:"${CMAKE_BUILD_TYPE}","Debug">>:__DEBUG>
)

# Install targets (optional)
install(TARGETS AudioPublisher AudioSubscriber
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/usr/bin
)
