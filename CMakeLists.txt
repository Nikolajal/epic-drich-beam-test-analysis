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
    GIT_TAG        v2.4.2
)
FetchContent_MakeAvailable(CLI11)

# toml++
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.3.0
)
FetchContent_MakeAvailable(tomlplusplus)

# MIST — Make It Simple, Toolkit
#
# Tracking the `dev` branch while MIST and beam-test-analysis are being
# co-developed.  CMake fetches the branch tip on first configure and then
# leaves the local clone alone (FETCHCONTENT_UPDATES_DISCONNECTED defaults
# to OFF, but FetchContent only re-fetches when the build dir is wiped or
# the cache is invalidated).  To force a refresh:
#
#     rm -rf build && ./scripts/install.sh
#
# Before any external release, pin to an immutable tag/SHA — e.g.
#     GIT_TAG v0.1.0     # once MIST cuts a tag
# or
#     GIT_TAG <40-char SHA>
# so the build is reproducible for downstream consumers.
FetchContent_Declare(
    mist
    GIT_REPOSITORY https://github.com/Nikolajal/mist
    GIT_TAG        dev
)
FetchContent_MakeAvailable(mist)

# --------------------------------------------------
# ROOT
# --------------------------------------------------
find_package(ROOT REQUIRED COMPONENTS RIO Tree Hist Graf Gui)

# --------------------------------------------------
# Dictionary Headers
# --------------------------------------------------
set(DICT_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/utility/global_index.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/utility/config_reader.h
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
    ${CMAKE_CURRENT_SOURCE_DIR}/include/parallel_streaming_framer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/writers/lightdata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/writers/recodata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/writers/recotrackdata.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/analysis_results.h
    # hough_transform.h is now part of MIST — no longer a local header,
    # so it does not need to be listed here for the ROOT dictionary.
    # If any of the headers above include <mist/ring_finding/hough_transform.h>,
    # the -I path below ensures rootcling can find it.
)

# --------------------------------------------------
# ROOT Dictionary
# --------------------------------------------------
ROOT_GENERATE_DICTIONARY(
    G__beam_test_analysis_dict
    ${DICT_HEADERS}
    LINKDEF ${CMAKE_CURRENT_SOURCE_DIR}/dict/alcor_linkdef.h
    OPTIONS
        -I${CMAKE_CURRENT_SOURCE_DIR}/include
        -I${mist_SOURCE_DIR}/include
        -I${tomlplusplus_SOURCE_DIR}/include
)

# --------------------------------------------------
# Library Sources
# --------------------------------------------------
set(SRC_FILES
    src/config_reader.cxx
    src/alcor_data.cxx
    src/alcor_finedata.cxx
    src/alcor_lightdata.cxx
    src/alcor_recodata.cxx
    src/alcor_spilldata.cxx
    src/alcor_recotrackdata.cxx
    src/mapping.cxx
    src/tracking_altai.cxx
    src/alcor_data_streamer.cxx
    src/parallel_streaming_framer.cxx
    src/triggers/config.cxx
    src/triggers/streaming/score.cxx
    src/triggers/streaming/hough.cxx
    src/lightdata_writer.cxx
    src/writers/lightdata/dcr_afterpulse_ct_qa.cxx
    src/writers/lightdata/finalize_streaming_qa.cxx
    src/recodata_writer.cxx
    src/writers/recodata/radial_fit.cxx
    src/writers/recodata/sigma_vs_n_fit.cxx
    src/writers/recodata/ring_compute.cxx
    src/writers/recodata/frame_pipeline.cxx
    src/recotrackdata_writer.cxx
    src/analysis_results.cxx
    src/radiator_efficiency.cxx
    src/utilities/btana_dump.cxx
    src/writers/pulser_calib_writer.cxx
    src/writers/anchor_dt_canvas.cxx
)

# --------------------------------------------------
# Shared Library
# --------------------------------------------------
add_library(beam_test_analysis SHARED
    ${SRC_FILES}
    G__beam_test_analysis_dict.cxx
)

