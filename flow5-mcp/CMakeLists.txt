cmake_minimum_required(VERSION 3.16)

# flow5-mcp: Model Context Protocol server for flow5
# Alternative to flow5-mcp.pro. Builds against the flow5 libraries,
# either installed with "make install" or left in their build directories.
#
#   cmake -S . -B build -DFLOW5_LIB_DIRS="/path/to/build/flow5-lib;/path/to/build/flow5-io-lib;/path/to/build/XFoil-lib"
#   cmake --build build

project(flow5-mcp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC OFF)

find_package(Qt6 REQUIRED COMPONENTS Core)

set(FLOW5_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Root of the flow5 source tree, for the headers")
set(FLOW5_LIB_DIRS "/usr/local/lib" CACHE STRING "Directories containing flow5-lib, flow5-io-lib and XFoil")

# override with -DOCC_INCLUDE_DIR=... and -DGMSH_INCLUDE_DIR=... if not found
find_path(OCC_INCLUDE_DIR TopoDS_Shape.hxx PATH_SUFFIXES opencascade
          PATHS /usr/local/include /usr/include /opt/homebrew/include
          DOC "OpenCASCADE include directory")
find_path(GMSH_INCLUDE_DIR gmsh.h PATHS /usr/local/include /usr/include /opt/homebrew/include
          DOC "gmsh include directory")
if(NOT OCC_INCLUDE_DIR OR NOT GMSH_INCLUDE_DIR)
    message(FATAL_ERROR "OpenCASCADE or gmsh headers not found; set OCC_INCLUDE_DIR and GMSH_INCLUDE_DIR")
endif()

add_executable(flow5-mcp
    main.cpp
    mcp/mcpserver.cpp mcp/mcpserver.h
    tools/common.cpp
    tools/flow5tools.h
    tools/foiltools.cpp
    tools/planetools.cpp
    tools/projecttools.cpp
    tools/resources.cpp
)

target_include_directories(flow5-mcp PRIVATE
    ${FLOW5_SOURCE_DIR}/XFoil-lib
    ${FLOW5_SOURCE_DIR}/flow5-lib
    ${FLOW5_SOURCE_DIR}/flow5-lib/api
    ${FLOW5_SOURCE_DIR}/flow5-io-lib
    ${FLOW5_SOURCE_DIR}/flow5-io-lib/api
    ${OCC_INCLUDE_DIR}
    ${GMSH_INCLUDE_DIR}
)

target_link_directories(flow5-mcp PRIVATE ${FLOW5_LIB_DIRS})
target_link_libraries(flow5-mcp PRIVATE flow5-io-lib flow5-lib Qt6::Core)

if(WIN32)
    target_compile_definitions(flow5-mcp PRIVATE WIN_OS)
    target_compile_options(flow5-mcp PRIVATE /Zc:__cplusplus /EHsc /utf-8 /permissive-)
    target_link_libraries(flow5-mcp PRIVATE XFoil1 gmsh.dll.lib)
elseif(APPLE)
    target_compile_definitions(flow5-mcp PRIVATE MAC_OS)
    target_link_libraries(flow5-mcp PRIVATE XFoil gmsh)
else()
    target_compile_definitions(flow5-mcp PRIVATE LINUX_OS)
    target_link_libraries(flow5-mcp PRIVATE XFoil gmsh)
    set_target_properties(flow5-mcp PROPERTIES BUILD_RPATH "${FLOW5_LIB_DIRS}")
endif()

include(GNUInstallDirs)
install(TARGETS flow5-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
