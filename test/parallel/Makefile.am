# Don't require GNU-standard files (Changelog, README, etc.)
AUTOMAKE_OPTIONS = foreign subdir-objects

LDADD = $(top_builddir)/src/libMOAB.la

MESHDIR = $(abs_top_srcdir)/MeshFiles/unittest

DEFS += -DIS_BUILDING_MB \
				-DSRCDIR=$(srcdir) \
        -DMESHDIR=$(MESHDIR)

FCDEFS += $(FC_DEFINE)IS_BUILDING_MB \
					$(FC_DEFINE)SRCDIR=$(srcdir) \
          $(FC_DEFINE)MESHDIR=$(MESHDIR)

AM_CPPFLAGS  = -I. \
               -I.. -I$(srcdir)/.. \
               -I$(top_builddir)/src \
               -I$(top_srcdir)/src \
               -I$(top_srcdir)/src/parallel \
               -I$(top_srcdir)/src/IntxMesh \
               -I$(top_srcdir)/src/io/mhdf/include

if ENABLE_mbcoupler
  AM_CPPFLAGS += -I$(top_srcdir)/tools/mbcoupler
endif

# Run parallel tests in parallel
if USE_MPIEXEC
  LOG_COMPILER = ${MPIEXEC}
  AM_LOG_FLAGS = ${MPIEXEC_NP} ${NP}
endif

TESTS = pcomm_unit \
        scdtest \
        pcomm_serial \
        par_spatial_locator_test \
        parallel_unit_tests imoab_ptest2 \
        uber_parallel_test \
        hireconst_test_parallel \
        $(NETCDF_TESTS) \
        $(HDF5_TESTS) \
        $(MBCSLAM_TESTS) $(IMESH_TESTS) $(IMOAB_TESTS)

if HAVE_HDF5_PARALLEL
  HDF5_TESTS = parallel_hdf5_test mhdf_parallel parallel_write_test \
         parallel_adj parmerge_test augment_with_ghosts uref_hirec_test_parallel migrate_test imoab_coupler \
         ghost_thin_layers 
if HAVE_ZOLTAN
         HDF5_TESTS += migrate_nontrivial
endif 

else
  HDF5_TESTS =
endif

if HAVE_NETCDF
  NETCDF_TESTS = scdpart read_nc_par ucdtrvpart mpastrvpart gcrm_par
else
  NETCDF_TESTS =
endif

if HAVE_PNETCDF
if !HAVE_NETCDF
  NETCDF_TESTS += scdpart read_nc_par ucdtrvpart mpastrvpart write_nc_par
else
  NETCDF_TESTS += write_nc_par
endif
endif

if HAVE_HYPRE
  MBHYPRE_TESTS = hypre_test
else
  MBHYPRE_TESTS =
endif

if HAVE_HDF5_PARALLEL
  MBCSLAM_TESTS = par_intx_sph
else
  MBCSLAM_TESTS =
endif

IMOAB_TESTS =
if ENABLE_FORTRAN
   IMOAB_TESTS += imoab_ptest
endif

IMESH_TESTS =
if ENABLE_imesh
if HAVE_HDF5_PARALLEL
if ENABLE_FORTRAN
   IMESH_TESTS += imeshp_test
   FCFLAGS += -I$(top_srcdir)/itaps \
              -I$(top_srcdir)/itaps/imesh \
              -I$(top_builddir)/itaps \
              -I$(top_builddir)/itaps/imesh
endif
endif
endif

if HAVE_HYPRE
AM_CPPFLAGS += $(HYPRE_INCLUDES) -I$(top_srcdir)/src/HYPRE/moab
AM_LDFLAGS += $(HYPRE_LIBS)

hypre_test_SOURCES = hypre_test.cpp
TESTS += hypre_test
endif

check_PROGRAMS = $(TESTS) mbparallelcomm_test partcheck structured3
if HAVE_HDF5_PARALLEL
  check_PROGRAMS += parmerge
endif


