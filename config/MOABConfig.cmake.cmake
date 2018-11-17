 Config file for MOAB; use the CMake find_package() function to pull this into
# your own CMakeLists.txt file.
#
# This file defines the following variables:
# MOAB_FOUND        - boolean indicating that MOAB is found
# MOAB_INCLUDE_DIRS - include directories from which to pick up MOAB includes
# MOAB_LIBRARIES    - libraries need to link to MOAB; use this in target_link_libraries for MOAB-dependent targets
# MOAB_CXX, MOAB_CC, MOAB_F77, MOAB_FC - compilers used to compile MOAB
# MOAB_CXXFLAGS, MOAB_CCFLAGS, MOAB_FFLAGS, MOAB_FCFLAGS - compiler flags used to compile MOAB; possibly need to use these in add_definitions or CMAKE_<LANG>_FLAGS_<MODE> 

set(MOAB_FOUND 1)

set(MOAB_CC @CMAKE_C_COMPILER@)
set(MOAB_CXX @CMAKE_CXX_COMPILER@)
set(MOAB_FC @CMAKE_Fortran_COMPILER@)
set(MOAB_F77 @CMAKE_Fortran_COMPILER@)
# Compiler flags used by MOAB
set(MOAB_CFLAGS "@CFLAGS@")
set(MOAB_CXXFLAGS "@CXXFLAGS@")
set(MOAB_FCFLAGS "@FFLAGS@")
set(MOAB_FFLAGS "@FFLAGS@")

set(MOAB_BUILT_SHARED @BUILD_SHARED_LIBS@)
set(MOAB_USE_MPI @MOAB_HAVE_MPI@)
set(MPI_DIR "@MPI_HOME@")
set(MOAB_USE_HDF5 @MOAB_HAVE_HDF5@)
set(MOAB_USE_HDF5_PARALLEL @MOAB_HAVE_HDF5_PARALLEL@)
set(HDF5_DIR "@HDF5_ROOT@")
set(MOAB_USE_ZLIB @MOAB_HAVE_ZLIB@)
set(ZLIB_DIR "@ZLIB_DIR@")
set(MOAB_USE_SZIP @MOAB_HAVE_SZIP@)
set(SZIP_DIR "@SZIP_DIR@")
set(MOAB_USE_NETCDF @MOAB_HAVE_NETCDF@)
set(NETCDF_DIR "@NETCDF_DIR@")
set(MOAB_USE_PNETCDF @MOAB_HAVE_PNETCDF@)
set(PNETCDF_DIR "@PNETCDF_DIR@")
set(MOAB_USE_METIS @MOAB_HAVE_METIS@)
set(METIS_DIR "@METIS_DIR@")
set(MOAB_USE_PARMETIS @MOAB_HAVE_PARMETIS@)
set(PARMETIS_DIR "@PARMETIS_DIR@")
set(MOAB_USE_ZOLTAN @MOAB_HAVE_ZOLTAN@)
set(ZOLTAN_DIR "@ZOLTAN_DIR@")
set(MOAB_USE_BLAS @MOAB_HAVE_BLAS@)
set(BLAS_LIBRARIES "@BLAS_LIBRARIES@")
set(MOAB_USE_LAPACK @MOAB_HAVE_LAPACK@)
set(LAPACK_LIBRARIES "@LAPACK_LIBRARIES@")
set(MOAB_USE_EIGEN @MOAB_HAVE_EIGEN3@)
set(EIGEN_INCLUDE_DIR "@EIGEN3_INCLUDE_DIR@")

set(MOAB_MESH_DIR "@CMAKE_SOURCE_DIR@/MeshFiles/unittest")

set(ENABLE_IGEOM @MOAB_HAVE_CGM@)
set(CGM_DIR "@CGM_DIR@")
set(ENABLE_IMESH @ENABLE_IMESH@)
set(ENABLE_IREL @ENABLE_IREL@)
set(ENABLE_FBIGEOM @ENABLE_FBIGEOM@)

