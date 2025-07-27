# Configure Project Details
cmake_minimum_required(VERSION 3.26)
project(vm C)
set(CMAKE_C_STANDARD 11)

set(CMAKE_BUILD_TYPE Debug)
add_compile_options(/Zi)  # Debug info
add_link_options(/DEBUG)  # Include debug info in PDB

# Configure include directories
include_directories(include)

# Configure source files
file(GLOB SOURCES "src/*.c")

# Configure executable
add_executable(vm ${SOURCES}
        include/scheduler.h
        include/bitmap.h
        src/bitmap.c)

# Configure installation
install(TARGETS vm DESTINATION bin)
install(FILES "include/vm.h" DESTINATION include)