pcomm_unit_SOURCES = pcomm_unit.cpp
parallel_hdf5_test_SOURCES = parallel_hdf5_test.cpp
mhdf_parallel_SOURCES = mhdf_parallel.c
mhdf_parallel_LDADD = $(LDADD) $(HDF5_LIBS)
parallel_unit_tests_SOURCES = parallel_unit_tests.cpp
parallel_write_test_SOURCES = parallel_write_test.cpp
uber_parallel_test_SOURCES = uber_parallel_test.cpp
pcomm_serial_SOURCES = pcomm_serial.cpp
mbparallelcomm_test_SOURCES = mbparallelcomm_test.cpp
scdtest_SOURCES = scdtest.cpp
partcheck_SOURCES = partcheck.cpp
structured3_SOURCES = structured3.cpp
parmerge_SOURCES = parmerge.cpp
scdpart_SOURCES = scdpart.cpp
read_nc_par_SOURCES = ../io/read_nc.cpp
ucdtrvpart_SOURCES = ucdtrvpart.cpp
mpastrvpart_SOURCES = mpastrvpart.cpp
gcrm_par_SOURCES = gcrm_par.cpp
write_nc_par_SOURCES = ../io/write_nc.cpp
par_spatial_locator_test_SOURCES = par_spatial_locator_test.cpp
parallel_adj_SOURCES = ../adj_moab_test.cpp
parmerge_test_SOURCES = parmerge_test.cpp
augment_with_ghosts_SOURCES = augment_with_ghosts.cpp
imoab_ptest_SOURCES = imoab_ptest.F
imoab_ptest_LDADD = $(LDADD) $(FCLIBS)
imoab_ptest2_SOURCES = imoab_ptest2.cpp
hireconst_test_parallel_SOURCES = hireconst_test_parallel.cpp
uref_hirec_test_parallel_SOURCES = uref_hirec_test_parallel.cpp ../geomObject.cpp
migrate_test_SOURCES = migrate_test.cpp
imoab_coupler_SOURCES = imoab_coupler.cpp
migrate_nontrivial_SOURCES = migrate_nontrivial.cpp
ghost_thin_layers_SOURCES = ghost_thin_layers.cpp

if ENABLE_imesh
if HAVE_HDF5_PARALLEL
if ENABLE_FORTRAN
  imeshp_test_SOURCES = imeshp_test.F90
  imeshp_test_LDADD =   $(top_builddir)/itaps/imesh/libiMesh.la $(LDADD) ${MOAB_CXX_LINKFLAGS} ${MOAB_CXX_LIBS} $(FCLIBS)
endif
endif
endif


par_intx_sph_SOURCES = par_intx_sph.cpp
par_intx_sph_LDADD = $(LDADD)

# Other files to clean up (e.g. output from tests)
MOSTLYCLEANFILES = mhdf_ll.h5m \
		   tmp0.0.h5m tmp1.0.h5m tmp2.0.h5m tmp3.0.h5m \
		   tmp0.1.h5m tmp1.1.h5m tmp2.1.h5m tmp3.1.h5m \
                   partial0.vtk \
                   partial1.vtk  \
                   partialConvex1.vtk \
                   partialConvex0.vtk \
                   initial0.vtk \
                   intersect1.h5m \
                   intersect0.h5m \
                   test.h5m \
                   initial1.vtk \
                   test_mpas.h5m \
                   dum.h5m \
                   test_gcrm.h5m \
                   test_gcrm_rcbzoltan.h5m \
                   test_mpas_no_mixed_elements.h5m \
                   test_mpas_rcbzoltan.h5m \
                   test_mpas_rcbzoltan_no_mixed_elements.h5m \
                   test_par_eul_across_files.nc \
                   test_par_eul_append.nc \
                   test_par_eul_ghosting.nc \
                   test_par_eul_T2.nc \
                   test_par_eul_T.nc \
                   test_par_fv_T.nc \
                   test_par_gcrm_vars.nc \
                   test_par_homme_T.nc \
                   test_par_mpas_vars.nc \
                   test2.h5m  \
                   testpm.h5m \
                   newfile.h5m \
                   TaskMesh_n2.0.h5m \
                   TaskMesh_n2.1.h5m \
                   testReadGhost_n2.0.h5m \
                   testReadGhost_n2.1.h5m \
                   whole.h5m \
                   recvMesh.h5m \
                   migrate12.h5m migrate21.h5m migrate11.h5m migrate22.h5m \
                   migrate11_tag.h5m migrate21_tag.h5m migrate12_tag.h5m migrate22_tag.h5m \
                   intx_00.h5m intx_01.h5m fIntxTarget.h5m recvTarget.h5m OcnWithProj.h5m \
                   migrate_graph.h5m migrate_geom.h5m

FORTDEFS = $(FCDEFS)
PPFCCOMPILE = $(FC) $(FORTDEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
    $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_FCFLAGS) $(FCFLAGS)
LTPPFCCOMPILE = $(LIBTOOL) $(AM_V_lt) --tag=FC $(AM_LIBTOOLFLAGS) \
    $(LIBTOOLFLAGS) --mode=compile $(FC) $(FCDEFS) \
    $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) \
    $(AM_FCFLAGS) $(FCFLAGS)
PPF77COMPILE = $(F77) $(FORTDEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
   $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_FFLAGS) $(FFLAGS)
LTPPF77COMPILE = $(LIBTOOL) $(AM_V_lt) --tag=F77 $(AM_LIBTOOLFLAGS) \
   $(LIBTOOLFLAGS) --mode=compile $(F77) $(FCDEFS) \
   $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) \
   $(AM_FFLAGS) $(FFLAGS)