#  Dev-mode lib_loader: source-tree headers + build-tree library.
#  Used by macros run directly from the project source dir (before any
#  `cmake --install`).  Lands at `macros/lib_loader.h` so the macros'
#  `#include "../lib_loader.h"` resolves to this dev flavour.
#
#  Why dev-mode points at the source tree:  `ROOT_GENERATE_DICTIONARY`
#  embeds absolute paths to the source-tree headers in the dict
#  payload.  If the macro's `#include` search uses any OTHER include
#  path that also contains those headers (e.g. a stale install at
#  `~/.local/include`), ROOT parses the same logical header from two
#  physical paths and triggers a redefinition cascade for every type
#  the dict autoloads.  Pinning the dev loader to the source tree
#  guarantees the macro and the dict resolve to identical file paths.
if(APPLE)
    set(DYNLIB_EXT ".dylib")
elseif(WIN32)
    set(DYNLIB_EXT ".dll")
else()
    set(DYNLIB_EXT ".so")
endif()
set(BEAM_LOADER_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(BEAM_LOADER_LIB     "${CMAKE_CURRENT_BINARY_DIR}/libbeam_test_analysis${DYNLIB_EXT}")
#  Dev-mode also needs the FetchContent'd dependency includes (mist,
#  toml++) — they live under `build/_deps/*/include` and aren't
#  reachable through the source-tree include path.
set(BEAM_LOADER_DEP_INCLUDES
"R__ADD_INCLUDE_PATH(${mist_SOURCE_DIR}/include)
R__ADD_INCLUDE_PATH(${tomlplusplus_SOURCE_DIR}/include)")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lib_loader.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/macros/lib_loader.h"
    @ONLY
)
#  Keep an in-build-tree copy too — some tooling expects `build/lib_loader.h`.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lib_loader.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lib_loader.h"
    @ONLY
)

target_include_directories(beam_test_analysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${tomlplusplus_SOURCE_DIR}/include
    # mist headers are transitively available via mist::mist —
    # no manual -I needed here.
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
        mist::mist
)

#  Install RPATH for the shared library — resolve its own siblings
#  (libmist.so etc.) from the same lib/ dir after install.  Ship BOTH
#  the macOS (@loader_path) and Linux ($ORIGIN) idioms in one list; the
#  loader on each platform ignores the token it doesn't understand.
set_target_properties(beam_test_analysis PROPERTIES
    INSTALL_RPATH "@loader_path;$ORIGIN"
)

# --------------------------------------------------
# Executables
# --------------------------------------------------
add_executable(lightdata_writer      macros/utilities/lightdata_writer.cpp)
add_executable(recodata_writer       macros/utilities/recodata_writer.cpp)
add_executable(recotrackdata_writer  macros/utilities/recotrackdata_writer.cpp)
add_executable(pulser_calib_writer   macros/utilities/pulser_calib_writer.cpp)
add_executable(btana-dump            macros/utilities/btana_dump.cpp)
# Stand-alone TBrowser launcher used by the QA dashboard's Inspect
# button.  See macros/utilities/qa_tbrowser.cpp for the rationale.
add_executable(qa_tbrowser           macros/utilities/qa_tbrowser.cpp)
# Sibling — pops a single histogram on a TCanvas (Open-in-ROOT path).
# See macros/utilities/qa_tcanvas.cpp.
add_executable(qa_tcanvas            macros/utilities/qa_tcanvas.cpp)

target_link_libraries(lightdata_writer      PRIVATE beam_test_analysis)
target_link_libraries(recodata_writer       PRIVATE beam_test_analysis)
target_link_libraries(recotrackdata_writer  PRIVATE beam_test_analysis)
target_link_libraries(pulser_calib_writer   PRIVATE beam_test_analysis)
target_link_libraries(btana-dump            PRIVATE beam_test_analysis)
# qa_tbrowser + qa_tcanvas only need ROOT (no project lib) since
# they're pure TApplication glue.  Link against the umbrella ROOT
# library target FindROOT already exports for the writers.
target_include_directories(qa_tbrowser     PRIVATE ${ROOT_INCLUDE_DIRS})
target_link_libraries(qa_tbrowser           PRIVATE ${ROOT_LIBRARIES})
target_include_directories(qa_tcanvas      PRIVATE ${ROOT_INCLUDE_DIRS})
target_link_libraries(qa_tcanvas            PRIVATE ${ROOT_LIBRARIES})