# missing support for DAMSEL, CCMIO
set (MOAB_PACKAGE_LIBS "@ZOLTAN_LIBRARIES@ @PNETCDF_LIBRARIES@ @NETCDF_LIBRARIES@ @CGNS_LIBRARIES@ @HDF5_LIBRARIES@ @CGM_LIBRARIES@ @PARMETIS_LIBRARIES@ @METIS_LIBRARIES@ @LAPACK_LIBRARIES@ @BLAS_LIBRARIES@ @MPI_CXX_LIBRARIES@" )
string(STRIP "${MOAB_PACKAGE_LIBS}" MOAB_PACKAGE_LIBS)
set(MOAB_PACKAGE_LIBS_LIST ${MOAB_PACKAGE_LIBS})
separate_arguments(MOAB_PACKAGE_LIBS_LIST)
list(REMOVE_DUPLICATES MOAB_PACKAGE_LIBS_LIST)
set(MOAB_PACKAGE_LIBS "${MOAB_PACKAGE_LIBS_LIST}")
set (MOAB_PACKAGE_INCLUDES "@ZOLTAN_INCLUDES@ @PNETCDF_INCLUDES@ @NETCDF_INCLUDES@ @HDF5_INCLUDES@ @PARMETIS_INCLUDES@ @METIS_INCLUDES@ @EIGEN_INCLUDE_DIR@ @IGEOM_INCLUDES@ @CGM_INCLUDES@" )
string(STRIP "${MOAB_PACKAGE_INCLUDES}" MOAB_PACKAGE_INCLUDES)
separate_arguments(MOAB_PACKAGE_INCLUDES)
list(REMOVE_DUPLICATES MOAB_PACKAGE_INCLUDES)

# Library and include defs
get_filename_component(MOAB_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

if(NOT TARGET MOAB AND NOT MOAB_BINARY_DIR)
  include("${MOAB_CMAKE_DIR}/MOABTargets.cmake")
endif()

# Target information
if(MOAB_USE_HDF5)
  if(EXISTS "@HDF5_ROOT@/share/cmake/hdf5/hdf5-config.cmake")
    include(@HDF5_ROOT@/share/cmake/hdf5/hdf5-config.cmake)
  endif()
endif()

set(MOAB_LIBRARY_DIRS "@CMAKE_INSTALL_PREFIX@/lib")
set(MOAB_INCLUDE_DIRS "-I@CMAKE_INSTALL_PREFIX@/include ${MOAB_PACKAGE_INCLUDES}")
set(MOAB_LIBS "-lMOAB")
set(MOAB_LIBRARIES "-L@CMAKE_INSTALL_PREFIX@/@CMAKE_INSTALL_LIBDIR@ ${MOAB_LIBS} ${MOAB_PACKAGE_LIBS}")
if(ENABLE_IMESH)
  set(MOAB_LIBS "-liMesh ${MOAB_LIBS}")
  set(IMESH_LIBRARIES "-L@CMAKE_INSTALL_PREFIX@/@CMAKE_INSTALL_LIBDIR@ ${MOAB_LIBS} ${MOAB_PACKAGE_LIBS}")
endif(ENABLE_IMESH)
if(ENABLE_FBIGEOM)
  set(MOAB_LIBS "-lFBiGeom ${MOAB_LIBS}")
  set(FBIGEOM_LIBRARIES "-L@CMAKE_INSTALL_PREFIX@/@CMAKE_INSTALL_LIBDIR@ ${MOAB_LIBS} ${MOAB_PACKAGE_LIBS}")
endif(ENABLE_FBIGEOM)
if(ENABLE_IREL)
  set(MOAB_LIBS "-liRel ${MOAB_LIBS}")
  set(IREL_LIBRARIES "-L@CMAKE_INSTALL_PREFIX@/@CMAKE_INSTALL_LIBDIR@ ${MOAB_LIBS} ${MOAB_PACKAGE_LIBS}")
endif(ENABLE_IREL)


