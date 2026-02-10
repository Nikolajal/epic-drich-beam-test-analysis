cmake_minimum_required(VERSION 3.10)
project(beam_test_analysis LANGUAGES CXX)

# ----------------------------
# C++ standard
# ----------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------
# RPATH settings (macOS + ROOT)
# ----------------------------
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_INSTALL_RPATH "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -g") // only for debug, breaks ROOT loading the libraries


# ----------------------------
# Fetch external dependencies
# ----------------------------
include(FetchContent)

#   --- CLI
FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
)
FetchContent_MakeAvailable(CLI11)

#   --- TOML++
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.3.0  # or latest stable
)
FetchContent_MakeAvailable(tomlplusplus)

# ----------------------------
# Find ROOT
# ----------------------------
find_package(ROOT REQUIRED COMPONENTS RIO Tree Hist)
include(${ROOT_USE_FILE})

# ----------------------------
# Include directories
# ----------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include          # Main headers
    ${tomlplusplus_SOURCE_DIR}/include           # toml++ headers
    # ${CMAKE_CURRENT_SOURCE_DIR}/external/utility-cpp/include  # Submodule if used
)

# ----------------------------
# Add shared library
# ----------------------------
add_library(test_beam_analysis SHARED
    src/config_reader.cxx
    src/triggers.cxx
    src/alcor_data.cxx
    src/alcor_finedata.cxx
    src/alcor_lightdata.cxx
    src/alcor_recodata.cxx
    src/alcor_spilldata.cxx
    src/mapping.cxx
    src/alcor_data_streamer.cxx
    src/streaming_framer.cxx
    src/parallel_streaming_framer.cxx
    src/lightdata_writer.cxx
    src/recodata_writer.cxx
)

# Link ROOT to the library
target_link_libraries(test_beam_analysis
    PUBLIC ${ROOT_LIBRARIES}
    PUBLIC CLI11::CLI11
)

# Set library output directory
set_target_properties(test_beam_analysis PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    BUILD_RPATH ${CMAKE_BINARY_DIR}/lib
    INSTALL_RPATH ${CMAKE_INSTALL_PREFIX}/lib
)

# ----------------------------
# Generate ROOT dictionary
# ----------------------------
set(DICT_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/config_reader.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/triggers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_data.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_finedata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_lightdata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_recodata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_spilldata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/mapping.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_data_streamer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/streaming_framer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/parallel_streaming_framer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lightdata_writer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/recodata_writer.h
)

set(DICT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

ROOT_GENERATE_DICTIONARY(
    G__test_beam_analysis_dict
    ${DICT_HEADERS}
    LINKDEF ${CMAKE_CURRENT_SOURCE_DIR}/dict/alcor_linkdef.h
)

set_target_properties(G__test_beam_analysis_dict PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    BUILD_RPATH ${CMAKE_BINARY_DIR}/lib
    INSTALL_RPATH ${CMAKE_INSTALL_PREFIX}/lib
)

# Create dictionary library
add_library(test_beam_analysis_dict SHARED G__test_beam_analysis_dict.cxx)
target_link_libraries(test_beam_analysis_dict PUBLIC ${ROOT_LIBRARIES} test_beam_analysis)

# ----------------------------
# Add executable
# ----------------------------
add_executable(lightdata_writer macros/utilities/lightdata_writer.cpp)
add_executable(recodata_writer macros/utilities/recodata_writer.cpp)

# Link the shared library and dictionary to the executable
target_link_libraries(lightdata_writer
    PRIVATE test_beam_analysis
    PRIVATE test_beam_analysis_dict
    PRIVATE ${ROOT_LIBRARIES}
)
target_link_libraries(recodata_writer
    PRIVATE test_beam_analysis
    PRIVATE test_beam_analysis_dict
    PRIVATE ${ROOT_LIBRARIES}
)

# Set executable output folder
set_target_properties(lightdata_writer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
set_target_properties(recodata_writer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ----------------------------
# Optional: install rules
# ----------------------------
install(TARGETS test_beam_analysis test_beam_analysis_dict lightdata_writer recodata_writer
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

# Install the PCM explicitly
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libtest_beam_analysis_dict_rdict.pcm
    DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include)
