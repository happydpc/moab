
if ENABLE_imesh
  imesh_DIR = imesh
else
  imesh_DIR =
endif

if ENABLE_fbigeom
  fbigeom_DIR = fbigeom
else
  fbigeom_DIR =
endif

if ENABLE_irel
  irel_DIR = irel
else
  irel_DIR =
endif

if ENABLE_mesquite
  mesquite_DIR = mesquite
else
  mesquite_DIR =
endif

include_HEADERS = iBase.h iBase_f.h

SUBDIRS = $(imesh_DIR) $(fbigeom_DIR) $(irel_DIR) $(mesquite_DIR)

EXTRA_DIST = CMakeLists.txt

