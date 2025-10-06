cmake_minimum_required(VERSION 3.10)

# Set some basic project attributes
project (fronius-ng
	VERSION 0.1
	DESCRIPTION "A Modbus Master talking to a Fronius inverter"
)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Optional: show the build type
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")


# --- find packages ---
find_package(Git QUIET)
find_package(spdlog REQUIRED)

if(GIT_FOUND)
    # Get commit hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Get commit date
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --format=%cd --date=short
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    message(WARNING "Git not found. Commit info will be set to 'unknown'.")
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_COMMIT_DATE "unknown")
endif()

# Common sources
set(SRC_FILES
    src/main.cpp
    src/modbus_master.cpp
    src/config_yaml.cpp
    src/mqtt_client.cpp
)

# This project will output an executable file
add_executable(${PROJECT_NAME} ${SRC_FILES})
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 23)

target_include_directories(${PROJECT_NAME} PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}
) 

# Create a simple configuration header
configure_file(config.h.in config.h)

add_library(fronius SHARED IMPORTED)

# Path to the actual library file
set_target_properties(fronius PROPERTIES
    IMPORTED_LOCATION "/home/alex/eclipse-workspace/libfronius/build/default/lib/libfronius.so"
    INTERFACE_INCLUDE_DIRECTORIES "/home/alex/eclipse-workspace/libfronius/include"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    fronius    
    modbus
    yaml-cpp
    mosquitto
    spdlog::spdlog
)