#  Install RPATH for the writer executables.  They install to bin/ and
#  link libbeam_test_analysis (+ transitively libmist) which install to
#  lib/, so the runtime search path must point one level up into lib/.
#  Without this a fresh `cmake --install` on Linux produced binaries
#  that failed at first run with "libmist.so: cannot open shared object
#  file" unless the operator set LD_LIBRARY_PATH by hand — exactly the
#  "README says it works, it doesn't" failure mode for a new shifter.
#  Both idioms shipped (see the library block above for why).  qa_tbrowser
#  / qa_tcanvas link only ROOT (found via its own RPATH) so they don't
#  need the project lib path, but including them is harmless and uniform.
set_target_properties(
    lightdata_writer
    recodata_writer
    recotrackdata_writer
    pulser_calib_writer
    btana-dump
    PROPERTIES
        INSTALL_RPATH "@loader_path/../lib;$ORIGIN/../lib"
)

# --------------------------------------------------
# Install Rules
# --------------------------------------------------
install(TARGETS
    beam_test_analysis
    lightdata_writer
    recodata_writer
    recotrackdata_writer
    pulser_calib_writer
    btana-dump
    qa_tbrowser
    qa_tcanvas
    EXPORT  beam_test_analysisTargets
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

#  DYNLIB_EXT is already defined above (dev-mode lib_loader block).

#  Install-mode lib_loader: install-prefix headers + install-prefix
#  library.  Used by downstream consumers that find the framework via
#  `find_package` or include against the installed tree.  Generated
#  with DIFFERENT path substitutions than the dev-mode loader above
#  (which lives in `macros/lib_loader.h`).
set(BEAM_LOADER_INCLUDE "${CMAKE_INSTALL_PREFIX}/include")
set(BEAM_LOADER_LIB     "${CMAKE_INSTALL_PREFIX}/lib/libbeam_test_analysis${DYNLIB_EXT}")
#  Install-mode: deps already live under the install prefix (mist is
#  installed there alongside this project; toml++ is header-only and
#  bundled).  Empty string → template substitutes nothing.
set(BEAM_LOADER_DEP_INCLUDES "")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/lib_loader.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/lib_loader.h"
    @ONLY
)

#  Install the install-mode loader into the project's include prefix.
#  Previous design copied this into `macros/` of the SOURCE tree at
#  install time — that's what caused the dev-mode macros to silently
#  pick up the install-prefix paths and clash with the source-tree
#  dict payload.  Headers now stay in their own tree.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/lib_loader.h"
        DESTINATION include)

# --------------------------------------------------
# Tests (off by default so downstream consumers skip them)
#
# BTANA_BUILD_TESTS is a CMake boolean option that defaults to OFF.  Enable
# it explicitly during development:
#     cmake -B build -DBTANA_BUILD_TESTS=ON
#
# Mirrors MIST's MIST_BUILD_TESTS pattern.  enable_testing() runs
# unconditionally so `ctest` works when the option is toggled from the
# command line without re-running the full configure step.
# --------------------------------------------------
option(BTANA_BUILD_TESTS "Build beam-test-analysis test executables" OFF)
enable_testing()

if(BTANA_BUILD_TESTS)
    # Helper: build a test_<name> binary from test/tester_<name>.cxx and
    # register it with CTest.  Avoids per-test boilerplate.
    function(btana_add_test name)
        add_executable(test_${name} test/tester_${name}.cxx)
        target_link_libraries(test_${name} PRIVATE beam_test_analysis)
        target_compile_features(test_${name} PRIVATE cxx_std_17)
        add_test(NAME test_${name} COMMAND test_${name})
    endfunction()

    btana_add_test(global_index)

    message(STATUS "[beam_test_analysis] Tests enabled — binaries will land in ${CMAKE_BINARY_DIR}/bin")
endif()