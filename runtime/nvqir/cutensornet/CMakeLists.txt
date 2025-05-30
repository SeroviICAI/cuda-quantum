# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

# Find CUDA Toolkit for CUDA libs, e.g., cudart.
enable_language(CUDA)
find_package(CUDAToolkit REQUIRED)

find_library(CUTENSOR_LIB
    NAMES   cutensor libcutensor.so.2
    HINTS   
        ${CUTENSOR_ROOT}/lib64
        ${CUTENSOR_ROOT}/lib
        ${CUTENSOR_ROOT}/lib64/${CUDAToolkit_VERSION_MAJOR}
        ${CUTENSOR_ROOT}/lib/${CUDAToolkit_VERSION_MAJOR}
)

find_library(CUTENSORNET_LIB
    NAMES   cutensornet libcutensornet.so.2
    HINTS   
        ${CUTENSORNET_ROOT}/lib64
        ${CUTENSORNET_ROOT}/lib
        ${CUTENSORNET_ROOT}/lib64/${CUDAToolkit_VERSION_MAJOR}
        ${CUTENSORNET_ROOT}/lib/${CUDAToolkit_VERSION_MAJOR}
)

find_file(CUTENSORNET_INC
    NAMES   cutensornet.h
    HINTS   
        ${CUTENSORNET_ROOT}/include      
        /usr/include    
        ENV CPATH
)

if(NOT CUTENSOR_LIB)
  message(FATAL_ERROR "\nUnable to find cutensor installation. Please ensure it is correctly installed and set and define CUTENSOR_ROOT if necessary (currently set to: ${CUTENSOR_ROOT}).")
endif()
message(STATUS "CUTENSOR_LIB: ${CUTENSOR_LIB}")

if(NOT CUTENSORNET_LIB OR NOT CUTENSORNET_INC)
  message(FATAL_ERROR "\nUnable to find the cutensornet installation. Please ensure it is correctly installed and define CUTENSORNET_ROOT if necessary (currently set to: ${CUTENSORNET_ROOT}).")
endif()
message(STATUS "CUTENSORNET_LIB: ${CUTENSORNET_LIB}")
message(STATUS "CUTENSORNET_INC: ${CUTENSORNET_INC}")

# Determine cutensornet version
file(READ "${CUTENSORNET_INC}" cutensornet_header)
string(REGEX MATCH "CUTENSORNET_MAJOR ([0-9]*)" _ ${cutensornet_header})
set(CUTENSORNET_MAJOR ${CMAKE_MATCH_1})

string(REGEX MATCH "CUTENSORNET_MINOR ([0-9]*)" _ ${cutensornet_header})
set(CUTENSORNET_MINOR ${CMAKE_MATCH_1})

string(REGEX MATCH "CUTENSORNET_PATCH ([0-9]*)" _ ${cutensornet_header})
set(CUTENSORNET_PATCH ${CMAKE_MATCH_1})

set(CUTENSORNET_VERSION ${CUTENSORNET_MAJOR}.${CUTENSORNET_MINOR}.${CUTENSORNET_PATCH})
message(STATUS "Found cutensornet version: ${CUTENSORNET_VERSION}")
# We need cutensornet v2.7.0+ (cutensornetStateApplyGeneralChannel)
if (${CUTENSORNET_VERSION} VERSION_GREATER_EQUAL "2.7")
  set (BASE_TENSOR_BACKEND_SRS  
        simulator_cutensornet.cpp 
        tensornet_spin_op.cpp
        tensornet_state.cpp
        tensornet_utils.cpp)
  get_filename_component(CUTENSORNET_INCLUDE_DIR ${CUTENSORNET_INC} DIRECTORY)
  get_filename_component(CUTENSORNET_LIB_DIR ${CUTENSORNET_LIB} DIRECTORY)
  get_filename_component(CUTENSOR_LIB_DIR ${CUTENSOR_LIB} DIRECTORY)
  SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH}:${CUTENSORNET_LIB_DIR}:${CUTENSOR_LIB_DIR}")

  # Helper macro to add cutensornet-based backends
  macro (nvqir_create_cutn_plugin LIBRARY_NAME)
    # This will create a target named ${LIBRARY_NAME}
    add_library(nvqir-${LIBRARY_NAME} SHARED ${ARGN})
    target_include_directories(nvqir-${LIBRARY_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/runtime/common ${CMAKE_SOURCE_DIR}/runtime/nvqir ${CUDAToolkit_INCLUDE_DIRS} ${CUTENSORNET_INCLUDE_DIR})
    target_link_libraries(nvqir-${LIBRARY_NAME} PRIVATE fmt::fmt-header-only cudaq cudaq-common ${CUTENSORNET_LIB} ${CUTENSOR_LIB} CUDA::cudart_static)
    install(TARGETS nvqir-${LIBRARY_NAME} DESTINATION lib)
    add_target_config(${LIBRARY_NAME})
  endmacro()

  nvqir_create_cutn_plugin(tensornet ${BASE_TENSOR_BACKEND_SRS} simulator_tensornet_register.cpp tn_simulation_state.cpp)
  nvqir_create_cutn_plugin(tensornet-mps ${BASE_TENSOR_BACKEND_SRS} simulator_mps_register.cpp mps_simulation_state.cpp)
  add_library(tensornet-mpi-util OBJECT mpi_support.cpp)
  target_include_directories(tensornet-mpi-util PRIVATE ${CUDAToolkit_INCLUDE_DIRS} ${CUTENSORNET_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/runtime)
  target_link_libraries(tensornet-mpi-util PRIVATE cudaq-common fmt::fmt-header-only)
  # Note: only tensornet backend supports MPI at cutensornet level (distributed tensor computation)
  target_link_libraries(nvqir-tensornet PRIVATE tensornet-mpi-util)
else()
  message(WARNING "Skipped tensornet backend due to incompatible cutensornet version. Please install cutensornet v2.3.0+.")
endif()
