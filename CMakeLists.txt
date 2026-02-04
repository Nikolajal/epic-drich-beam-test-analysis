cmake_minimum_required(VERSION 3.10)
project(beam_test_analysis LANGUAGES CXX)

# ----------------------------
# Fetch external dependencies
# ----------------------------
include(FetchContent)

FetchContent_Declare(
  CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
)

FetchContent_MakeAvailable(CLI11)

# ----------------------------
# C++ standard
# ----------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    src/alcor_data_streamer.cxx
    src/streaming_framer.cxx
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
)

# ----------------------------
# Generate ROOT dictionary
# ----------------------------
set(DICT_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_lightdata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/alcor_finedata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/triggers.h
)

set(DICT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

ROOT_GENERATE_DICTIONARY(
    G__mydict
    ${DICT_HEADERS}
    LINKDEF ${CMAKE_CURRENT_SOURCE_DIR}/dict/alcor_linkdef.h
)

# Create dictionary library
add_library(mydict SHARED G__mydict.cxx)
target_link_libraries(mydict PUBLIC ${ROOT_LIBRARIES} test_beam_analysis)

# ----------------------------
# Add executable
# ----------------------------
add_executable(lightdata_writer macros/lightdata_writer.C)
add_executable(recodata_writer macros/recodata_writer.C)

# Link the shared library and dictionary to the executable
target_link_libraries(lightdata_writer
    PRIVATE test_beam_analysis
    PRIVATE mydict
    PRIVATE ${ROOT_LIBRARIES}
)
target_link_libraries(recodata_writer
    PRIVATE test_beam_analysis
    PRIVATE mydict
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
install(TARGETS test_beam_analysis mydict lightdata_writer recodata_writer
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include)
