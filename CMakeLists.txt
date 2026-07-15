cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

string(ASCII 27 Esc)
set(ColourReset "${Esc}[m")
set(Red         "${Esc}[31m")
# module setup options

# set the project name
project(lasambmi VERSION 1.0.0 DESCRIPTION "OWP LASAM BMI Module Shared Library")

IF(CMAKE_BUILD_TYPE MATCHES Debug)
    message("Debug build.")
ENDIF(CMAKE_BUILD_TYPE MATCHES Debug)

message(CMAKE_CXX_COMPILER " ${CMAKE_CXX_COMPILER}")
message(CMAKE_C_COMPILER " ${CMAKE_C_COMPILER}")
message("CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

# A numeric verbosity level can be set for debug output printed/logged during execution.
# It is exposed to the source as the LGAR_DEBUG macro, which uses ">=" logic (a setting of 2
# includes levels 0, 1, and 2). LGAR_DEBUG selects the *default* run-time verbosity
# (none/low/high) applied when the config file does not set a "verbosity" key; the config key
# always overrides it. Default builds thus stay quiet and lighter for ngen runs.
# 0: Nothing (default run-time verbosity "none")
# 1: Model info, e.g. the end-of-run mass balance summary (default verbosity "low")
# 2: BMI info, e.g. per-timestep detail (default verbosity "high")
# Use 1 by default when build type is "Debug"
if (NOT DEFINED DEBUG_VERBOSITY AND "${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    set(DEBUG_VERBOSITY 1)
    # Use 0 by default for any build types other than "Debug"
elseif (NOT DEFINED DEBUG_VERBOSITY)
    set(DEBUG_VERBOSITY 0)
    # Catch any "ON" or equivalent, excluding all integers other than "1" (which we want to handle separately)
elseif (${DEBUG_VERBOSITY} AND NOT ${DEBUG_VERBOSITY} GREATER "1" AND NOT ${DEBUG_VERBOSITY} LESS_EQUAL "0")
    set(DEBUG_VERBOSITY 1)
    # Should catch any other values except an integer greater than 1 (which we want to use directly)
elseif (NOT ${DEBUG_VERBOSITY} GREATER "1")
    set(DEBUG_VERBOSITY 0)
endif ()
# While not an explicit condition, DEBUG_VERBOSITY is left alone if set to some integer greater than 1

add_compile_definitions(LGAR_DEBUG=${DEBUG_VERBOSITY})

# standalone
add_executable(lasam_standalone ./src/bmi_main_lgar.cxx ./src/bmi_lgar.cxx ./src/lgar.cxx ./src/soil_funcs.cxx ./src/conceptual_reservoir.cxx
             ./src/linked_list.cxx ./src/mem_funcs.cxx ./src/util_funcs.cxx ./src/aet.cxx
             ./giuh/giuh.h ./giuh/giuh.c)
target_link_libraries(lasam_standalone PRIVATE m)

# unittest
add_executable(lasam_unitest ./tests/main_unit_test_bmi.cxx ./src/bmi_lgar.cxx ./src/lgar.cxx ./src/soil_funcs.cxx ./src/conceptual_reservoir.cxx
             ./src/linked_list.cxx ./src/mem_funcs.cxx ./src/util_funcs.cxx ./src/aet.cxx ./giuh/giuh.h
             ./giuh/giuh.c)
target_link_libraries(lasam_unitest PRIVATE m)

# Make sure these are compiled with this directive
add_compile_definitions(BMI_ACTIVE)

add_library(lasambmi SHARED src/bmi_lgar.cxx src/lgar.cxx ./src/soil_funcs.cxx ./src/linked_list.cxx ./src/mem_funcs.cxx ./src/conceptual_reservoir.cxx
        ./src/util_funcs.cxx ./src/aet.cxx ./giuh/giuh.c include/all.hxx ./giuh/giuh.h)

target_compile_definitions(lasambmi PRIVATE NGEN)
target_include_directories(lasambmi PRIVATE include)

set_target_properties(lasambmi PROPERTIES VERSION ${PROJECT_VERSION})

set_target_properties(lasambmi PROPERTIES PUBLIC_HEADER ./include/bmi_lgar.hxx)

include(GNUInstallDirs)

install(TARGETS lasambmi
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_file(lasambmi.pc.in lasambmi.pc @ONLY)

install(FILES ${CMAKE_BINARY_DIR}/lasambmi.pc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig)
