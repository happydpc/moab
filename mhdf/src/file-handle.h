/**
 * MOAB, a Mesh-Oriented datABase, is a software component for creating,
 * storing and accessing finite element mesh data.
 * 
 * Copyright 2004 Sandia Corporation.  Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Coroporation, the U.S. Government
 * retains certain rights in this software.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 */

#ifndef MHDF_FILE_HANDLE_H
#define MHDF_FILE_HANDLE_H

#include "mhdf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct struct_FileHandle {
  uint32_t magic;
  hid_t hdf_handle;
  int open_handle_count;
  
  long max_id;
} FileHandle;

FileHandle* mhdf_alloc_FileHandle( hid_t hdf_handle, mhdf_Status* status );

int mhdf_check_valid_file( FileHandle* handle, mhdf_Status* status );

#ifdef __cplusplus
} // extern "C"
#endif

#endif
