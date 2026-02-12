cmake_minimum_required(VERSION 3.16)
project(beam_test_analysis LANGUAGES CXX)

# ----------------------------
# Set default install prefix (user-local) if not specified
# ----------------------------
if(NOT CMAKE_INSTALL_PREFIX)
    if(WIN32)
        set(CMAKE_INSTALL_PREFIX "$ENV{USERPROFILE}/.local" CACHE PATH "Default install prefix" FORCE)
    else()
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Default install prefix" FORCE)
    endif()
endif()

# Place executables in build/bin during build
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --------------------------------------------------
# C++ Standard
# --------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: auto-export symbols for shared library
if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# --------------------------------------------------
# Dependencies (FetchContent)
# --------------------------------------------------
include(FetchContent)

# CLI11
FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(CLI11)

# toml++
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.3.0
)
FetchContent_MakeAvailable(tomlplusplus)

#   --- cppinfra
FetchContent_Declare(
    cppinfra
    GIT_REPOSITORY https://github.com/Nikolajal/ccpinfra.git
    GIT_TAG main
)
FetchContent_MakeAvailable(cppinfra)

# --------------------------------------------------
# ROOT
# --------------------------------------------------
find_package(ROOT REQUIRED COMPONENTS RIO Tree Hist Graf Gui)

# --------------------------------------------------
# Dictionary Headers
# --------------------------------------------------
set(DICT_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/config_reader.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/triggers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_data.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_finedata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_lightdata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_recodata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_spilldata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_recotrackdata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mapping.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/tracking_altai.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_data_streamer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/streaming_framer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/parallel_streaming_framer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lightdata_writer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/recodata_writer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/recotrackdata_writer.h
)

# --------------------------------------------------
# ROOT Dictionary
# --------------------------------------------------
ROOT_GENERATE_DICTIONARY(
    G__beam_test_analysis_dict
    ${DICT_HEADERS}
    LINKDEF ${CMAKE_CURRENT_SOURCE_DIR}/dict/alcor_linkdef.h
    OPTIONS -I${CMAKE_CURRENT_SOURCE_DIR}/include
        OPTIONS
        -I${CMAKE_CURRENT_SOURCE_DIR}/include
        -I${cppinfra_SOURCE_DIR}/include
        -I${tomlplusplus_SOURCE_DIR}/include
)

# --------------------------------------------------
# Library Sources
# --------------------------------------------------
set(SRC_FILES
    src/config_reader.cxx
    src/triggers.cxx
    src/alcor_data.cxx
    src/alcor_finedata.cxx
    src/alcor_lightdata.cxx
    src/alcor_recodata.cxx
    src/alcor_spilldata.cxx
    src/alcor_recotrackdata.cxx
    src/mapping.cxx
    src/tracking_altai.cxx
    src/alcor_data_streamer.cxx
    src/streaming_framer.cxx
    src/parallel_streaming_framer.cxx
    src/lightdata_writer.cxx
    src/recodata_writer.cxx
    src/recotrackdata_writer.cxx
)

# --------------------------------------------------
# Shared Library
# --------------------------------------------------
add_library(beam_test_analysis SHARED
    ${SRC_FILES}
    G__beam_test_analysis_dict.cxx
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lib_loader.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lib_loader.h"
    @ONLY
)

target_include_directories(beam_test_analysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${cppinfra_SOURCE_DIR}/include
        ${tomlplusplus_SOURCE_DIR}/include
)

target_link_libraries(beam_test_analysis
    PUBLIC
        ROOT::RIO
        ROOT::Tree
        ROOT::Hist
        ROOT::Graf
        ROOT::Gui
        CLI11::CLI11
        tomlplusplus::tomlplusplus
)

if(APPLE)
    set_target_properties(beam_test_analysis PROPERTIES
        INSTALL_RPATH "@loader_path"
    )
endif()

# --------------------------------------------------
# Executables
# --------------------------------------------------
add_executable(lightdata_writer macros/utilities/lightdata_writer.cpp)
add_executable(recodata_writer macros/utilities/recodata_writer.cpp)
add_executable(recotrackdata_writer macros/utilities/recotrackdata_writer.cpp)

target_link_libraries(lightdata_writer PRIVATE beam_test_analysis)
target_link_libraries(recodata_writer PRIVATE beam_test_analysis)
target_link_libraries(recotrackdata_writer PRIVATE beam_test_analysis)

# --------------------------------------------------
# Install Rules
# --------------------------------------------------
install(TARGETS
    beam_test_analysis
    lightdata_writer
    recodata_writer
    recotrackdata_writer
    EXPORT beam_test_analysisTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

# Install ROOT dictionary artifacts
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libbeam_test_analysis_dict_rdict.pcm
    DESTINATION lib
    OPTIONAL
)

# Determine dynamic library extension
if(APPLE)
    set(DYNLIB_EXT ".dylib")
elseif(WIN32)
    set(DYNLIB_EXT ".dll")
else()
    set(DYNLIB_EXT ".so")
endif()

# Paths
set(BEAM_DICT_LIB "${CMAKE_INSTALL_PREFIX}/lib/libbeam_test_analysis_dict${DYNLIB_EXT}")
set(BEAM_MAIN_LIB "${CMAKE_INSTALL_PREFIX}/lib/libbeam_test_analysis${DYNLIB_EXT}")

# Configure the header
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lib_loader.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/lib_loader.h"
    @ONLY
)

# Install so macros can include it
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/lib_loader.h"
        DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/macros/")
