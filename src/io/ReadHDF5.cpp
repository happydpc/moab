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

//-------------------------------------------------------------------------
// Filename      : ReadHDF5.cpp
//
// Purpose       : HDF5 Writer 
//
// Creator       : Jason Kraftcheck
//
// Creation Date : 04/18/04
//-------------------------------------------------------------------------

#include <assert.h>
/* include our MPI header before any HDF5 because otherwise
   it will get included indirectly by HDF5 */
#ifdef USE_MPI
#  include "moab_mpi.h"
#  include "moab/ParallelComm.hpp"
#endif 
#include <H5Tpublic.h>
#include <H5Ppublic.h>
#include <H5Epublic.h>
#include "moab/Interface.hpp"
#include "Internals.hpp"
#include "MBTagConventions.hpp"
#include "ReadHDF5.hpp"
#include "moab/CN.hpp"
#include "FileOptions.hpp"
#ifdef HDF5_PARALLEL
#  include <H5FDmpi.h>
#  include <H5FDmpio.h>
#endif
//#include "WriteHDF5.hpp"

#include <stdlib.h>
#include <string.h>
#include <limits>
#include <functional>

#include "IODebugTrack.hpp"
#include "ReadHDF5Dataset.hpp"
#include "ReadHDF5VarLen.hpp"

namespace moab {

#undef BLOCKED_COORD_IO

#define READ_HDF5_BUFFER_SIZE (128*1024*1024)

#define assert_range( PTR, CNT ) \
  assert( (PTR) >= (void*)dataBuffer ); assert( ((PTR)+(CNT)) <= (void*)(dataBuffer + bufferSize) );


// This function doesn't do anything useful.  It's just a nice
// place to set a break point to determine why the reader fails.
static inline ErrorCode error( ErrorCode rval )
  { return rval; }

// Call \c error function during HDF5 library errors to make
// it easier to trap such errors in the debugger.  This function
// gets registered with the HDF5 library as a callback.  It
// works the same as the default (H5Eprint), except that it 
// also calls the \c error fuction as a no-op.
#if defined(H5E_auto_t_vers) && H5E_auto_t_vers > 1
static herr_t handle_hdf5_error( hid_t stack, void* data )
{
  ReadHDF5::HDF5ErrorHandler* h = reinterpret_cast<ReadHDF5::HDF5ErrorHandler*>(data);
  herr_t result = (*h->func)(stack,h->data);
  error(MB_FAILURE);
  return result;
}
#else
static herr_t handle_hdf5_error( void* data )
{
  ReadHDF5::HDF5ErrorHandler* h = reinterpret_cast<ReadHDF5::HDF5ErrorHandler*>(data);
  herr_t result = (*h->func)(h->data);
  error(MB_FAILURE);
  return result;
}
#endif

static void copy_sorted_file_ids( const EntityHandle* sorted_ids, 
                                  long num_ids,
                                  Range& results )
{
  Range::iterator hint = results.begin();
  long i = 0;
  while (i < num_ids) {
    EntityHandle start = sorted_ids[i];
    for (++i; i < num_ids && sorted_ids[i] == 1+sorted_ids[i-1]; ++i);
    hint = results.insert( hint, start, sorted_ids[i-1] );
  }
}

static void intersect( const mhdf_EntDesc& group, const Range& range, Range& result )
{
  Range::const_iterator s, e;
  s = Range::lower_bound( range.begin(), range.end(), group.start_id );
  e = Range::lower_bound( s, range.end(), group.start_id + group.count );
  result.merge( s, e );
}

#define debug_barrier() debug_barrier_line(__LINE__)
void ReadHDF5::debug_barrier_line(int lineno)
{
#ifdef USE_MPI
  const unsigned threshold = 2;
  static unsigned long count = 0;
  if (dbgOut.get_verbosity() >= threshold) {
    dbgOut.printf( threshold, "*********** Debug Barrier %lu (@%d)***********\n", ++count, lineno);
    MPI_Barrier( myPcomm->proc_config().proc_comm() );
  }
#endif
}

ReaderIface* ReadHDF5::factory( Interface* iface )
  { return new ReadHDF5( iface ); }

ReadHDF5::ReadHDF5( Interface* iface )
  : bufferSize( READ_HDF5_BUFFER_SIZE ),
    dataBuffer( 0 ),
    iFace( iface ), 
    filePtr( 0 ), 
    fileInfo( 0 ), 
    readUtil( 0 ),
    handleType( 0 ),
    indepIO( H5P_DEFAULT ),
    collIO( H5P_DEFAULT ),
    debugTrack( false ),
    dbgOut(stderr)
{
}

ErrorCode ReadHDF5::init()
{
  ErrorCode rval;

  if (readUtil) 
    return MB_SUCCESS;
  
  indepIO = collIO = H5P_DEFAULT;
  //WriteHDF5::register_known_tag_types( iFace );
  
  handleType = H5Tcopy( H5T_NATIVE_ULONG );
  if (handleType < 0)
    return error(MB_FAILURE);
  
  if (H5Tset_size( handleType, sizeof(EntityHandle)) < 0)
  {
    H5Tclose( handleType );
    return error(MB_FAILURE);
  }
  
  void* ptr = 0;
  rval = iFace->query_interface( "ReadUtilIface", &ptr );
  if (MB_SUCCESS != rval)
  {
    H5Tclose( handleType );
    return error(rval);
  }
  readUtil = reinterpret_cast<ReadUtilIface*>(ptr);
  
  idMap.clear();
  fileInfo = 0;
  debugTrack = false;
  myPcomm = 0;
  
  return MB_SUCCESS;
}
  

ReadHDF5::~ReadHDF5()
{
  if (!readUtil) // init() failed.
    return;

  iFace->release_interface( "ReadUtilIface", readUtil );
  H5Tclose( handleType );
}

ErrorCode ReadHDF5::set_up_read( const char* filename,
                                 const FileOptions& opts )
{
  ErrorCode rval;
  mhdf_Status status;
  indepIO = collIO = H5P_DEFAULT;
  mpiComm = 0;

  if (MB_SUCCESS != init())
    return error(MB_FAILURE);
  
#if defined(H5Eget_auto_vers) && H5Eget_auto_vers > 1
  herr_t err = H5Eget_auto( H5E_DEFAULT, &errorHandler.func, &errorHandler.data );
#else
  herr_t err = H5Eget_auto( &errorHandler.func, &errorHandler.data );
#endif
  if (err < 0) {
    errorHandler.func = 0;
    errorHandler.data = 0;
  }
  else {
#if defined(H5Eset_auto_vers) && H5Eset_auto_vers > 1
    err = H5Eset_auto( H5E_DEFAULT, &handle_hdf5_error, &errorHandler );
#else
    err = H5Eset_auto( &handle_hdf5_error, &errorHandler );
#endif
    if (err < 0) {
      errorHandler.func = 0;
      errorHandler.data = 0;
    }
  }
      
  
  // Set up debug output
  int tmpval;
  if (MB_SUCCESS == opts.get_int_option("DEBUG_IO", 1, tmpval)) {
    dbgOut.set_verbosity(tmpval);
    dbgOut.set_prefix("H5M ");
  }
  
  // Enable some extra checks for reads.  Note: amongst other things this
  // will print errors if the entire file is not read, so if doing a 
  // partial read that is not a parallel read, this should be disabled.
  debugTrack = (MB_SUCCESS == opts.get_null_option("DEBUG_BINIO"));
    
    // Handle parallel options
  std::string junk;
  bool use_mpio = (MB_SUCCESS == opts.get_null_option("USE_MPIO"));
  rval = opts.match_option("PARALLEL", "READ_PART");
  bool parallel = (rval != MB_ENTITY_NOT_FOUND);
  nativeParallel = (rval == MB_SUCCESS);
  if (use_mpio && !parallel) {
    readUtil->report_error( "'USE_MPIO' option specified w/out 'PARALLEL' option" );
    return MB_NOT_IMPLEMENTED;
  }
  
  // This option is intended for testing purposes only, and thus
  // is not documented anywhere.  Decreasing the buffer size can
  // expose bugs that would otherwise only be seen when reading
  // very large files.
  rval = opts.get_int_option( "BUFFER_SIZE", bufferSize );
  if (MB_SUCCESS != rval) {
    bufferSize = READ_HDF5_BUFFER_SIZE;
  }
  else if (bufferSize < (int)std::max( sizeof(EntityHandle), sizeof(void*) )) {
    return error(MB_INVALID_SIZE);
  }
  
  ReadHDF5Dataset::default_hyperslab_selection_limit();
  int hslimit;
  rval = opts.get_int_option( "HYPERSLAB_SELECT_LIMIT", hslimit );
  if (MB_SUCCESS == rval && hslimit > 0)
    ReadHDF5Dataset::set_hyperslab_selection_limit( hslimit );
  else
    ReadHDF5Dataset::default_hyperslab_selection_limit();
  if (MB_SUCCESS == opts.get_null_option( "HYERERSLAB_APPEND" ))
    ReadHDF5Dataset::append_hyperslabs();
  
  dataBuffer = (char*)malloc( bufferSize );
  if (!dataBuffer)
    return error(MB_MEMORY_ALLOCATION_FAILED);
  
  if (use_mpio || nativeParallel) {
#ifndef HDF5_PARALLEL
    readUtil->report_error("MOAB not configured with parallel HDF5 support");
    free(dataBuffer);
    return MB_NOT_IMPLEMENTED;
#else

    int pcomm_no = 0;
    rval = opts.get_int_option("PARALLEL_COMM", pcomm_no);
    if (rval == MB_TYPE_OUT_OF_RANGE) {
      readUtil->report_error("Invalid value for PARALLEL_COMM option");
      return rval;
    }
    myPcomm = ParallelComm::get_pcomm(iFace, pcomm_no);
    if (0 == myPcomm) {
      myPcomm = new ParallelComm(iFace);
    }
    const int rank = myPcomm->proc_config().proc_rank();
    dbgOut.set_rank(rank);
    mpiComm = new MPI_Comm(myPcomm->proc_config().proc_comm());

      // Open the file in serial on root to read summary
    dbgOut.tprint( 1, "Getting file summary\n" );
    fileInfo = 0;
    unsigned long size = 0;


    /*
    if (rank == 0) {

      filePtr = mhdf_openFile( filename, 0, NULL, &status );
     
      if (filePtr) {  
        fileInfo = mhdf_getFileSummary( filePtr, handleType, &status );
        if (!is_error(status)) {
          size = fileInfo->total_size;
          fileInfo->offset = (unsigned char*)fileInfo;
        }
      }
      mhdf_closeFile( filePtr, &status );
      if (fileInfo && mhdf_isError(&status)) {
        free(fileInfo);
        fileInfo = 0;
      }
    }

    */


    /* dbgOut.tprint( 1, "Communicating file summary\n" );
    int mpi_err = MPI_Bcast( &size, 1, MPI_UNSIGNED_LONG, 0, myPcomm->proc_config().proc_comm() );
    if (mpi_err || !size)
      return MB_FAILURE;
    
      
    if (rank != 0) 
      fileInfo = reinterpret_cast<mhdf_FileDesc*>( malloc( size ) );
      
    MPI_Bcast( fileInfo, size, MPI_BYTE, 0, myPcomm->proc_config().proc_comm() );
      
    if (rank != 0)
      mhdf_fixFileDesc( fileInfo, reinterpret_cast<mhdf_FileDesc*>(fileInfo->offset) );
    */
  
    hid_t file_prop = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_fapl_mpio(file_prop, myPcomm->proc_config().proc_comm(), MPI_INFO_NULL);

    collIO = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_dxpl_mpio(collIO, H5FD_MPIO_COLLECTIVE);
    indepIO = nativeParallel ? H5P_DEFAULT : collIO;

      // re-open file in parallel
    dbgOut.tprintf( 1, "Re-opening \"%s\" for parallel IO\n", filename );
    filePtr = mhdf_openFileWithOpt( filename, 0, NULL, file_prop, &status );
    
    if (filePtr) {                                                                                                                                                                                                                                                           
      fileInfo = mhdf_getFileSummary( filePtr, handleType, &status );                                                                                                                                                                                                        
      if (!is_error(status)) {                                                                                                                                                                                                                                               
	size = fileInfo->total_size;                                                                                                                                                                                                                                         
	fileInfo->offset = (unsigned char*)fileInfo;                                                                                                                                                                                                                         
      }
    }
    

      


      
      

      

    H5Pclose( file_prop );
    if (!filePtr)
    {
      readUtil->report_error( mhdf_message( &status ));
      free( dataBuffer );
      H5Pclose( indepIO ); 
      if (collIO != indepIO)
        H5Pclose( collIO );
      collIO = indepIO = H5P_DEFAULT;
      return error(MB_FAILURE);
    }
#endif
  }
  else {
  
      // Open the file
    filePtr = mhdf_openFile( filename, 0, NULL, &status );
    if (!filePtr)
    {
      readUtil->report_error( "%s", mhdf_message( &status ));
      free( dataBuffer );
      return error(MB_FAILURE);
    }

      // get file info
    fileInfo = mhdf_getFileSummary( filePtr, handleType, &status );
    if (is_error(status)) {
      free( dataBuffer );
      mhdf_closeFile( filePtr, &status );
      return error(MB_FAILURE);
    }
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::clean_up_read( const FileOptions& )
{
  HDF5ErrorHandler handler;
#if defined(H5Eget_auto_vers) && H5Eget_auto_vers > 1
  herr_t err = H5Eget_auto( H5E_DEFAULT, &handler.func, &handler.data );
#else
  herr_t err = H5Eget_auto( &handler.func, &handler.data );
#endif
  if (err >= 0 && handler.func == &handle_hdf5_error) {
    assert(handler.data = &errorHandler);
#if defined(H5Eget_auto_vers) && H5Eget_auto_vers > 1
    H5Eset_auto( H5E_DEFAULT, errorHandler.func, errorHandler.data );
#else
    H5Eset_auto( errorHandler.func, errorHandler.data );
#endif
  }

  free( dataBuffer );
  free( fileInfo );
  delete mpiComm;
  mpiComm = 0;

  if (indepIO != H5P_DEFAULT)
    H5Pclose( indepIO );
  if (collIO != indepIO)
    H5Pclose( collIO );
  collIO = indepIO = H5P_DEFAULT;

  mhdf_Status status;
  mhdf_closeFile( filePtr, &status );
  filePtr = 0;
  return is_error(status) ? MB_FAILURE : MB_SUCCESS;
}

ErrorCode ReadHDF5::load_file( const char* filename, 
                               const EntityHandle* file_set, 
                               const FileOptions& opts,
                               const ReaderIface::SubsetList* subset_list,
                               const Tag* file_id_tag )
{
  ErrorCode rval;
 
  rval = set_up_read( filename, opts );
  if (MB_SUCCESS != rval)
    return rval;
 
  if (subset_list) 
    rval = load_file_partial( subset_list->tag_list, 
                              subset_list->tag_list_length, 
                              subset_list->num_parts,
                              subset_list->part_number,
                              opts );
  else
    rval = load_file_impl( opts );
    
  if (MB_SUCCESS == rval && file_id_tag) {
    dbgOut.tprint( 1, "Storing file IDs in tag\n" );
    rval = store_file_ids( *file_id_tag );
  }
  
  if (MB_SUCCESS == rval && 0 != file_set) {
    dbgOut.tprint( 1, "Reading QA records\n" );
    rval = read_qa( *file_set );
  }
  
  
  dbgOut.tprint( 1, "Cleaining up\n" );
  ErrorCode rval2 = clean_up_read( opts );
  if (rval == MB_SUCCESS && rval2 != MB_SUCCESS)
    rval = rval2;
  
  dbgOut.tprint(1, "Read finished.\n");
  
  if (H5P_DEFAULT != collIO)
    H5Pclose( collIO );
  if (H5P_DEFAULT != indepIO)
    H5Pclose( indepIO );
  collIO = indepIO = H5P_DEFAULT;
  
  return rval;
}
  


ErrorCode ReadHDF5::load_file_impl( const FileOptions& )
{
  ErrorCode rval;
  mhdf_Status status;
  std::string tagname;
  int i;

  dbgOut.tprint(1, "Reading all nodes...\n");
  Range ids;
  if (fileInfo->nodes.count) {
    ids.insert( fileInfo->nodes.start_id,
                fileInfo->nodes.start_id + fileInfo->nodes.count - 1);
    rval = read_nodes( ids );
    if (MB_SUCCESS != rval)
      return error(rval);
  }


  dbgOut.tprint(1, "Reading all element connectivity...\n");
  std::vector<int> polyhedra; // need to do these last so that faces are loaded
  for (i = 0; i < fileInfo->num_elem_desc; ++i) {
    if (CN::EntityTypeFromName(fileInfo->elems[i].type) == MBPOLYHEDRON) {
      polyhedra.push_back(i);
      continue;
    }
    
    rval = read_elems( i );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  for (std::vector<int>::iterator it = polyhedra.begin();
       it != polyhedra.end(); ++it) {
    rval = read_elems( *it );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
  dbgOut.tprint(1, "Reading all sets...\n");
  ids.clear();
  if (fileInfo->sets.count) {
    ids.insert( fileInfo->sets.start_id,
                fileInfo->sets.start_id + fileInfo->sets.count - 1);
    rval = read_sets( ids );
    if (rval != MB_SUCCESS) {
      return error(rval);
    }
  }
  
  dbgOut.tprint(1, "Reading all adjacencies...\n");
  for (i = 0; i < fileInfo->num_elem_desc; ++i) {
    if (!fileInfo->elems[i].have_adj)
      continue;
    
    long table_len;
    hid_t table = mhdf_openAdjacency( filePtr, 
                                      fileInfo->elems[i].handle,
                                      &table_len,
                                      &status );
    if (is_error(status))
      return error(MB_FAILURE);
      
    rval = read_adjacencies( table, table_len );
    mhdf_closeData( filePtr, table, &status );
    if (MB_SUCCESS != rval)
      return error(rval);
    if (is_error(status))
      return error(MB_FAILURE);
  }

  dbgOut.tprint(1, "Reading all tags...\n");
  for (i = 0; i < fileInfo->num_tag_desc; ++i) {
    rval = read_tag( i );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
  dbgOut.tprint(1, "Core read finished.  Cleaning up...\n");
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::find_int_tag( const char* name, int& index )
{
  for (index = 0; index < fileInfo->num_tag_desc; ++index) 
    if (!strcmp( name, fileInfo->tags[index].name))
      break;

  if (index == fileInfo->num_tag_desc) {
    readUtil->report_error( "File does not contain subset tag '%s'", name );
    return error(MB_TAG_NOT_FOUND);
  }

  if (fileInfo->tags[index].type != mhdf_INTEGER ||
      fileInfo->tags[index].size != 1) {
    readUtil->report_error( "Tag '%s' does not containa single integer value", name );
    return error(MB_TYPE_OUT_OF_RANGE);
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::get_subset_ids( const ReaderIface::IDTag* subset_list,
                                      int subset_list_length,
                                      Range& file_ids )
{
  ErrorCode rval;
  
  for (int i = 0; i < subset_list_length; ++i) {  
    
    int tag_index;
    rval = find_int_tag( subset_list[i].tag_name, tag_index );
    if (MB_SUCCESS != rval)
      return error(rval);
  
    Range tmp_file_ids;
    if (!subset_list[i].num_tag_values) {
      rval = get_tagged_entities( tag_index, tmp_file_ids );
    }
    else {
      std::vector<int> ids( subset_list[i].tag_values, 
                            subset_list[i].tag_values + subset_list[i].num_tag_values );
      std::sort( ids.begin(), ids.end() );
      rval = search_tag_values( tag_index, ids, tmp_file_ids );
      if (MB_SUCCESS != rval)
        return error(rval);
    }
    
    if (tmp_file_ids.empty())
      return error(MB_ENTITY_NOT_FOUND);
    
    if (i == 0) 
      file_ids.swap( tmp_file_ids );
    else 
      file_ids = intersect( tmp_file_ids, file_ids );
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::get_partition( Range& tmp_file_ids, int num_parts, int part_number )
{    
     // check that the tag only identified sets
   if ((unsigned long)fileInfo->sets.start_id > tmp_file_ids.front()) {
     dbgOut.print(2,"Ignoreing non-set entities with partition set tag\n");
     tmp_file_ids.erase( tmp_file_ids.begin(), 
                         tmp_file_ids.lower_bound( 
                           (EntityHandle)fileInfo->sets.start_id ) );
   }
   unsigned long set_end = (unsigned long)fileInfo->sets.start_id + fileInfo->sets.count;
   if (tmp_file_ids.back() >= set_end) {
     dbgOut.print(2,"Ignoreing non-set entities with partition set tag\n");
     tmp_file_ids.erase( tmp_file_ids.upper_bound( (EntityHandle)set_end ),
                         tmp_file_ids.end() );
   }
      
  Range::iterator s = tmp_file_ids.begin();
  size_t num_per_proc = tmp_file_ids.size() / num_parts;
  size_t num_extra = tmp_file_ids.size() % num_parts;
  Range::iterator e;
  if (part_number < (long)num_extra) {
    s += (num_per_proc+1) * part_number;
    e = s;
    e += (num_per_proc+1);
  }
  else {
    s += num_per_proc * part_number + num_extra;
    e = s;
    e += num_per_proc;
  }
  tmp_file_ids.erase(e, tmp_file_ids.end());
  tmp_file_ids.erase(tmp_file_ids.begin(), s);

  return MB_SUCCESS;
}


ErrorCode ReadHDF5::load_file_partial( const ReaderIface::IDTag* subset_list,
                                       int subset_list_length,
                                       int num_parts,
                                       int part_number,
                                       const FileOptions& opts )
{
  mhdf_Status status;
  
  for (int i = 0; i < subset_list_length; ++i) {
    dbgOut.printf( 2, "Select by \"%s\" with num_tag_values = %d\n",
                   subset_list[i].tag_name, subset_list[i].num_tag_values );
    if (subset_list[i].num_tag_values) {
      assert(0 != subset_list[i].tag_values);
      dbgOut.printf( 2, "  \"%s\" values = { %d",
        subset_list[i].tag_name, subset_list[i].tag_values[0] );
      for (int j = 1; j < subset_list[i].num_tag_values; ++j)
        dbgOut.printf( 2, ", %d", subset_list[i].tag_values[j] );
      dbgOut.printf(2," }\n");
    }
  }
  if (num_parts) 
    dbgOut.printf( 2, "Partition with num_parts = %d and part_number = %d\n", 
                   num_parts, part_number );
  
  dbgOut.tprint( 1, "RETREIVING TAGGED ENTITIES\n" );
    
  Range file_ids;
  ErrorCode rval = get_subset_ids( subset_list, subset_list_length, file_ids );
  if (MB_SUCCESS != rval)
    return error(rval);
  
  if (num_parts) {
    rval = get_partition( file_ids, num_parts, part_number );
    if (MB_SUCCESS != rval)
      return error(rval);
  }

  dbgOut.print_ints( 4, "Set file IDs for partial read: ", file_ids );
  
  dbgOut.tprint( 1, "GATHERING ADDITIONAL ENTITIES\n" );
  
  const char* const set_opts[] = { "NONE", "SETS", "CONTENTS" };
  int child_mode;
  rval = opts.match_option( "CHILDREN", set_opts, child_mode );
  if (MB_ENTITY_NOT_FOUND == rval)
    child_mode = 2;
  else if (MB_SUCCESS != rval) {
    readUtil->report_error( "Invalid value for 'CHILDREN' option" );
    return error(rval);
  }
  int content_mode;
  rval = opts.match_option( "SETS", set_opts, content_mode );
  if (MB_ENTITY_NOT_FOUND == rval)
    content_mode = 2;
  else if (MB_SUCCESS != rval) {
    readUtil->report_error( "Invalid value for 'SETS' option" );
    return error(rval);
  }
  
    // If we want the contents of contained/child sets, 
    // search for them now (before gathering the non-set contents
    // of the sets.)
  Range sets;
  intersect( fileInfo->sets, file_ids, sets );
  if (content_mode == 2 || child_mode == 2) {
    rval = read_set_ids_recursive( sets, content_mode == 2, child_mode == 2 );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
  debug_barrier();
  
    // get elements and vertices contained in sets
  rval = get_set_contents( sets, file_ids );
  if (MB_SUCCESS != rval)
    return error(rval);

  dbgOut.print_ints( 5, "File IDs for partial read: ", file_ids );
  debug_barrier();
    
  dbgOut.tprint( 1, "GATHERING NODE IDS\n" );
  
    // Figure out the maximum dimension of entity to be read
  int max_dim = 0;
  for (int i = 0; i < fileInfo->num_elem_desc; ++i) {
    EntityType type = CN::EntityTypeFromName( fileInfo->elems[i].type );
    if (type <= MBVERTEX || type >= MBENTITYSET) {
      assert( false ); // for debug code die for unknown element tyoes
      continue; // for release code, skip unknown element types
    }
    int dim = CN::Dimension(type);
    if (dim > max_dim) {
      Range subset;
      intersect( fileInfo->elems[i].desc, file_ids, subset );
      if (!subset.empty())
        max_dim = dim;
    }
  }
#ifdef USE_MPI
  if (nativeParallel) {
    int send = max_dim;
    MPI_Allreduce( &send, &max_dim, 1, MPI_INT, MPI_MAX, *mpiComm );
  }
#endif
  
    // if input contained any polyhedra, then need to get faces
    // of the polyhedra before the next loop because we need to 
    // read said faces in that loop.
  for (int i = 0; i < fileInfo->num_elem_desc; ++i) {
    EntityType type = CN::EntityTypeFromName( fileInfo->elems[i].type );
    if (type != MBPOLYHEDRON)
      continue;
    
    debug_barrier();
    dbgOut.print( 2, "    Getting polyhedra faces\n" );
    
    Range polyhedra;
    intersect( fileInfo->elems[i].desc, file_ids, polyhedra );
    rval = read_elems( i, polyhedra, &file_ids );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
    // get node file ids for all elements
  Range nodes;
  intersect( fileInfo->nodes, file_ids, nodes );
  for (int i = 0; i < fileInfo->num_elem_desc; ++i) {
    EntityType type = CN::EntityTypeFromName( fileInfo->elems[i].type );
    if (type <= MBVERTEX || type >= MBENTITYSET) {
      assert( false ); // for debug code die for unknown element tyoes
      continue; // for release code, skip unknown element types
    }
    if (MBPOLYHEDRON == type)
      continue;
      
    debug_barrier();
    dbgOut.printf( 2, "    Getting element node IDs for: %s\n", fileInfo->elems[i].handle );
    
    Range subset;
    intersect( fileInfo->elems[i].desc, file_ids, subset );
    
      // If dimension is max_dim, then we can create the elements now
      // so we don't have to read the table again later (connectivity 
      // will be fixed up after nodes are created when update_connectivity())
      // is called.  For elements of a smaller dimension, we just build
      // the node ID range now because a) we'll have to read the whole 
      // connectivity table again later, and b) we don't want to worry
      // about accidentally creating multiple copies of the same element.
    if (CN::Dimension(type) == max_dim)
      rval = read_elems( i, subset, &nodes );
    else
      rval = read_elems( i, subset, nodes );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
    
  debug_barrier();
  dbgOut.tprint( 1, "READING NODE COORDINATES\n" );
  
    // Read node coordinates and create vertices in MOAB
    // NOTE:  This populates the RangeMap with node file ids,
    //        which is expected by read_node_adj_elems.
  rval = read_nodes( nodes );
  if (MB_SUCCESS != rval)
    return error(rval);
 
  debug_barrier();
  dbgOut.tprint( 1, "READING ELEMENTS\n" );
 
    // decide if we need to read additional elements
  int side_mode;
  const char* const options[] = { "EXPLICIT", "NODES", "SIDES", 0 };
  rval = opts.match_option( "ELEMENTS", options, side_mode );
  if (MB_ENTITY_NOT_FOUND == rval) {
      // If only nodes were specified, then default to "NODES", otherwise
      // default to "SIDES".
    if (0 == max_dim)
      side_mode = 1;
    else
      side_mode = 2;
  }
  else if (MB_SUCCESS != rval) {
    readUtil->report_error( "Invalid value for 'ELEMENTS' option" );
    return error(rval);
  }
  
  if (side_mode == 2 /*ELEMENTS=SIDES*/ && max_dim == 0 /*node-based*/) {
      // Read elements until we find something.  Once we find someting,
      // read only elements of the same dimension.  NOTE: loop termination
      // criterion changes on both sides (max_dim can be changed in loop
      // body).
    for (int dim = 3; dim >= max_dim; --dim) {
      for (int i = 0; i < fileInfo->num_elem_desc; ++i) {
        EntityType type = CN::EntityTypeFromName( fileInfo->elems[i].type );
        if (CN::Dimension(type) == dim) {
          debug_barrier();
          dbgOut.tprintf( 2, "    Reading node-adjacent elements for: %s\n", fileInfo->elems[i].handle );
          Range ents;
          rval = read_node_adj_elems( fileInfo->elems[i] );
          if (MB_SUCCESS != rval)
            return error(rval);
          if (!ents.empty())
            max_dim = 3;
        }
      }
    }
  }

  Range side_entities;
  if (side_mode != 0 /*ELEMENTS=NODES || ELEMENTS=SIDES*/) {
    if (0 == max_dim)
      max_dim = 4;
      // now read any additional elements for which we've already read all
      // of the nodes.
    for (int dim = max_dim - 1; dim > 0; --dim) {
      for (int i = 0; i < fileInfo->num_elem_desc; ++i) {
        EntityType type = CN::EntityTypeFromName( fileInfo->elems[i].type );
        if (CN::Dimension(type) == dim) {
          debug_barrier();
          dbgOut.tprintf( 2, "    Reading node-adjacent elements for: %s\n", fileInfo->elems[i].handle );
          rval = read_node_adj_elems( fileInfo->elems[i], &side_entities );
          if (MB_SUCCESS != rval)
            return error(rval);
        }
      }
    }
  }

    // We need to do this here for polyhedra to be handled coorectly.
    // We have to wait until the faces are read in the above code block,
    // but need to create the connectivity before doing update_connectivity, 
    // which might otherwise delete polyhedra faces.
  debug_barrier();
  dbgOut.tprint( 1, "UPDATING CONNECTIVITY ARRAYS FOR READ ELEMENTS\n" );
  rval = update_connectivity();
  if (MB_SUCCESS != rval)
    return error(rval);

  
  dbgOut.tprint( 1, "READING ADJACENCIES\n" );
  for (int i = 0; i < fileInfo->num_elem_desc; ++i) {
    if (fileInfo->elems[i].have_adj &&
        idMap.intersects( fileInfo->elems[i].desc.start_id, fileInfo->elems[i].desc.count )) {
      long len;
      hid_t th = mhdf_openAdjacency( filePtr, fileInfo->elems[i].handle, &len, &status );
      if (is_error(status))
        return error(MB_FAILURE);

      rval = read_adjacencies( th, len );
      mhdf_closeData( filePtr, th, &status );
      if (MB_SUCCESS != rval)
        return error(rval);
    }
  }
  
    // If doing ELEMENTS=SIDES then we need to delete any entities
    // that we read that aren't actually sides (e.g. an interior face
    // that connects two disjoint portions of the part).  Both
    // update_connectivity and reading of any explicit adjacencies must
    // happen before this.
  if (side_mode == 2) {
    debug_barrier();
    dbgOut.tprint( 1, "CHECKING FOR AND DELETING NON-SIDE ELEMENTS\n" );
    rval = delete_non_side_elements( side_entities );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
  debug_barrier();
  dbgOut.tprint( 1, "READING SETS\n" );
    
    // If reading contained/child sets but not their contents then find
    // them now. If we were also reading their contents we would
    // have found them already.
  if (content_mode == 1 || child_mode == 1) {
    rval = read_set_ids_recursive( sets, content_mode != 0, child_mode != 0 );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
    // Append file IDs of sets containing any of the nodes or elements
    // we've read up to this point.
  rval = find_sets_containing( sets );
  if (MB_SUCCESS != rval)
    return error(rval);
    // Now actually read all set data and instantiate sets in MOAB.
    // Get any contained sets out of file_ids.
  EntityHandle first_set = fileInfo->sets.start_id;
  sets.merge( file_ids.lower_bound( first_set ),
              file_ids.lower_bound( first_set + fileInfo->sets.count ) );
  rval = read_sets( sets );
  if (MB_SUCCESS != rval)
    return error(rval);
  
  dbgOut.tprint( 1, "READING TAGS\n" );
  
  for (int i = 0; i < fileInfo->num_tag_desc; ++i) {
    rval = read_tag( i );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
  dbgOut.tprint( 1, "PARTIAL READ COMPLETE.\n" );
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::search_tag_values( int tag_index,
                                       const std::vector<int>& sorted_values,
                                       Range& file_ids,
                                       bool sets_only )
{
  ErrorCode rval;
  mhdf_Status status;
  std::vector<EntityHandle>::iterator iter;
  const mhdf_TagDesc& tag = fileInfo->tags[tag_index];
  long size;
  long start_id;

  debug_barrier();
   
    // do dense data
    
  hid_t table;
  const char* name;
  std::vector<EntityHandle> indices;
    // These are probably in order of dimension, so iterate
    // in reverse order to make Range insertions more efficient.
  std::vector<int> grp_indices( tag.dense_elem_indices, tag.dense_elem_indices+tag.num_dense_indices );
  for (std::vector<int>::reverse_iterator i = grp_indices.rbegin(); i != grp_indices.rend(); ++i)
  {
    int idx = *i;
    if (idx == -2) {
      name = mhdf_set_type_handle();
      start_id = fileInfo->sets.start_id;
    }
    else if (sets_only) {
      continue;
    }
    else if (idx == -1) {
      name = mhdf_node_type_handle();
     start_id = fileInfo->nodes.start_id;
    }
    else {
      if (idx < 0 || idx >= fileInfo->num_elem_desc) 
        return error(MB_FAILURE);
      name = fileInfo->elems[idx].handle;
      start_id = fileInfo->elems[idx].desc.start_id;
    }
    table = mhdf_openDenseTagData( filePtr, tag.name, name, &size, &status );
    if (is_error(status))
      return error(MB_FAILURE);
    rval = search_tag_values( table, size, sorted_values, indices );
    mhdf_closeData( filePtr, table, &status );
    if (MB_SUCCESS != rval || is_error(status))
      return error(MB_FAILURE);
      // Convert from table indices to file IDs and add to result list
    std::sort( indices.begin(), indices.end(), std::greater<EntityHandle>() );
    std::transform( indices.begin(), indices.end(), range_inserter(file_ids),
                    std::bind1st( std::plus<long>(), start_id ) );
    indices.clear();
  }
  
  if (!tag.have_sparse)
    return MB_SUCCESS;
  
    // do sparse data
    
  hid_t tables[2]; 
  long junk; // redundant value for non-variable-length tags
  mhdf_openSparseTagData( filePtr, tag.name, &size, &junk, tables, &status );
  if (is_error(status))
    return error(MB_FAILURE);
  rval = search_tag_values( tables[1], size, sorted_values, indices );
  mhdf_closeData( filePtr, tables[1], &status );
  if (MB_SUCCESS != rval || is_error(status)) {
    mhdf_closeData( filePtr, tables[0], &status );
    return error(MB_FAILURE);
  }
    // convert to ranges
  std::sort( indices.begin(), indices.end() );
  std::vector<EntityHandle> ranges;
  iter = indices.begin();
  while (iter != indices.end()) {
    ranges.push_back( *iter );
    EntityHandle last = *iter;
    for (++iter; iter != indices.end() && (last + 1) == *iter; ++iter, ++last);
    ranges.push_back( last );
  }
    // read file ids
  iter = ranges.begin();
  unsigned long offset = 0;
  while (iter != ranges.end()) {
    long begin = *iter; ++iter;
    long end   = *iter; ++iter;
    mhdf_readSparseTagEntitiesWithOpt( tables[0], begin, end - begin + 1, 
                                handleType, &indices[offset], indepIO, &status );
    if (is_error(status)) {
      mhdf_closeData( filePtr, tables[0], &status );
      return error(MB_FAILURE);
    }
    offset += end - begin + 1;
  }
  mhdf_closeData( filePtr, tables[0], &status );
  if (is_error(status))
    return error(MB_FAILURE);
  assert( offset == indices.size() );
  std::sort( indices.begin(), indices.end() );
  
  if (sets_only) {
    iter = std::lower_bound( indices.begin(), indices.end(), 
                             fileInfo->sets.start_id + fileInfo->sets.count );
    indices.erase( iter, indices.end() );
    iter = std::lower_bound( indices.begin(), indices.end(), 
                             fileInfo->sets.start_id );
    indices.erase( indices.begin(), iter );
  }
  copy_sorted_file_ids( &indices[0], indices.size(), file_ids );
  
  return MB_SUCCESS;  
}

ErrorCode ReadHDF5::get_tagged_entities( int tag_index, Range& file_ids )
{
  const mhdf_TagDesc& tag = fileInfo->tags[tag_index];
   
    // do dense data
  Range::iterator hint = file_ids.begin();
  for (int i = 0; i < tag.num_dense_indices; ++i)
  {
    int idx = tag.dense_elem_indices[i];
    mhdf_EntDesc* ents;
    if (idx == -2)
      ents = &fileInfo->sets;
    else if (idx == -1) 
      ents = &fileInfo->nodes;
    else {
      if (idx < 0 || idx >= fileInfo->num_elem_desc) 
        return error(MB_FAILURE);
      ents = &(fileInfo->elems[idx].desc);
    }
    
    EntityHandle h = (EntityHandle)ents->start_id;
    hint = file_ids.insert( hint, h, h + ents->count - 1 );
  }
  
  if (!tag.have_sparse)
    return MB_SUCCESS;
  
    // do sparse data
    
  mhdf_Status status;
  hid_t tables[2]; 
  long size, junk; 
  mhdf_openSparseTagData( filePtr, tag.name, &size, &junk, tables, &status );
  if (is_error(status))
    return error(MB_FAILURE);
  mhdf_closeData( filePtr, tables[1], &status );
  if (is_error(status)) {
    mhdf_closeData( filePtr, tables[0], &status );
    return error(MB_FAILURE);
  }
  
  hint = file_ids.begin();
  EntityHandle* buffer = reinterpret_cast<EntityHandle*>(dataBuffer);
  const long buffer_size = bufferSize / sizeof(EntityHandle);
  long remaining = size, offset = 0;
  while (remaining) {
    long count = std::min( buffer_size, remaining );
    assert_range( buffer, count );
    mhdf_readSparseTagEntitiesWithOpt( *tables, offset, count, 
                                handleType, buffer, collIO, &status );
    if (is_error(status)) {
      mhdf_closeData( filePtr, *tables, &status );
      return error(MB_FAILURE);
    }
    
    std::sort( buffer, buffer + count );
    for (long i = 0; i < count; ++i)
      hint = file_ids.insert( hint, buffer[i], buffer[i] );
    
    remaining -= count;
    offset += count;
  }

  mhdf_closeData( filePtr, *tables, &status );
  if (is_error(status))
    return error(MB_FAILURE);
  
  return MB_SUCCESS;  
}

ErrorCode ReadHDF5::search_tag_values( hid_t tag_table, 
                                       unsigned long table_size,
                                       const std::vector<int>& sorted_values,
                                       std::vector<EntityHandle>& value_indices )
{

  debug_barrier();

  mhdf_Status status;
  size_t chunk_size = bufferSize / sizeof(unsigned);
  unsigned * buffer = reinterpret_cast<unsigned*>(dataBuffer);
  size_t remaining = table_size, offset = 0;
  while (remaining) {
      // Get a block of tag values
    size_t count = std::min( chunk_size, remaining );
    assert_range( buffer, count );
    mhdf_readTagValuesWithOpt( tag_table, offset, count, H5T_NATIVE_UINT, buffer, collIO, &status );
    if (is_error(status))
      return error(MB_FAILURE);
    
      // search tag values
    for (size_t i = 0; i < count; ++i)
      if (std::binary_search( sorted_values.begin(), sorted_values.end(), buffer[i] ))
        value_indices.push_back( i + offset );
    
    offset += count;
    remaining -= count;
  }

  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_nodes( const Range& node_file_ids )
{
  ErrorCode rval;
  mhdf_Status status;
  const int dim = fileInfo->nodes.vals_per_ent;
  Range range;
  
  if (node_file_ids.empty())
    return MB_SUCCESS;
  
  int cdim;
  rval = iFace->get_dimension( cdim );
  if (MB_SUCCESS != rval)
    return error(rval);
  
  if (cdim < dim)
  {
    rval = iFace->set_dimension( dim );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
  hid_t data_id = mhdf_openNodeCoordsSimple( filePtr, &status );
  if (is_error(status))
    return error(MB_FAILURE);

  EntityHandle handle;
  std::vector<double*> arrays(dim);
  const size_t num_nodes = node_file_ids.size();
  rval = readUtil->get_node_coords( dim, (int)num_nodes, 0, handle, arrays );
  if (MB_SUCCESS != rval)
  {
    mhdf_closeData( filePtr, data_id, &status );
    return error(rval);
  }

#ifdef BLOCKED_COORD_IO
  try {
    for (int d = 0; d < dim; ++d) {
      ReadHDF5Dataset reader( "blocked coords", data_id, nativeParallel, mpiComm, false );
      reader.set_column( d );
      reader.set_file_ids( node_file_ids, fileInfo->nodes.start_id, num_nodes, H5T_NATIVE_DOUBLE );
      dbgOut.printf( 3, "Reading %lu chunks for coordinate dimension %d\n", reader.get_read_count(), d );
      // should normally only have one read call, unless sparse nature
      // of file_ids caused reader to do something strange
      size_t count, offset = 0;
      int nn = 0;
      while (!reader.done()) {
        dbgOut.printf(3,"Reading chunk %d for dimension %d\n", ++nn, d );
        reader.read( arrays[d]+offset, count );
        offset += count;
      }
      if (offset != num_nodes) {
        mhdf_closeData( filePtr, data_id, &status );
        assert(false);
        return MB_FAILURE;
      }
    }
  }
  catch (ReadHDF5Dataset::Exception) {
    mhdf_closeData( filePtr, data_id, &status );
    return error(MB_FAILURE);
  }
#else
  double* buffer = (double*)dataBuffer;
  long chunk_size = bufferSize / (3*sizeof(double));
  long coffset = 0;
  int nn = 0;
  try {
    ReadHDF5Dataset reader( "interleaved coords", data_id, nativeParallel, mpiComm, false );
    reader.set_file_ids( node_file_ids, fileInfo->nodes.start_id, chunk_size, H5T_NATIVE_DOUBLE );
    dbgOut.printf( 3, "Reading %lu chunks for coordinate coordinates\n", reader.get_read_count() );
    while (!reader.done()) {
      dbgOut.tprintf(3,"Reading chunk %d of node coords\n", ++nn);
      
      size_t count;
      reader.read( buffer, count );
      
      for (size_t i = 0; i < count; ++i)
        for (int d = 0; d < dim; ++d) 
          arrays[d][coffset+i] = buffer[dim*i+d];
      coffset += count;
    }
  }
  catch (ReadHDF5Dataset::Exception) {
    mhdf_closeData( filePtr, data_id, &status );
    return error(MB_FAILURE);
  }
#endif
  dbgOut.print(3,"Closing node coordinate table\n");
  mhdf_closeData( filePtr, data_id, &status );
  for (int d = dim; d < cdim; ++d)
    memset( arrays[d], 0, num_nodes*sizeof(double) );
    
  dbgOut.printf(3,"Updating ID to handle map for %lu nodes\n", (unsigned long)node_file_ids.size());
  return insert_in_id_map( node_file_ids, handle );
}

ErrorCode ReadHDF5::read_elems( int i )
{
  Range ids;
  ids.insert( fileInfo->elems[i].desc.start_id,
              fileInfo->elems[i].desc.start_id + fileInfo->elems[i].desc.count - 1);
  return read_elems( i, ids );
}

ErrorCode ReadHDF5::read_elems( int i, const Range& file_ids, Range* node_ids )
{
  if (fileInfo->elems[i].desc.vals_per_ent < 0) {
    if (node_ids != 0) // not implemented for version 3 format of poly data
      return error(MB_TYPE_OUT_OF_RANGE);
    return read_poly( fileInfo->elems[i], file_ids );
  }
  else
    return read_elems( fileInfo->elems[i], file_ids, node_ids );
}

ErrorCode ReadHDF5::read_elems( const mhdf_ElemDesc& elems, const Range& file_ids, Range* node_ids )
{

  debug_barrier();

  ErrorCode rval = MB_SUCCESS;
  mhdf_Status status;
  
  EntityType type = CN::EntityTypeFromName( elems.type );
  if (type == MBMAXTYPE)
  {
    readUtil->report_error( "Unknown element type: \"%s\".\n", elems.type );
    return error(MB_FAILURE);
  }
  
  const int nodes_per_elem = elems.desc.vals_per_ent;
  const size_t count = file_ids.size();
  hid_t data_id = mhdf_openConnectivitySimple( filePtr, elems.handle, &status );
  if (is_error(status))
    return error(MB_FAILURE);

  EntityHandle handle;
  EntityHandle* array = 0;
  rval = readUtil->get_element_connect( count, nodes_per_elem, type,
                                        0, handle, array );
  if (MB_SUCCESS != rval)
    return error(rval);
  
  try {
    EntityHandle* buffer = reinterpret_cast<EntityHandle*>(dataBuffer);
    const size_t buffer_size = bufferSize/(sizeof(EntityHandle)*nodes_per_elem);
    ReadHDF5Dataset reader( elems.handle, data_id, nativeParallel, mpiComm );
    reader.set_file_ids( file_ids, elems.desc.start_id, buffer_size, handleType ); 
    dbgOut.printf( 3, "Reading connectivity in %lu chunks for element group \"%s\"\n",
      reader.get_read_count(), elems.handle );
    EntityHandle* iter = array;
    int nn = 0;
    while (!reader.done()) {
      dbgOut.printf( 3, "Reading chunk %d for \"%s\"\n", ++nn, elems.handle );
    
      size_t num_read;
      reader.read( buffer, num_read );
      iter = std::copy( buffer, buffer+num_read*nodes_per_elem, iter );
      
      if (node_ids) {
        std::sort( buffer, buffer + num_read*nodes_per_elem );
        num_read = std::unique( buffer, buffer + num_read*nodes_per_elem ) - buffer;
        copy_sorted_file_ids( buffer, num_read, *node_ids );
      }
    }
    assert(iter - array == (ptrdiff_t)count * nodes_per_elem);
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
  
  if (!node_ids) {
    rval = convert_id_to_handle( array, count*nodes_per_elem );
    if (MB_SUCCESS != rval)
      return error(rval);

    rval = readUtil->update_adjacencies( handle, count, nodes_per_elem, array );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  else {
    IDConnectivity t;
    t.handle = handle;
    t.count = count;
    t.nodes_per_elem = nodes_per_elem;
    t.array = array;
    idConnectivityList.push_back(t);
  }
  
  return insert_in_id_map( file_ids, handle );
}

ErrorCode ReadHDF5::update_connectivity()
{
  ErrorCode rval;
  std::vector<IDConnectivity>::iterator i;
  for (i = idConnectivityList.begin(); i != idConnectivityList.end(); ++i) {
    rval = convert_id_to_handle( i->array, i->count * i->nodes_per_elem );
    if (MB_SUCCESS != rval)
      return error(rval);
    
    rval = readUtil->update_adjacencies( i->handle, i->count, i->nodes_per_elem, i->array );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  idConnectivityList.clear();
  return MB_SUCCESS;    
}

ErrorCode ReadHDF5::read_node_adj_elems( const mhdf_ElemDesc& group, Range* handles_out )
{
  mhdf_Status status;
  ErrorCode rval;
  
  hid_t table = mhdf_openConnectivitySimple( filePtr, group.handle, &status );
  if (is_error(status))
    return error(MB_FAILURE);
    
  rval = read_node_adj_elems( group, table, handles_out );
  
  mhdf_closeData( filePtr, table, &status );
  if (MB_SUCCESS == rval && is_error(status))
    return error(rval = MB_FAILURE);
  return rval;
}

ErrorCode ReadHDF5::read_node_adj_elems( const mhdf_ElemDesc& group, 
                                         hid_t table_handle,
                                         Range* handles_out )
{

  debug_barrier();

  mhdf_Status status;
  ErrorCode rval;
  IODebugTrack debug_track( debugTrack, std::string(group.handle) );

    // copy data to local variables (makes other code clearer)
  const int node_per_elem = group.desc.vals_per_ent;
  long start_id = group.desc.start_id;
  long remaining = group.desc.count;
  const EntityType type = CN::EntityTypeFromName( group.type );
  
    // figure out how many elements we can read in each pass
  long* const buffer = reinterpret_cast<long*>( dataBuffer );
  const long buffer_size = bufferSize / (node_per_elem * sizeof(buffer[0]));
    // read all element connectivity in buffer_size blocks
  long offset = 0;
  dbgOut.printf( 3, "Reading node-adjacent elements from \"%s\" in %ld chunks\n",
    group.handle, (remaining + buffer_size - 1) / buffer_size );
  int nn = 0;
  Range::iterator hint;
  if (handles_out)
    hint = handles_out->begin();
  while (remaining) {
    dbgOut.printf( 3, "Reading chunk %d of connectivity data for \"%s\"\n", ++nn, group.handle );
  
      // read a block of connectivity data
    const long count = std::min( remaining, buffer_size );
    debug_track.record_io( offset, count );
    assert_range( buffer, count*node_per_elem );
    mhdf_readConnectivityWithOpt( table_handle, offset, count, H5T_NATIVE_LONG, buffer, collIO, &status );
    if (is_error(status))
      return error(MB_FAILURE);
    offset += count;
    remaining -= count;
    
      // count the number of elements in the block that we want,
      // zero connectivity for other elements
    long num_elem = 0;
    long* iter = buffer;
    for (long i = 0; i < count; ++i) {
      for (int j = 0; j < node_per_elem; ++j) {
        iter[j] = (long)idMap.find( iter[j] );
        if (!iter[j]) {
          iter[0] = 0;
          break;
        }
      }
      if (iter[0])
        ++num_elem;
      iter += node_per_elem;
    }
    
    if (!num_elem) {
      start_id += count;
      continue;
    }
    
      // create elements
    EntityHandle handle;
    EntityHandle* array;
    rval = readUtil->get_element_connect( (int)num_elem,
                                         node_per_elem,
                                         type,
                                         0,
                                         handle, 
                                         array );
    if (MB_SUCCESS != rval)
      return error(rval);
   
      // copy all non-zero connectivity values
    iter = buffer;
    EntityHandle* iter2 = array;
    EntityHandle h = handle;
    for (long i = 0; i < count; ++i) {
      if (!*iter) {
        iter += node_per_elem;
        continue;
      }
      if (!idMap.insert( start_id + i, h++, 1 ).second) 
        return error(MB_FAILURE);
        
      long* const end = iter + node_per_elem;
      for (; iter != end; ++iter, ++iter2)
        *iter2 = (EntityHandle)*iter;
    }
    assert( iter2 - array == num_elem * node_per_elem );
    start_id += count;
    
    rval = readUtil->update_adjacencies( handle, num_elem, node_per_elem, array );
    if (MB_SUCCESS != rval) return error(rval);
    if (handles_out)
      hint = handles_out->insert( hint, handle, handle + num_elem - 1 );
   }
  
  debug_track.all_reduce();
  return MB_SUCCESS;
}
  

ErrorCode ReadHDF5::read_elems( int i, const Range& elems_in, Range& nodes )
{
  EntityHandle* const buffer = reinterpret_cast<EntityHandle*>(dataBuffer);
  const int node_per_elem = fileInfo->elems[i].desc.vals_per_ent;
  const size_t buffer_size = bufferSize / (node_per_elem*sizeof(EntityHandle));
  
  if (elems_in.empty())
    return MB_SUCCESS;
    
  assert( (long)elems_in.front() >= fileInfo->elems[i].desc.start_id );
  assert( (long)elems_in.back() - fileInfo->elems[i].desc.start_id < fileInfo->elems[i].desc.count );
  
    // we don't support version 3 style poly element data
  if (fileInfo->elems[i].desc.vals_per_ent <= 0)
    return error(MB_TYPE_OUT_OF_RANGE);
  
  mhdf_Status status;
  hid_t table = mhdf_openConnectivitySimple( filePtr, fileInfo->elems[i].handle, &status );
  if (is_error(status))
    return error(MB_FAILURE);
  
  try {
    ReadHDF5Dataset reader( fileInfo->elems[i].handle, table, nativeParallel, mpiComm );
    reader.set_file_ids( elems_in, fileInfo->elems[i].desc.start_id, 
                         buffer_size, handleType );
    dbgOut.printf( 3, "Reading node list in %lu chunks for \"%s\"\n", reader.get_read_count(), fileInfo->elems[i].handle );
    int nn = 0;
    while (!reader.done()) {
      dbgOut.printf( 3, "Reading chunk %d of \"%s\" connectivity\n", ++nn, fileInfo->elems[i].handle );
      size_t num_read;
      reader.read( buffer, num_read );
      std::sort( buffer, buffer + num_read*node_per_elem );
      num_read = std::unique( buffer, buffer + num_read*node_per_elem ) - buffer;
      copy_sorted_file_ids( buffer, num_read, nodes );
    }
  } 
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
  
  return MB_SUCCESS;
}


ErrorCode ReadHDF5::read_poly( const mhdf_ElemDesc& elems, const Range& file_ids )
{
  class PolyReader : public ReadHDF5VarLen {
    private:
      const EntityType type;
      ReadHDF5* readHDF5;
    public:
    PolyReader( EntityType elem_type, void* buffer, size_t buffer_size,
                ReadHDF5* owner, DebugOutput& dbg )
               : ReadHDF5VarLen( dbg, buffer, buffer_size ),
                 type(elem_type), readHDF5(owner) 
               {}
    ErrorCode store_data( EntityHandle file_id, void* data, long len, bool )
    {
      size_t valid;
      EntityHandle* conn = reinterpret_cast<EntityHandle*>(data);
      readHDF5->convert_id_to_handle( conn, len, valid );
      if (valid != (size_t)len)
        return error(MB_ENTITY_NOT_FOUND);
      EntityHandle handle;
      ErrorCode rval = readHDF5->moab()->create_element( type, conn, len, handle );
      if (MB_SUCCESS != rval)
        return error(rval);
      
      rval = readHDF5->insert_in_id_map( file_id, handle );
      return rval;
    }
  };

  debug_barrier();
  
  EntityType type = CN::EntityTypeFromName( elems.type );
  if (type == MBMAXTYPE)
  {
    readUtil->report_error( "Unknown element type: \"%s\".\n", elems.type );
    return error(MB_FAILURE);
  }
  
  hid_t handles[2];
  mhdf_Status status;
  long num_poly, num_conn, first_id;
  mhdf_openPolyConnectivity( filePtr, elems.handle, &num_poly, &num_conn, &first_id, 
                             handles, &status );
  if (is_error(status))
    return error(MB_FAILURE);

  std::string nm(elems.handle);
  ReadHDF5Dataset offset_reader( (nm + " offsets").c_str(), handles[0], nativeParallel, mpiComm, true );
  ReadHDF5Dataset connect_reader( (nm + " data").c_str(), handles[1], nativeParallel, mpiComm, true );
  
  PolyReader tool( type, dataBuffer, bufferSize, this, dbgOut );
  return tool.read( offset_reader, connect_reader, file_ids, first_id, handleType );
}


ErrorCode ReadHDF5::delete_non_side_elements( const Range& side_ents )
{
  ErrorCode rval;

  // build list of entities that we need to find the sides of
  Range explicit_ents;
  Range::iterator hint = explicit_ents.begin();
  for (IDMap::iterator i = idMap.begin(); i != idMap.end(); ++i) {
    EntityHandle start = i->value;
    EntityHandle end = i->value + i->count - 1;
    EntityType type = TYPE_FROM_HANDLE(start);
    assert( type == TYPE_FROM_HANDLE(end) ); // otherwise handle space entirely full!!
    if (type != MBVERTEX && type != MBENTITYSET)
      hint = explicit_ents.insert( hint, start, end );
  }
  explicit_ents = subtract( explicit_ents, side_ents );
  
    // figure out which entities we want to delete
  Range dead_ents( side_ents );
  Range::iterator ds, de, es;
  ds = dead_ents.lower_bound( CN::TypeDimensionMap[1].first );
  de = dead_ents.lower_bound( CN::TypeDimensionMap[2].first, ds );
  if (ds != de) {
    // get subset of explicit ents of dimension greater than 1
    es = explicit_ents.lower_bound( CN::TypeDimensionMap[2].first );
    Range subset, adj;
    subset.insert( es, explicit_ents.end() );
    rval = iFace->get_adjacencies( subset, 1, false, adj, Interface::UNION );
    if (MB_SUCCESS != rval)
      return rval;
    dead_ents = subtract( dead_ents, adj );
  }
  ds = dead_ents.lower_bound( CN::TypeDimensionMap[2].first );
  de = dead_ents.lower_bound( CN::TypeDimensionMap[3].first, ds );
  assert(de == dead_ents.end());
  if (ds != de) {
    // get subset of explicit ents of dimension 3
    es = explicit_ents.lower_bound( CN::TypeDimensionMap[3].first );
    Range subset, adj;
    subset.insert( es, explicit_ents.end() );
    rval = iFace->get_adjacencies( subset, 2, false, adj, Interface::UNION );
    if (MB_SUCCESS != rval)
      return rval;
    dead_ents = subtract( dead_ents, adj );
  }
  
    // now delete anything remaining in dead_ents
  dbgOut.printf( 2, "Deleting %lu elements\n", (unsigned long)dead_ents.size() );
  dbgOut.print( 4, "\tDead entities: ", dead_ents );
  rval = iFace->delete_entities( dead_ents );
  if (MB_SUCCESS != rval)
    return error(rval);
  
    // remove dead entities from ID map
  while (!dead_ents.empty()) {
    EntityHandle start = dead_ents.front();
    EntityID count = dead_ents.const_pair_begin()->second - start + 1;
    IDMap::iterator rit;
    for (rit = idMap.begin(); rit != idMap.end(); ++rit) 
      if (rit->value <= start && (long)(start - rit->value) < rit->count)
        break;
    if (rit == idMap.end())
      return error(MB_FAILURE);
  
    EntityID offset = start - rit->value;
    EntityID avail = rit->count - offset;
    if (avail < count)
      count = avail;
    
    dead_ents.erase( dead_ents.begin(), dead_ents.begin() + count );
    idMap.erase( rit->begin + offset, count );
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_sets( const Range& file_ids )
{

  debug_barrier();

  mhdf_Status status;
  ErrorCode rval;

  if (!fileInfo->sets.count) // If no sets at all!
    return MB_SUCCESS;
  
  hid_t meta_handle = mhdf_openSetMetaSimple( filePtr, &status );
  if (is_error(status))
    return error(MB_FAILURE);

    // create sets 
  Range ranged_set_ids;
  EntityHandle start_handle;
  rval = read_sets( file_ids, meta_handle, ranged_set_ids, start_handle );
  if (MB_SUCCESS != rval) {
    mhdf_closeData( filePtr, meta_handle, &status );
    return error(rval);
  }
    
    // read contents
  if (fileInfo->have_set_contents) {
    long len = 0;
    hid_t handle = mhdf_openSetData( filePtr, &len, &status );
    if (is_error(status)) {
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(MB_FAILURE);
    }
    
    rval = read_contents( file_ids, start_handle, meta_handle, handle, len,
                          ranged_set_ids );
    mhdf_closeData( filePtr, handle, &status );
    if (MB_SUCCESS == rval && is_error(status))
      rval = MB_FAILURE;
    if (MB_SUCCESS != rval) {
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(rval);
    }
  }
  
    // read set child lists
  if (fileInfo->have_set_children) {
    long len = 0;
    hid_t handle = mhdf_openSetChildren( filePtr, &len, &status );
    if (is_error(status)) {
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(MB_FAILURE);
    }
    
    rval = read_children( file_ids, start_handle, meta_handle, handle, len );
    mhdf_closeData( filePtr, handle, &status );
    if (MB_SUCCESS == rval && is_error(status))
      rval = MB_FAILURE;
    if (MB_SUCCESS != rval) {
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(rval);
    }
  }
  
    // read set parent lists
  if (fileInfo->have_set_parents) {
    long len = 0;
    hid_t handle = mhdf_openSetParents( filePtr, &len, &status );
    if (is_error(status)) {
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(MB_FAILURE);
    }
    
    rval = read_parents( file_ids, start_handle, meta_handle, handle, len );
    mhdf_closeData( filePtr, handle, &status );
    if (MB_SUCCESS == rval && is_error(status))
      rval = MB_FAILURE;
    if (MB_SUCCESS != rval) {
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(rval);
    }
  }
    
  mhdf_closeData( filePtr, meta_handle, &status );
  return is_error(status) ? error(MB_FAILURE) : MB_SUCCESS;
}

ErrorCode ReadHDF5::read_set_ids_recursive( Range& sets_in_out,
                                            bool contained_sets,
                                            bool child_sets )
{
  if (!fileInfo->have_set_children)
    child_sets = false;
  if (!fileInfo->have_set_contents)
    contained_sets = false;
  if (!child_sets && !contained_sets)
    return MB_SUCCESS;

    // open data tables
  if (fileInfo->sets.count == 0) {
    assert( sets_in_out.empty() );
    return MB_SUCCESS;
  }
  
  if (!contained_sets && !child_sets)
    return MB_SUCCESS;
  
  hid_t meta_handle, content_handle = 0, child_handle = 0;
  
  mhdf_Status status;
  meta_handle = mhdf_openSetMetaSimple( filePtr, &status );
  if (is_error(status))
    return error(MB_FAILURE);
  
  if (contained_sets) {
    long content_len = 0;
    content_handle = mhdf_openSetData( filePtr, &content_len, &status );
    if (is_error(status)) {
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(MB_FAILURE);
    }
  }
  
  if (child_sets) {
    long child_len = 0;
    child_handle = mhdf_openSetChildren( filePtr, &child_len, &status );
    if (is_error(status)) {
      if (contained_sets)
        mhdf_closeData( filePtr, content_handle, &status );
      mhdf_closeData( filePtr, meta_handle, &status );
      return error(MB_FAILURE);
    }
  }
  
  ErrorCode rval = MB_SUCCESS;
  Range children, new_children(sets_in_out);
  do {
    children.clear();
    if (child_sets) {
      rval = read_child_ids( new_children, meta_handle, child_handle, children );
      if (MB_SUCCESS != rval)
        break;
    }
    if (contained_sets) {
      rval = read_contained_set_ids( new_children, meta_handle, content_handle, children );
      if (MB_SUCCESS != rval)
        break;
    }
    new_children = subtract( children,  sets_in_out );
    dbgOut.print_ints( 2, "Adding additional contained/child sets", new_children );
    sets_in_out.merge( new_children );
  } while (!new_children.empty());
  
  if (child_sets) {
    mhdf_closeData( filePtr, child_handle, &status );
    if (MB_SUCCESS == rval && is_error(status))
      rval = error(MB_FAILURE);
  }
  if (contained_sets) {
    mhdf_closeData( filePtr, content_handle, &status );
    if (MB_SUCCESS == rval && is_error(status))
      rval = error(MB_FAILURE);
  }
  mhdf_closeData( filePtr, meta_handle, &status );
  if (MB_SUCCESS == rval && is_error(status))
    rval = error(MB_FAILURE);
  
  return rval;
}

ErrorCode ReadHDF5::find_sets_containing( Range& sets_out )
{
  ErrorCode rval;
  mhdf_Status status;

  if (!fileInfo->have_set_contents)
    return MB_SUCCESS;
  assert( fileInfo->sets.count );

    // open data tables
  hid_t meta_handle = mhdf_openSetMetaSimple( filePtr, &status );
  if (is_error(status))
    return error(MB_FAILURE);
  long content_len = 0;
  hid_t content_handle = mhdf_openSetData( filePtr, &content_len, &status );
  if (is_error(status)) {
    mhdf_closeData( filePtr, meta_handle, &status );
    return error(MB_FAILURE);
  }

  rval = find_sets_containing( meta_handle, content_handle, content_len, sets_out );

  mhdf_closeData( filePtr, content_handle, &status );
  if(MB_SUCCESS == rval && is_error(status))
    rval = error(MB_FAILURE);
  mhdf_closeData( filePtr, meta_handle, &status );
  if(MB_SUCCESS == rval && is_error(status))
    rval = error(MB_FAILURE);
    
  return rval;
}

static bool set_map_intersect( unsigned short flags,
                               const long* contents,
                               int content_len,
                               const RangeMap<long,EntityHandle>& id_map  )
{
  if (flags & mhdf_SET_RANGE_BIT) {
    if (!content_len || id_map.empty())
      return false;
      
    const long* j = contents;
    const long* const end = contents + content_len;
    assert(content_len % 2 == 0);
    while (j != end) {
      long start = *(j++);
      long count = *(j++);
      if (id_map.intersects( start, count ))
        return true;
    }
  }
  else {
    const long* const end = contents + content_len;
    for (const long* i = contents; i != end; ++i)
      if (id_map.exists( *i ))
        return true;
  }
  return false;
}

ErrorCode ReadHDF5::find_sets_containing( hid_t meta_handle,
                                          hid_t contents_handle, 
                                          long contents_len,
                                          Range& file_ids )
{
  const long avg_set_len = contents_len / fileInfo->sets.count;
  long sets_per_buffer = bufferSize / (sizeof(short) + sizeof(long) * (2+avg_set_len));
    // round to down multiple of 8 to avoid alignment issues
  sets_per_buffer = 8 * (sets_per_buffer / 8);
  if (sets_per_buffer < 10) // just in case there's one huge set
    sets_per_buffer = 10;  
  unsigned short* flag_buffer = (unsigned short*)dataBuffer;
  long* offset_buffer = (long*)(flag_buffer + sets_per_buffer);
  long* content_buffer = offset_buffer + sets_per_buffer;
  assert(bufferSize % sizeof(long) == 0);
  long content_len = (long*)(dataBuffer + bufferSize) - content_buffer;
  assert(dataBuffer + bufferSize >= (char*)(content_buffer + content_len));
    // scan set table  
  mhdf_Status status;
  Range::iterator hint = file_ids.begin();
  long remaining = fileInfo->sets.count;
  long offset = 0;
  long prev_idx = -1;
  int nn = 0;
  dbgOut.printf( 3, "Searching set content\n" ); 
  while (remaining) {
    dbgOut.printf( 3, "Reading chunk %d of set description table\n", ++nn );
  
    long count = std::min( remaining, sets_per_buffer );
    assert_range( flag_buffer, count );
    mhdf_readSetFlagsWithOpt( meta_handle, offset, count, H5T_NATIVE_USHORT, flag_buffer, collIO, &status );
    if (is_error(status)) 
      return error(MB_FAILURE);
    assert_range( offset_buffer, count );
    mhdf_readSetContentEndIndicesWithOpt( meta_handle, offset, count, H5T_NATIVE_LONG, offset_buffer, collIO, &status );
    if (is_error(status))
      return error(MB_FAILURE);
    
    long sets_remaining = count;
    long sets_offset = 0;
    while (sets_remaining) {
      int mm = 0;
        // figure how many of the remaining sets are required to 
        // fill the set contents buffer.
      long sets_count = std::lower_bound( offset_buffer + sets_offset, 
                          offset_buffer + count, content_len + prev_idx )
                          - offset_buffer - sets_offset;
      if (!sets_count) { // contents of single set don't fit in buffer
        long content_remaining = offset_buffer[sets_offset] - prev_idx;
        long content_offset = prev_idx+1;
        while (content_remaining) {
          dbgOut.printf( 3, "Reading chunk %d of set contents table\n", ++mm);
          long content_count = content_len < content_remaining ?
                               2*(content_len/2) : content_remaining;
          assert_range( content_buffer, content_count );
          mhdf_readSetDataWithOpt( contents_handle, content_offset,
                                   content_count, H5T_NATIVE_LONG, 
                                   content_buffer, collIO, &status );
          if (is_error(status))
            return error(MB_FAILURE);
          if (set_map_intersect( flag_buffer[sets_offset],
                                 content_buffer, content_count, idMap )) {
            long id = fileInfo->sets.start_id + offset + sets_offset;
            hint = file_ids.insert( hint, id, id );
            break;
          }
          content_remaining -= content_count;
          content_offset += content_count;
        }
        prev_idx = offset_buffer[sets_offset];
        sets_count = 1;
      }
      else if (long read_num = offset_buffer[sets_offset + sets_count - 1] - prev_idx) {
        assert(sets_count > 0);
        assert_range( content_buffer, read_num );
        dbgOut.printf( 3, "Reading chunk %d of set contents table\n", ++mm);
        mhdf_readSetDataWithOpt( contents_handle, prev_idx+1, read_num, 
                                 H5T_NATIVE_LONG, content_buffer, collIO, &status );
        if (is_error(status))
          return error(MB_FAILURE);
        
        long* buff_iter = content_buffer;
        for (long i = 0; i < sets_count; ++i) {
          long set_size = offset_buffer[i+sets_offset] - prev_idx;
          prev_idx += set_size;
          if (set_map_intersect( flag_buffer[sets_offset+i],
                                 buff_iter, set_size, idMap )) {
            long id = fileInfo->sets.start_id + offset + sets_offset + i;
            hint = file_ids.insert( hint, id, id );
          }
          buff_iter += set_size;
        }
      }
    
      sets_offset += sets_count;
      sets_remaining -= sets_count;
    }
    
    offset += count;
    remaining -= count;
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_child_ids( const Range& input_file_ids,
                                    hid_t meta_handle,
                                    hid_t child_handle,
                                    Range& child_file_ids )
{

  dbgOut.tprintf(2,"Reading child set IDs for %ld ranges\n", (long)input_file_ids.psize());

  mhdf_Status status;
  long* buffer = reinterpret_cast<long*>(dataBuffer);
  long buffer_size = bufferSize / sizeof(long);
  long first, range[2], count, remaining;
  Range sets(input_file_ids);
  Range::iterator hint;
  while (!sets.empty()) {
    count = (long)sets.const_pair_begin()->second - sets.front() + 1;
    first = (long)sets.front() - fileInfo->sets.start_id;
    sets.erase( sets.begin(), sets.begin() + count );
    
    if (!first) {
      range[0] = -1;
      mhdf_readSetChildEndIndicesWithOpt( meta_handle, first+count-1, 1, 
                                          H5T_NATIVE_LONG, range+1, 
                                          indepIO, &status );
      if (is_error(status))
        return error(MB_FAILURE);
    }
    else if (count == 1) {
      mhdf_readSetChildEndIndicesWithOpt( meta_handle, first-1, 2, 
                                          H5T_NATIVE_LONG, range, 
                                          indepIO, &status );
      if (is_error(status))
        return error(MB_FAILURE);
    }
    else {
      mhdf_readSetChildEndIndicesWithOpt( meta_handle, first-1, 1, 
                                          H5T_NATIVE_LONG, range, 
                                          indepIO, &status );
      if (is_error(status))
        return error(MB_FAILURE);
      mhdf_readSetChildEndIndicesWithOpt( meta_handle, first+count-1, 1, 
                                          H5T_NATIVE_LONG, range+1, 
                                          indepIO, &status );
      if (is_error(status))
        return error(MB_FAILURE);
    }
    
    if (range[0] > range[1]) 
      return error(MB_FAILURE);
    remaining = range[1] - range[0];
    long offset = range[0] + 1;
    while (remaining) {
      count = std::min( buffer_size, remaining );
      remaining -= count;
      assert_range( buffer, count );
      mhdf_readSetParentsChildrenWithOpt( child_handle, offset, count, 
                                          H5T_NATIVE_LONG, buffer, 
                                          indepIO, &status );
  
      std::sort( buffer, buffer + count );
      count = std::unique( buffer, buffer + count ) - buffer;
      hint = child_file_ids.begin();
      for (long i = 0; i < count; ++i) {
        EntityHandle h = (EntityHandle)buffer[i];
        hint = child_file_ids.insert( hint, h, h );
      }
    }
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_contained_set_ids( const Range& input_file_ids,
                                            hid_t meta_handle,
                                            hid_t content_handle,
                                            Range& contained_set_file_ids )
{
  mhdf_Status status;
  long buffer_size = bufferSize / (sizeof(long) + sizeof(short));
    // don't want to worry about reading half of a range pair later
  if (buffer_size % 2) --buffer_size;
  long* content_buffer = reinterpret_cast<long*>(dataBuffer);
  unsigned short* flag_buffer = reinterpret_cast<unsigned short*>(content_buffer + buffer_size);
  long first, range[2], count, remaining, sets_offset;

  dbgOut.tprintf(2,"Reading contained set IDs for %ld ranges\n", (long)input_file_ids.psize());

  Range sets(input_file_ids);
  Range::iterator hint;
  while (!sets.empty()) {
    count = (long)sets.const_pair_begin()->second - sets.front() + 1;
    first = (long)sets.front() - fileInfo->sets.start_id;
    if (count > buffer_size)
      count = buffer_size;
    sets.erase( sets.begin(), sets.begin() + count );
    
    assert_range( flag_buffer, count );
    mhdf_readSetFlags( meta_handle, first, count, H5T_NATIVE_USHORT, flag_buffer, &status );
    if (is_error(status))
      return MB_FAILURE;
    
    sets_offset = 0;
    while (sets_offset < count) {
        // Find block of sets with same value for ranged flag
      long start_idx = sets_offset;
      unsigned short ranged = static_cast<unsigned short>(flag_buffer[start_idx] & mhdf_SET_RANGE_BIT);
      for (++sets_offset; sets_offset < count; ++sets_offset)
        if ((flag_buffer[sets_offset] & mhdf_SET_RANGE_BIT) != ranged)
          break;
          
      if (!first && !start_idx) { // first set
        range[0] = -1;
        mhdf_readSetContentEndIndicesWithOpt( meta_handle, first+sets_offset-1, 
                                              1, H5T_NATIVE_LONG, range+1, 
                                              indepIO, &status );
        if (is_error(status))
          return error(MB_FAILURE);
      }
      else if (count == 1) {
        mhdf_readSetContentEndIndicesWithOpt( meta_handle, first+start_idx-1, 
                                              2, H5T_NATIVE_LONG, range, 
                                              indepIO, &status );
        if (is_error(status))
          return error(MB_FAILURE);
      }
      else {
        mhdf_readSetContentEndIndicesWithOpt( meta_handle, first+start_idx-1, 
                                              1, H5T_NATIVE_LONG, range, 
                                              indepIO, &status );
        if (is_error(status))
          return error(MB_FAILURE);
        mhdf_readSetContentEndIndicesWithOpt( meta_handle, first+sets_offset-1, 
                                              1, H5T_NATIVE_LONG, range+1, 
                                              indepIO, &status );
        if (is_error(status))
          return error(MB_FAILURE);
      }
    
      remaining = range[1] - range[0];
      long offset = range[0] + 1;
      while (remaining) {
        assert( !ranged || !(remaining % 2) );
        long content_count = std::min( buffer_size, remaining );
        remaining -= content_count;
        assert_range( content_buffer, content_count );
        mhdf_readSetDataWithOpt( content_handle, offset, content_count, 
                                 H5T_NATIVE_LONG, content_buffer, indepIO, 
                                 &status );
  
        if (ranged) {
          hint = contained_set_file_ids.begin();
          for (long i = 0; i < content_count; i += 2) {
            EntityHandle s = (EntityHandle)content_buffer[i];
            EntityHandle e = s + content_buffer[i+1];
            if ((long)s < fileInfo->sets.start_id)
              s = fileInfo->sets.start_id;
            if ((long)e > fileInfo->sets.start_id + fileInfo->sets.count)
              e = fileInfo->sets.start_id + fileInfo->sets.count;
            if (s < e) 
              hint = contained_set_file_ids.insert( hint, s, e - 1 );
          }
        }
        else {
          std::sort( content_buffer, content_buffer + content_count );
          long* s = std::lower_bound( content_buffer, content_buffer + content_count,
                                      fileInfo->sets.start_id );
          long* e = std::lower_bound( s, content_buffer + content_count, 
                                      fileInfo->sets.start_id + fileInfo->sets.count );
          e = std::unique( s, e );
          hint = contained_set_file_ids.begin();
          for ( ; s != e; ++s) {
            EntityHandle h = *s;
            hint = contained_set_file_ids.insert( hint, h, h );
          }
        }
      }
    }
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_sets( const Range& file_ids,
                               hid_t meta_handle, 
                               Range& ranged_file_ids,
                               EntityHandle& start_handle,
                               bool create )
{
  ErrorCode rval;

  size_t num_sets = file_ids.size();

  std::vector<unsigned> flags;  
  unsigned* buffer = reinterpret_cast<unsigned*>(dataBuffer);
  try {
    ReadHDF5Dataset reader( "set flags", meta_handle, nativeParallel, mpiComm, false );
    reader.set_column( reader.columns() - 1 );
    reader.set_file_ids( file_ids, fileInfo->sets.start_id, bufferSize/sizeof(unsigned), H5T_NATIVE_UINT );
    dbgOut.printf( 3, "Reading set flags in %lu chunks\n", reader.get_read_count() );
    // should normally only have one read call, unless sparse nature
    // of file_ids caused reader to do something strange
    int nn = 0;
    dbgOut.printf( 3, "Reading chunk %d of set flags\n", ++nn );
    size_t count;
    reader.read( buffer, count );
    if (count != num_sets) {
      flags.insert( flags.end(), buffer, buffer+count );
    }
    while (!reader.done()) {
      dbgOut.printf( 3, "Reading chunk %d of set flags\n", ++nn );
      reader.read( buffer, count );
      flags.insert( flags.end(), buffer, buffer+count );
    }
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
  
  if (!flags.empty())
    buffer = &flags[0];
  unsigned* buff_iter = buffer;
  Range::iterator hint = ranged_file_ids.begin();
  for (Range::iterator i = file_ids.begin(); i != file_ids.end(); ++i, ++buff_iter) {
    if ((*buff_iter) & mhdf_SET_RANGE_BIT) {
      *buff_iter &= ~(unsigned)mhdf_SET_RANGE_BIT;
      hint = ranged_file_ids.insert( hint, *i, *i );
    }
  }
  
  if (create) {
    rval = readUtil->create_entity_sets( num_sets, buffer, 0, start_handle );
    if (MB_SUCCESS != rval)
      return error(rval);
    
    rval = insert_in_id_map( file_ids, start_handle );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  
  return MB_SUCCESS;
}


ErrorCode ReadHDF5::read_contents( const Range& set_file_ids,
                                   EntityHandle start_handle,
                                   hid_t set_meta_data_table,
                                   hid_t set_contents_table,
                                   long /*set_contents_length*/,
                                   const Range& ranged_set_file_ids )
{

  class ReadSetContents : public ReadHDF5VarLen {
    ReadHDF5* readHDF5;
    EntityHandle startHandle;
  public:
    ReadSetContents( DebugOutput& dbg_out,
                     void* buffer,
                     size_t buffer_size,
                     EntityHandle start_handle,
                     ReadHDF5* moab )
                     : ReadHDF5VarLen(dbg_out, buffer, buffer_size ),
                       readHDF5(moab),
                       startHandle(start_handle)
                    {}
    ErrorCode store_data( EntityHandle file_id, void* data, long len, bool ranged ) 
    {
      EntityHandle h = startHandle++;
#ifndef NDEBUG
      size_t ok = 0;
      EntityHandle h2 = file_id;
      readHDF5->convert_id_to_handle( &h2, 1, ok );
      assert(ok && h2 == h);
#endif

      EntityHandle* array = reinterpret_cast<EntityHandle*>(data);
      if (ranged) {
        if (len % 2) 
          return error(MB_INDEX_OUT_OF_RANGE);
        Range range;
        readHDF5->convert_range_to_handle( array, len/2, range );
        return readHDF5->moab()->add_entities( h, range );
      }
      else {
        if (!len)
          return MB_SUCCESS;
        size_t valid;
        readHDF5->convert_id_to_handle( array, len, valid );
        return readHDF5->moab()->add_entities( h, array, valid );
      }
    }
  };
  
  dbgOut.print(1,"Reading set contents\n");
  try {
    ReadHDF5Dataset met_dat( "set content offsets", set_meta_data_table, nativeParallel, mpiComm, false );
    met_dat.set_column( 0 );
    ReadHDF5Dataset cnt_dat( "set contents", set_contents_table, nativeParallel, mpiComm, false );
    ReadSetContents tool( dbgOut, dataBuffer, bufferSize, start_handle, this );
    return tool.read( met_dat, cnt_dat, 
                      set_file_ids, fileInfo->sets.start_id, 
                      handleType,
                      &ranged_set_file_ids );
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
}


ErrorCode ReadHDF5::read_children( const Range& set_file_ids,
                                   EntityHandle start_handle,
                                   hid_t set_meta_data_table,
                                   hid_t set_contents_table,
                                   long /*set_contents_length*/ )
{
  class ReadSetChildren : public ReadHDF5VarLen {
    ReadHDF5* readHDF5;
    EntityHandle startHandle;
  public:
    ReadSetChildren( DebugOutput& dbg_out,
                    void* buffer,
                    size_t buffer_size,
                    EntityHandle start_handle,
                    ReadHDF5* moab )
                    : ReadHDF5VarLen(dbg_out, buffer, buffer_size ),
                      readHDF5(moab),
                      startHandle(start_handle)
                    {}
    ErrorCode store_data( EntityHandle file_id, void* data, long len, bool ) 
    {
      EntityHandle h = startHandle++;
#ifndef NDEBUG
      size_t ok = 0;
      EntityHandle h2 = file_id;
      readHDF5->convert_id_to_handle( &h2, 1, ok );
      assert(ok && h2 == h);
#endif

      EntityHandle* array = reinterpret_cast<EntityHandle*>(data);
      size_t valid;
      readHDF5->convert_id_to_handle( array, len, valid );
      return readHDF5->moab()->add_child_meshsets( h, array, valid );
    }
  };

  dbgOut.print(1,"Reading set children\n");
  try {
    ReadHDF5Dataset met_dat( "set child offsets", set_meta_data_table, nativeParallel, mpiComm, false );
    met_dat.set_column( 1 );
    ReadHDF5Dataset cnt_dat( "set children", set_contents_table, nativeParallel, mpiComm, false );
    ReadSetChildren tool( dbgOut, dataBuffer, bufferSize,
                          start_handle, this );
    return tool.read( met_dat, cnt_dat, set_file_ids, 
                      fileInfo->sets.start_id, handleType );
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
}

ErrorCode ReadHDF5::read_parents( const Range& set_file_ids,
                                  EntityHandle start_handle,
                                  hid_t set_meta_data_table,
                                  hid_t set_contents_table,
                                  long /*set_contents_length*/ )
{
  class ReadSetParents : public ReadHDF5VarLen {
    ReadHDF5* readHDF5;
    EntityHandle startHandle;
  public:
    ReadSetParents( DebugOutput& dbg_out,
                    void* buffer,
                    size_t buffer_size,
                    EntityHandle start_handle,
                    ReadHDF5* moab )
                    : ReadHDF5VarLen(dbg_out, buffer, buffer_size ),
                      readHDF5(moab),
                      startHandle(start_handle)
                    {}
    ErrorCode store_data( EntityHandle file_id, void* data, long len, bool ) 
    {
      EntityHandle h = startHandle++;
#ifndef NDEBUG
      size_t ok = 0;
      EntityHandle h2 = file_id;
      readHDF5->convert_id_to_handle( &h2, 1, ok );
      assert(ok && h2 == h);
#endif

      EntityHandle* array = reinterpret_cast<EntityHandle*>(data);
      size_t valid;
      readHDF5->convert_id_to_handle( array, len, valid );
      return readHDF5->moab()->add_parent_meshsets( h, array, valid );
    }
  };

  dbgOut.print(1,"Reading set parents\n");
  try {
    ReadHDF5Dataset met_dat( "set parent offsets", set_meta_data_table, nativeParallel, mpiComm, false );
    met_dat.set_column( 2 );
    ReadHDF5Dataset cnt_dat( "set parents", set_contents_table, nativeParallel, mpiComm, false );
    ReadSetParents tool( dbgOut, dataBuffer, bufferSize,
                         start_handle, this );
    return tool.read( met_dat, cnt_dat, set_file_ids, 
                      fileInfo->sets.start_id, handleType );
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
}

static void copy_set_contents( int ranged,
                               const EntityHandle* contents,
                               long length,
                               Range& results )
{
  if (ranged) {
    assert( length%2 == 0 );
    Range::iterator hint = results.begin();
    for (long i = 0; i < length; i += 2)
      hint = results.insert( hint, contents[i], contents[i] + contents[i+1] - 1 );
  }
  else {
    for (long i = 0; i < length; ++i)
      results.insert( contents[i] );
  }
}


ErrorCode ReadHDF5::get_set_contents( const Range& sets, Range& file_ids )
{
  class GetContentList : public ReadHDF5VarLen {
    Range *const resultList;
  public:
    GetContentList( DebugOutput& dbg_out,
                    void* buffer,
                    size_t buffer_size,
                    Range* result_set )
                    : ReadHDF5VarLen(dbg_out, buffer, buffer_size),
                      resultList(result_set) 
                    {}

    ErrorCode store_data( EntityHandle, void* data, long len, bool ranged ) 
    {
      EntityHandle* array = reinterpret_cast<EntityHandle*>(data);
      if (ranged) {
        if (len % 2)
          return error(MB_INDEX_OUT_OF_RANGE);
        copy_set_contents( 1, array, len, *resultList );
      }
      else {
        std::sort( array, array+len );
        copy_sorted_file_ids( array, len, *resultList );
      }
      return MB_SUCCESS;
    }
  };

  ErrorCode rval;
  
  if (!fileInfo->have_set_contents)
    return MB_SUCCESS;

  dbgOut.tprint(2,"Reading set contained file IDs\n");
  try {
    mhdf_Status status;
    long content_len;
    hid_t meta, contents;
    meta = mhdf_openSetMetaSimple( filePtr, &status );
    if (is_error(status))
      return error(MB_FAILURE);
    ReadHDF5Dataset meta_reader( "set content offsets", meta, nativeParallel, mpiComm, true );
    meta_reader.set_column( 0 );

    contents = mhdf_openSetData( filePtr, &content_len, &status );
    if (is_error(status)) 
      return error(MB_FAILURE);
    ReadHDF5Dataset data_reader( "set contents", contents, nativeParallel, mpiComm, true );

    EntityHandle junk;
    Range ranged;
    rval = read_sets( sets, meta, ranged, junk, false );
    if (MB_SUCCESS != rval)
      return error(rval);

    GetContentList tool( dbgOut, dataBuffer, bufferSize, &file_ids );
    rval = tool.read( meta_reader, data_reader, sets, 
                      fileInfo->sets.start_id, handleType, &ranged );
    return rval;
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
}


ErrorCode ReadHDF5::read_adjacencies( hid_t table, long table_len )
{
  ErrorCode rval;
  mhdf_Status status;

  debug_barrier();

  hid_t read_type = H5Dget_type( table );
  if (read_type < 0) 
    return error(MB_FAILURE);
  const bool convert = !H5Tequal( read_type, handleType );
  
  EntityHandle* buffer = (EntityHandle*)dataBuffer;
  size_t chunk_size = bufferSize / H5Tget_size(read_type);
  size_t remaining = table_len;
  size_t left_over = 0;
  size_t offset = 0;
  dbgOut.printf( 3, "Reading adjacency list in %lu chunks\n",
    (unsigned long)(remaining + chunk_size - 1)/chunk_size );
  int nn = 0;
  while (remaining)
  {
    dbgOut.printf( 3, "Reading chunk %d of adjacency list\n", ++nn );
  
    size_t count = std::min( chunk_size, remaining );
    count -= left_over;
    remaining -= count;
    
    assert_range( buffer + left_over, count );
    mhdf_readAdjacencyWithOpt( table, offset, count, read_type, buffer + left_over,
                               collIO, &status );
    if (is_error(status))
      return error(MB_FAILURE);
    
    if (convert) {
      herr_t err = H5Tconvert( read_type, handleType, count, buffer + left_over, 0, H5P_DEFAULT );
      if (err < 0)
        return error(MB_FAILURE);
    }
    
    EntityHandle* iter = buffer;
    EntityHandle* end = buffer + count + left_over;
    while (end - iter >= 3)
    {
      EntityHandle h = idMap.find( *iter++ );
      EntityHandle count2 = *iter++;
      if (!h) {
        iter += count2;
        continue;
      }

      if (count2 < 1)
        return error(MB_FAILURE);

      if (end < count2 + iter)
      {
        iter -= 2;
        break;
      }
      
      size_t valid;
      convert_id_to_handle( iter, count2, valid, idMap );
      rval = iFace->add_adjacencies( h, iter, valid, false );
      if (MB_SUCCESS != rval)
        return error(rval);
     
      iter += count2;
    }
    
    left_over = end - iter;
    assert_range( (char*)buffer, left_over );
    assert_range( (char*)iter, left_over );
    memmove( buffer, iter, left_over );
  }
  
  assert(!left_over);  // unexpected truncation of data
  
  return MB_SUCCESS;  
}


ErrorCode ReadHDF5::read_tag( int tag_index )
{
  dbgOut.tprintf(2, "Reading tag \"%s\"\n", fileInfo->tags[tag_index].name );

  debug_barrier();


  ErrorCode rval;
  mhdf_Status status;
  Tag tag = 0;
  hid_t read_type = -1;
  bool table_type;
  rval = create_tag( fileInfo->tags[tag_index], tag, read_type ); 
  if (MB_SUCCESS != rval)
    return error(rval);

  if (fileInfo->tags[tag_index].have_sparse) {
    hid_t handles[3];
    long num_ent, num_val;
    mhdf_openSparseTagData( filePtr, 
                            fileInfo->tags[tag_index].name,
                            &num_ent, &num_val,
                            handles, &status );
    if (is_error(status)) {
      if (read_type) H5Tclose( read_type );
      return error(MB_FAILURE);
    }
    
    table_type = false;
    if (read_type == 0) {
      read_type = H5Dget_type( handles[1] );
      if (read_type == 0) {
        mhdf_closeData( filePtr, handles[0], &status );
        mhdf_closeData( filePtr, handles[0], &status );
        if (fileInfo->tags[tag_index].size <= 0) 
          mhdf_closeData( filePtr, handles[2], &status );
        return error(MB_FAILURE);
      }
      table_type = true;
    }

    if (fileInfo->tags[tag_index].size > 0) {
      dbgOut.printf(2, "Reading sparse data for tag \"%s\"\n", fileInfo->tags[tag_index].name );
      rval = read_sparse_tag( tag, read_type, handles[0], handles[1], num_ent );
    }
    else {
      dbgOut.printf(2, "Reading var-len sparse data for tag \"%s\"\n", fileInfo->tags[tag_index].name );
      rval = read_var_len_tag( tag, read_type, handles[0], handles[1], handles[2], num_ent, num_val );
    }

    if (table_type) {
      H5Tclose(read_type);
      read_type = 0;
    }
    
    mhdf_closeData( filePtr, handles[0], &status );
    if (MB_SUCCESS == rval && is_error(status))
      rval = MB_FAILURE;
    mhdf_closeData( filePtr, handles[1], &status );
    if (MB_SUCCESS == rval && is_error(status))
      rval = MB_FAILURE;
    if (fileInfo->tags[tag_index].size <= 0) {
      mhdf_closeData( filePtr, handles[2], &status );
      if (MB_SUCCESS == rval && is_error(status))
        rval = MB_FAILURE;
    }
    if (MB_SUCCESS != rval) {
      if (read_type) H5Tclose( read_type );
      return error(rval);
    }
  }
  
  for (int j = 0; j < fileInfo->tags[tag_index].num_dense_indices; ++j) {
    long count;
    const char* name = 0;
    mhdf_EntDesc* desc;
    int elem_idx = fileInfo->tags[tag_index].dense_elem_indices[j];
    if (elem_idx == -2) {
      desc = &fileInfo->sets;
      name = mhdf_set_type_handle();
    }
    else if (elem_idx == -1) {
      desc = &fileInfo->nodes;
      name = mhdf_node_type_handle();
    }
    else if (elem_idx >= 0 && elem_idx < fileInfo->num_elem_desc) {
      desc = &fileInfo->elems[elem_idx].desc;
      name = fileInfo->elems[elem_idx].handle;
    }
    else {
      return error(MB_FAILURE);
    }
    
    dbgOut.printf(2, "Read dense data block for tag \"%s\" on \"%s\"\n", fileInfo->tags[tag_index].name, name );
    
    hid_t handle = mhdf_openDenseTagData( filePtr, 
                                          fileInfo->tags[tag_index].name,
                                          name,
                                          &count, &status );
    if (is_error(status)) {
      rval = error(MB_FAILURE);
      break;
    }
    
    if (count > desc->count) {
      readUtil->report_error( "Invalid data length for dense tag data: %s/%s\n",
                              name, fileInfo->tags[tag_index].name );
      mhdf_closeData( filePtr, handle, &status );
      rval = error(MB_FAILURE);
      break;
    }
    
    table_type = false;
    if (read_type == 0) {
      read_type = H5Dget_type( handle );
      if (read_type == 0) {
        mhdf_closeData( filePtr, handle, &status );
        return error(MB_FAILURE);
      }
      table_type = true;
    }

    rval = read_dense_tag( tag, name, read_type, handle, desc->start_id, count );
    
    if (table_type) {
      H5Tclose( read_type );
      read_type = 0;
    }
    
    mhdf_closeData( filePtr, handle, &status );
    if (MB_SUCCESS != rval)
      break;
    if (is_error(status)) {
      rval = error(MB_FAILURE);
      break;
    }
  }
  
  if (read_type) 
    H5Tclose( read_type );
  return rval;
}
                              
                                            
    


ErrorCode ReadHDF5::create_tag( const mhdf_TagDesc& info,
                                Tag& handle,
                                hid_t& hdf_type )
{
  ErrorCode rval;
  mhdf_Status status;
  TagType storage;
  DataType mb_type;
  bool re_read_default = false;

  switch (info.storage) {
    case mhdf_DENSE_TYPE : storage = MB_TAG_DENSE ; break;
    case mhdf_SPARSE_TYPE: storage = MB_TAG_SPARSE; break;
    case mhdf_BIT_TYPE   : storage = MB_TAG_BIT;    break;
    case mhdf_MESH_TYPE  : storage = MB_TAG_MESH;   break;
    default:
      readUtil->report_error( "Invalid storage type for tag '%s': %d\n", info.name, info.storage );
      return error(MB_FAILURE);
  }

    // Type-specific stuff
  if (info.type == mhdf_BITFIELD) {
    if (info.size < 1 || info.size > 8)
    {
      readUtil->report_error( "Invalid bit tag:  class is MB_TAG_BIT, num bits = %d\n", info.size );
      return error(MB_FAILURE);
    }
    hdf_type = H5Tcopy(H5T_NATIVE_B8);
    mb_type = MB_TYPE_BIT;
    if (hdf_type < 0)
      return error(MB_FAILURE);
  }
  else if (info.type == mhdf_OPAQUE) {
    mb_type = MB_TYPE_OPAQUE;

      // Check for user-provided type
    Tag type_handle;
    std::string tag_type_name = "__hdf5_tag_type_";
    tag_type_name += info.name;
    rval = iFace->tag_get_handle( tag_type_name.c_str(), type_handle );
    if (MB_SUCCESS == rval) {
      rval = iFace->tag_get_data( type_handle, 0, 0, &hdf_type );
      if (MB_SUCCESS != rval)
        return error(rval);
      hdf_type = H5Tcopy( hdf_type );
      re_read_default = true;
    }
    else if (MB_TAG_NOT_FOUND == rval) {
      hdf_type = 0;
    }
    else
      return error(rval);
      
    if (hdf_type < 0)
      return error(MB_FAILURE);
  }
  else {
    switch (info.type)
    {
      case mhdf_INTEGER:
        hdf_type = H5T_NATIVE_INT;
        mb_type = MB_TYPE_INTEGER;
        break;

      case mhdf_FLOAT:
        hdf_type = H5T_NATIVE_DOUBLE;
        mb_type = MB_TYPE_DOUBLE;
        break;

      case mhdf_BOOLEAN:
        hdf_type = H5T_NATIVE_UINT;
        mb_type = MB_TYPE_INTEGER;
        break;

      case mhdf_ENTITY_ID:
        hdf_type = handleType;
        mb_type = MB_TYPE_HANDLE;
        break;

      default:
        return error(MB_FAILURE);
    }
    
    if (info.size > 1) { // array
        hsize_t tmpsize = info.size;
#if defined(H5Tarray_create_vers) && H5Tarray_create_vers > 1  
        hdf_type = H5Tarray_create2( hdf_type, 1, &tmpsize );
#else
        hdf_type = H5Tarray_create( hdf_type, 1, &tmpsize, NULL );
#endif
    }
    else {
      hdf_type = H5Tcopy( hdf_type );
    }
    if (hdf_type < 0)
      return error(MB_FAILURE);
  }

  
    // If default or global/mesh value in file, read it.
  if (info.default_value || info.global_value)
  {
    if (re_read_default) {
      mhdf_getTagValues( filePtr, info.name, hdf_type, info.default_value, info.global_value, &status );
      if (mhdf_isError( &status ))
      {
        readUtil->report_error( "%s", mhdf_message( &status ) );
        if (hdf_type) H5Tclose( hdf_type );
        return error(MB_FAILURE);
      }
    }
    
    if (MB_TYPE_HANDLE == mb_type) {
      if (info.default_value) {
        rval = convert_id_to_handle( (EntityHandle*)info.default_value, info.default_value_size );
        if (MB_SUCCESS != rval) {
          if (hdf_type) H5Tclose( hdf_type );
          return error(rval);
        }
      }
      if (info.global_value) {
        rval = convert_id_to_handle( (EntityHandle*)info.global_value, info.global_value_size );
        if (MB_SUCCESS != rval) {
          if (hdf_type) H5Tclose( hdf_type );
          return error(rval);
        }
      }
    }
  }
  
  
    // Check if tag already exists
  rval = iFace->tag_get_handle( info.name, handle );
  if (MB_SUCCESS == rval) {
    // If tag exists, make sure it is consistant with the type in the file
    int curr_size;
    DataType curr_type;
    TagType curr_store;
    
    rval = iFace->tag_get_size( handle, curr_size );
    if (MB_VARIABLE_DATA_LENGTH == rval)
      curr_size = -1;
    else if (MB_SUCCESS != rval) {
      if (hdf_type) H5Tclose( hdf_type );
      return error(rval);
    }
    
    rval = iFace->tag_get_data_type( handle, curr_type );
    if (MB_SUCCESS != rval) {
      if (hdf_type) H5Tclose( hdf_type );
      return error(rval);
    }
    
    rval = iFace->tag_get_type( handle, curr_store );
    if (MB_SUCCESS != rval) {
      if (hdf_type) H5Tclose( hdf_type );
      return error(rval);
    }
    
    if ((curr_store != MB_TAG_BIT && curr_size != info.bytes) || curr_type != mb_type ||
        ((curr_store == MB_TAG_BIT || storage == MB_TAG_BIT) && 
          curr_store != storage))
    {
      readUtil->report_error( "Tag type in file does not match type in "
                              "database for \"%s\"\n", info.name );
      if (hdf_type) H5Tclose( hdf_type );
      return error(MB_FAILURE);
    }
  }
    // Create the tag if it doesn't exist
  else if (MB_TAG_NOT_FOUND == rval)
  {
    if (info.size < 0) {
      size_t size = hdf_type ? H5Tget_size(hdf_type) : 1;
      rval = iFace->tag_create_variable_length( info.name, storage, mb_type,
                                                handle, info.default_value, 
                                                info.default_value_size * size );
    }
    else
      rval = iFace->tag_create( info.name, info.bytes, storage, mb_type,
                                handle, info.default_value );
    if (MB_SUCCESS != rval) {
      if (hdf_type) H5Tclose( hdf_type );
      return error(rval);
    }
  }
    // error
  else {
    if (hdf_type) H5Tclose( hdf_type );
    return error(rval);
  }
    
  if (info.global_value) {
    int type_size = hdf_type ? H5Tget_size(hdf_type) : 1;
    int tag_size = info.global_value_size * type_size;
    rval = iFace->tag_set_data( handle, 0, 0, &info.global_value, &tag_size );
    if (MB_SUCCESS != rval) {
      if (hdf_type) H5Tclose( hdf_type );
      return error(rval);
    }
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_dense_tag( Tag tag_handle,
                                    const char* ent_name,
                                    hid_t hdf_read_type,
                                    hid_t data,
                                    long start_id,
                                    long num_values )
{
  ErrorCode rval;
  DataType mb_type;
  
  rval = iFace->tag_get_data_type( tag_handle, mb_type );
  if (MB_SUCCESS != rval) 
    return error(rval);

  
  int read_size;
  rval = iFace->tag_get_size( tag_handle, read_size );
  if (MB_SUCCESS != rval) // wrong function for variable-length tags
    return error(rval);
  if (MB_TYPE_BIT == mb_type) 
    read_size = (read_size + 7)/8; // convert bits to bytes, plus 7 for ceiling
    
  if (hdf_read_type) { // if not opaque
    hsize_t hdf_size = H5Tget_size( hdf_read_type );
    if (hdf_size != (hsize_t)read_size) 
      return error(MB_FAILURE);
  }
  
    // get actual entities read from file
  Range file_ids, handles;
  Range::iterator f_ins = file_ids.begin(), h_ins = handles.begin();
  IDMap::iterator l, u;
  l = idMap.lower_bound( start_id );
  u = idMap.lower_bound( start_id + num_values - 1 );
  if (l != idMap.end() && start_id + num_values > l->begin) {
    if (l == u) {
      size_t beg = std::max(start_id, l->begin);
      size_t end = std::min(start_id + num_values, u->begin + u->count) - 1;
      f_ins = file_ids.insert( f_ins, beg, end );
      h_ins = handles.insert( h_ins, l->value + (beg - l->begin),
                                     l->value + (end - l->begin) );
    }
    else {
      size_t beg = std::max(start_id, l->begin);
      f_ins = file_ids.insert( f_ins, beg, l->begin + l->count - 1 );
      h_ins = handles.insert( h_ins, l->value + (beg - l->begin), l->value + l->count - 1 );
      for (++l; l != u; ++l) {
        f_ins = file_ids.insert( f_ins, l->begin, l->begin + l->count - 1 );
        h_ins = handles.insert( h_ins, l->value, l->value + l->count - 1 );
      }
      if (u != idMap.end() && u->begin < start_id + num_values) {
        size_t end = std::min( start_id + num_values, u->begin + u->count - 1 );
        f_ins = file_ids.insert( f_ins, u->begin, end );
        h_ins = handles.insert( h_ins, u->value, u->value + end - u->begin );
      }
    }
  }
  
    // Given that all of the entities for this dense tag data should
    // have been created as a single contiguous block, the resulting
    // MOAB handle range should be contiguous. 
    // THE ABOVE IS NOT NECESSARILY TRUE.  SOMETIMES LOWER-DIMENSION
    // ENTS ARE READ AND THEN DELETED FOR PARTIAL READS.
  //assert( handles.empty() || handles.size() == (handles.back() - handles.front() + 1));
  
  std::string tn("<error>");
  iFace->tag_get_name( tag_handle, tn );
  tn += " data for ";
  tn += ent_name;
  try {
    h_ins = handles.begin();
    ReadHDF5Dataset reader( tn.c_str(), data, nativeParallel, mpiComm, false );
    long buffer_size = bufferSize / read_size;
    reader.set_file_ids( file_ids, start_id, buffer_size, hdf_read_type );
    dbgOut.printf( 3, "Reading dense data for tag \"%s\" and group \"%s\" in %lu chunks\n",
                       tn.c_str(), ent_name, reader.get_read_count() );
    int nn = 0;
    while (!reader.done()) {
      dbgOut.printf( 3, "Reading chunk %d of \"%s\" data\n", ++nn, tn.c_str() );
    
      size_t count;
      reader.read( dataBuffer, count );

      if (MB_TYPE_HANDLE == mb_type) {
        rval = convert_id_to_handle( (EntityHandle*)dataBuffer, count * read_size / sizeof(EntityHandle) );
        if (MB_SUCCESS != rval)
          return error(rval);
      }

      Range ents;
      Range::iterator end = h_ins;
      end += count;
      ents.insert( h_ins, end );
      h_ins = end;

      rval = iFace->tag_set_data( tag_handle, ents, dataBuffer );
      if (MB_SUCCESS != rval)
        return error(MB_FAILURE);
    }
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
    
  return MB_SUCCESS;
}


  // Read entire ID table and for those file IDs corresponding
  // to entities that we have read from the file add both the
  // offset into the offset range and the handle into the handle 
  // range.  If handles are not ordered, switch to using a vector.
ErrorCode ReadHDF5::read_sparse_tag_indices( const char* name, 
                                             hid_t id_table,
                                             EntityHandle start_offset,// can't put zero in a Range
                                             Range& offset_range,
                                             Range& handle_range,
                                             std::vector<EntityHandle>& handle_vect )
{
  offset_range.clear();
  handle_range.clear();
  handle_vect.clear();

  ErrorCode rval;
  Range::iterator handle_hint = handle_range.begin();
  Range::iterator offset_hint = offset_range.begin();
  
  EntityHandle* idbuf = (EntityHandle*)dataBuffer;
  size_t idbuf_size = bufferSize / sizeof(EntityHandle);

  std::string tn(name);
  tn += " indices";

  assert(start_offset > 0); // can't put zero in a Range
  try {
    ReadHDF5Dataset id_reader( tn.c_str(), id_table, nativeParallel, mpiComm, false );
    id_reader.set_all_file_ids( idbuf_size, handleType );
    size_t offset = start_offset;
    dbgOut.printf( 3, "Reading file ids for sparse tag \"%s\" in %lu chunks\n", name, id_reader.get_read_count() );
    int nn = 0;
    while (!id_reader.done()) {\
      dbgOut.printf( 3, "Reading chunk %d of \"%s\" IDs\n", ++nn, name );
      size_t count;
      id_reader.read( idbuf, count ); 

      rval = convert_id_to_handle( idbuf, count );
      if (MB_SUCCESS != rval)
        return error(rval);

        // idbuf will now contain zero-valued handles for those
        // tag values that correspond to entities we are not reading
        // from the file.
      for (size_t i = 0; i < count; ++i) {
        if (idbuf[i]) {
          offset_hint = offset_range.insert( offset_hint, offset+i );
          if (!handle_vect.empty()) {
            handle_vect.push_back( idbuf[i] );
          }
          else if (handle_range.empty() || idbuf[i] > handle_range.back()) {
            handle_hint = handle_range.insert( handle_hint, idbuf[i] );
          }
          else {
            handle_vect.resize( handle_range.size() );
            std::copy( handle_range.begin(), handle_range.end(), handle_vect.begin() );
            handle_range.clear();
            handle_vect.push_back( idbuf[i] );
            dbgOut.print(2,"Switching to unordered list for tag handle list\n");
          }
        }
      }
      
      offset += count;
    }  
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }

  return MB_SUCCESS;
}
                                              


ErrorCode ReadHDF5::read_sparse_tag( Tag tag_handle,
                                     hid_t hdf_read_type,
                                     hid_t id_table,
                                     hid_t value_table,
                                     long /*num_values*/ )
{
    // Read entire ID table and for those file IDs corresponding
    // to entities that we have read from the file add both the
    // offset into the offset range and the handle into the handle 
    // range.  If handles are not ordered, switch to using a vector.
  const EntityHandle base_offset = 1; // can't put zero in a Range
  std::vector<EntityHandle> handle_vect;
  Range handle_range, offset_range;
  std::string tn("<error>");
  iFace->tag_get_name( tag_handle, tn );
  ErrorCode rval = read_sparse_tag_indices( tn.c_str(),
                                            id_table, base_offset,
                                            offset_range, handle_range,
                                            handle_vect );
  
  DataType mbtype;
  rval = iFace->tag_get_data_type( tag_handle, mbtype );
  if (MB_SUCCESS != rval) 
    return error(rval);
  
  int read_size;
  rval = iFace->tag_get_size( tag_handle, read_size );
  if (MB_SUCCESS != rval) // wrong function for variable-length tags
    return error(rval);
  if (MB_TYPE_BIT == mbtype) 
    read_size = (read_size + 7)/8; // convert bits to bytes, plus 7 for ceiling
    
  if (hdf_read_type) { // if not opaque
    hsize_t hdf_size = H5Tget_size( hdf_read_type );
    if (hdf_size != (hsize_t)read_size) 
      return error(MB_FAILURE);
  }

  const int handles_per_tag = read_size/sizeof(EntityHandle);

    // Now read data values
  size_t chunk_size = bufferSize / read_size;
  try {
    ReadHDF5Dataset val_reader( (tn + " values").c_str(), value_table, nativeParallel, mpiComm, false );
    val_reader.set_file_ids( offset_range, base_offset, chunk_size, hdf_read_type );
    dbgOut.printf( 3, "Reading sparse values for tag \"%s\" in %lu chunks\n", tn.c_str(), val_reader.get_read_count() );
    int nn = 0;
    size_t offset = 0;
    while (!val_reader.done()) {
      dbgOut.printf( 3, "Reading chunk %d of \"%s\" values\n", ++nn, tn.c_str() );
      size_t count;
      val_reader.read( dataBuffer, count );
      if (MB_TYPE_HANDLE == mbtype) {
        rval = convert_id_to_handle( (EntityHandle*)dataBuffer, count*handles_per_tag );
        if (MB_SUCCESS != rval)
          return error(rval);
      }
    
      if (!handle_vect.empty()) {
        rval = iFace->tag_set_data( tag_handle, &handle_vect[offset], count, dataBuffer );
        offset += count;
      }
      else {
        Range r;
        r.merge( handle_range.begin(), handle_range.begin() + count );
        handle_range.erase( handle_range.begin(), handle_range.begin() + count );
        rval = iFace->tag_set_data( tag_handle, r, dataBuffer );
      }
      if (MB_SUCCESS != rval)
        return error(rval);
    }
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_var_len_tag( Tag tag_handle,
                                      hid_t hdf_read_type,
                                      hid_t ent_table,
                                      hid_t val_table,
                                      hid_t off_table,
                                      long /*num_entities*/,
                                      long /*num_values*/ )
{
  ErrorCode rval;
  DataType mbtype;
  
  rval = iFace->tag_get_data_type( tag_handle, mbtype );
  if (MB_SUCCESS != rval) 
    return error(rval);
    
    // can't do variable-length bit tags
  if (MB_TYPE_BIT == mbtype)
    return error(MB_VARIABLE_DATA_LENGTH);

    // if here, MOAB tag must be variable-length
  int mbsize;
  if (MB_VARIABLE_DATA_LENGTH != iFace->tag_get_size( tag_handle, mbsize )) {
    assert(false);
    return error(MB_VARIABLE_DATA_LENGTH);
  }
  
  int read_size;
  if (hdf_read_type) {
    hsize_t hdf_size = H5Tget_size( hdf_read_type );
    if (hdf_size < 1)
      return error(MB_FAILURE);
    read_size = hdf_size;
  }
  else {
    // opaque
    read_size = 1;
  }
  
  std::string tn("<error>");
  iFace->tag_get_name( tag_handle, tn );

    // Read entire ID table and for those file IDs corresponding
    // to entities that we have read from the file add both the
    // offset into the offset range and the handle into the handle 
    // range.  If handles are not ordered, switch to using a vector.
  const EntityHandle base_offset = 1; // can't put zero in a Range
  std::vector<EntityHandle> handle_vect;
  Range handle_range, offset_range;
  rval = read_sparse_tag_indices( tn.c_str(),
                                  ent_table, base_offset,
                                  offset_range, handle_range,
                                  handle_vect );

    // This code only works if the id_table is an ordered list.
    // This assumption was also true for the previous iteration
    // of this code, but wasn't checked.  MOAB's file writer
    // always writes an ordered list for id_table.
  if (!handle_vect.empty()) {
    readUtil->report_error("Unordered file ids for variable length tag not supported.\n");
    return MB_FAILURE;
  }
  
  class VTReader : public ReadHDF5VarLen {
      Tag tagHandle;
      bool isHandle;
      size_t readSize;
      ReadHDF5* readHDF5;
    public:
      ErrorCode store_data( EntityHandle file_id, void* data, long count, bool )
      {
        ErrorCode rval;
        if (isHandle) {
          assert(readSize == sizeof(EntityHandle));
          rval = readHDF5->convert_id_to_handle( (EntityHandle*)data, count );
          if (MB_SUCCESS != rval)
            return error(rval);
        }
        int n = count*readSize;
        assert(count*readSize == (size_t)n); // watch for overflow
        return readHDF5->moab()->tag_set_data( tagHandle, &file_id, 1, &data, &n );
      }
      VTReader( DebugOutput& debug_output, void* buffer, size_t buffer_size,
                Tag tag, bool is_handle_tag, size_t read_size, ReadHDF5* owner )
        : ReadHDF5VarLen( debug_output, buffer, buffer_size ),
          tagHandle(tag),
          isHandle(is_handle_tag),
          readSize(read_size),
          readHDF5(owner)
      {}
  };
  
  VTReader tool( dbgOut, dataBuffer, bufferSize, tag_handle, 
                 MB_TYPE_HANDLE == mbtype, read_size, this );
  try {
      // Read offsets into value table.
    std::vector<unsigned> counts;
    Range offsets;
    ReadHDF5Dataset off_reader( (tn + " offsets").c_str(), off_table, nativeParallel, mpiComm, false );
    rval = tool.read_offsets( off_reader, offset_range, base_offset,
                              base_offset, offsets, counts );
    if (MB_SUCCESS != rval)
      return error(rval);
  
      // Read tag values
    Range empty;
    ReadHDF5Dataset val_reader( (tn + " values").c_str(), val_table, nativeParallel, mpiComm, false );
    rval = tool.read_data( val_reader, offsets, base_offset, hdf_read_type,
                           handle_range, counts, empty );
    if (MB_SUCCESS != rval)
      return error(rval);
  }
  catch (ReadHDF5Dataset::Exception) {
    return error(MB_FAILURE);
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::convert_id_to_handle( EntityHandle* array, 
                                            size_t size )
{
  convert_id_to_handle( array, size, idMap );
  return MB_SUCCESS;
}

void ReadHDF5::convert_id_to_handle( EntityHandle* array, 
                                     size_t size,
                                     const RangeMap<long,EntityHandle>& id_map )
{
  for (EntityHandle* const end = array + size; array != end; ++array)
    *array = id_map.find( *array );
}

void ReadHDF5::convert_id_to_handle( EntityHandle* array, 
                                     size_t size, size_t& new_size,
                                     const RangeMap<long,EntityHandle>& id_map )
{
  RangeMap<long,EntityHandle>::const_iterator it;
  new_size = 0;
  for (size_t i = 0; i < size; ++i) {
    it = id_map.lower_bound( array[i] );
    if (it != id_map.end() && it->begin <= (long)array[i])
      array[new_size++] = it->value + (array[i] - it->begin);
  }
}

void ReadHDF5::convert_range_to_handle( const EntityHandle* ranges,
                                        size_t num_ranges,
                                        const RangeMap<long,EntityHandle>& id_map,
                                        Range& merge )
{
  RangeMap<long,EntityHandle>::iterator it = id_map.begin();
  for (size_t i = 0; i < num_ranges; ++i) {
    long id = ranges[2*i];
    const long end = id + ranges[2*i+1];
      // we assume that 'ranges' is sorted, but check just in case it isn't.
    if (it == id_map.end() || it->begin > id)
      it = id_map.begin();
    it = id_map.lower_bound( it, id_map.end(), id );
    if (it == id_map.end())
      continue;
    if (id < it->begin)
      id = it->begin;
    while (id < end) {
      const long off = id - it->begin;
      long count = std::min( it->count - off,  end - id );
      merge.insert( it->value + off, it->value + off + count - 1 );
      id += count;
      if (id < end)
        if (++it == id_map.end())
          break;
    }
  }
}

ErrorCode ReadHDF5::convert_range_to_handle( const EntityHandle* array,
                                             size_t num_ranges,
                                             Range& range )
{
  convert_range_to_handle( array, num_ranges, idMap, range );
  return MB_SUCCESS;
}
  

ErrorCode ReadHDF5::insert_in_id_map( const Range& file_ids,
                                      EntityHandle start_id )
{
  IDMap tmp_map;
  bool merge = !idMap.empty() && !file_ids.empty() && idMap.back().begin > (long)file_ids.front();
  IDMap& map = merge ? tmp_map : idMap;
  Range::const_pair_iterator p;
  for (p = file_ids.const_pair_begin(); p != file_ids.const_pair_end(); ++p) {
    size_t count = p->second - p->first + 1;
    if (!map.insert( p->first, start_id, count ).second) 
      return error(MB_FAILURE);
    start_id += count;
  }
  if (merge && !idMap.merge( tmp_map ))
    return error(MB_FAILURE);
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::insert_in_id_map( long file_id,
                                      EntityHandle handle )
{
  if (!idMap.insert( file_id, handle, 1 ).second) 
      return error(MB_FAILURE);
  return MB_SUCCESS;
}


ErrorCode ReadHDF5::read_qa( EntityHandle  )
{
  mhdf_Status status;
  std::vector<std::string> qa_list;
  
  int qa_len;
  char** qa = mhdf_readHistory( filePtr, &qa_len, &status );
  if (mhdf_isError( &status ))
  {
    readUtil->report_error( "%s", mhdf_message( &status ) );
    return error(MB_FAILURE);
  }
  qa_list.resize(qa_len);
  for (int i = 0; i < qa_len; i++)
  {
    qa_list[i] = qa[i];
    free( qa[i] );
  }
  free( qa );
  
  /** FIX ME - how to put QA list on set?? */

  return MB_SUCCESS;
}

ErrorCode ReadHDF5::store_file_ids( Tag tag )
{
  typedef int tag_type;
  tag_type* buffer = reinterpret_cast<tag_type*>(dataBuffer);
  const long buffer_size = bufferSize / sizeof(tag_type);
  for (IDMap::iterator i = idMap.begin(); i != idMap.end(); ++i) {
    IDMap::Range range = *i;
    
      // make sure the values will fit in the tag type
    IDMap::key_type rv = range.begin + (range.count - 1);
    tag_type tv = (tag_type)rv;
    if ((IDMap::key_type)tv != rv) {
      assert(false);
      return MB_INDEX_OUT_OF_RANGE;
    }
    
    while (range.count) {
      long count = buffer_size < range.count ? buffer_size : range.count;

      Range handles;
      handles.insert( range.value, range.value + count - 1 );
      range.value += count;
      range.count -= count;
      for (long j = 0; j < count; ++j) 
        buffer[j] = (tag_type)range.begin++;

      ErrorCode rval = iFace->tag_set_data( tag, handles, buffer );
      if (MB_SUCCESS != rval)
        return rval;
    }
  }
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_tag_values( const char* file_name,
                                     const char* tag_name,
                                     const FileOptions& opts,
                                     std::vector<int>& tag_values_out,
                                     const SubsetList* subset_list )
{
  ErrorCode rval;
  
  rval = set_up_read( file_name, opts );
  if (MB_SUCCESS != rval)
    return error(rval);
  
  int tag_index;
  rval = find_int_tag( tag_name, tag_index );
  if (MB_SUCCESS != rval) {
    clean_up_read( opts );
    return error(rval);
  }
  
  if (subset_list) {
    Range file_ids;
    rval = get_subset_ids( subset_list->tag_list, subset_list->tag_list_length, file_ids );
    if (MB_SUCCESS != rval) {
      clean_up_read( opts );
      return error(rval);
    }
    
    rval = read_tag_values_partial( tag_index, file_ids, tag_values_out );
    if (MB_SUCCESS != rval) {
      clean_up_read( opts );
      return error(rval);
    }
  }
  else {
    rval = read_tag_values_all( tag_index, tag_values_out );
    if (MB_SUCCESS != rval) {
      clean_up_read( opts );
      return error(rval);
    }
  }
    
  return clean_up_read( opts );
}

ErrorCode ReadHDF5::read_tag_values_partial( int tag_index,
                                             const Range& file_ids,
                                             std::vector<int>& tag_values )
{
  mhdf_Status status;
  const mhdf_TagDesc& tag = fileInfo->tags[tag_index];
  long num_ent, num_val;
  size_t count;
  std::string tn(tag.name);
  
    // read sparse values
  if (tag.have_sparse) {
    hid_t handles[3];
    mhdf_openSparseTagData( filePtr, tag.name, &num_ent, &num_val, handles, &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      return error(MB_FAILURE);
    }
    
    try {
        // read all entity handles and fill 'offsets' with ranges of
        // offsets into the data table for entities that we want.
      Range offsets;
      long* buffer = reinterpret_cast<long*>(dataBuffer);
      const long buffer_size = bufferSize/sizeof(long);
      ReadHDF5Dataset ids( (tn + " ids").c_str(), handles[0], nativeParallel, mpiComm );
      ids.set_all_file_ids( buffer_size, H5T_NATIVE_LONG );
      size_t offset = 0;
      dbgOut.printf( 3, "Reading sparse IDs for tag \"%s\" in %lu chunks\n",
                     tag.name, ids.get_read_count() );
      int nn = 0;
      while (!ids.done()) {
        dbgOut.printf( 3, "Reading chunk %d of IDs for \"%s\"\n", ++nn, tag.name );
        ids.read( buffer, count );

        std::sort( buffer, buffer+count );
        Range::iterator ins = offsets.begin();
        Range::const_iterator i = file_ids.begin();
        for (size_t j = 0; j < count; ++j) {
          while (i != file_ids.end() && (long)*i < buffer[j])
            ++i;
          if (i == file_ids.end())
            break;
          if ((long)*i == buffer[j]) {
            ins = offsets.insert( ins, j+offset, j+offset );
          }
        }
        
        offset += count;
      }

      tag_values.clear();
      tag_values.reserve( offsets.size() );
      const size_t data_buffer_size = bufferSize/sizeof(int);
      int* data_buffer = reinterpret_cast<int*>(dataBuffer);
      ReadHDF5Dataset vals( (tn + " sparse vals").c_str(), handles[1], nativeParallel, mpiComm );
      vals.set_file_ids( offsets, 0, data_buffer_size, H5T_NATIVE_INT );
      dbgOut.printf( 3, "Reading sparse values for tag \"%s\" in %lu chunks\n",
                     tag.name, vals.get_read_count() );
      nn = 0;
      // should normally only have one read call, unless sparse nature
      // of file_ids caused reader to do something strange
      while (!vals.done()) {
        dbgOut.printf( 3, "Reading chunk %d of values for \"%s\"\n", ++nn, tag.name );
        vals.read( data_buffer, count );
        tag_values.insert( tag_values.end(), data_buffer, data_buffer+count );
      }
    }
    catch (ReadHDF5Dataset::Exception) {
      return error(MB_FAILURE);
    }
  }
  
  std::sort( tag_values.begin(), tag_values.end() );
  tag_values.erase( std::unique(tag_values.begin(), tag_values.end()), tag_values.end() );
  
    // read dense values
  std::vector<int> prev_data, curr_data;
  for (int i = 0; i < tag.num_dense_indices; ++i) {
    int grp = tag.dense_elem_indices[i];
    const char* gname = 0;
    mhdf_EntDesc* desc = 0;
    if (grp == -1) {
      gname = mhdf_node_type_handle();
      desc = &fileInfo->nodes;
    }
    else if (grp == -2) {
      gname = mhdf_set_type_handle();
      desc = &fileInfo->sets;
    }
    else {
      assert(grp >= 0 && grp < fileInfo->num_elem_desc);
      gname = fileInfo->elems[grp].handle;
      desc = &fileInfo->elems[grp].desc;
    }
    
    Range::iterator s = file_ids.lower_bound( (EntityHandle)(desc->start_id) );
    Range::iterator e = Range::lower_bound( s, file_ids.end(),  
                                   (EntityHandle)(desc->start_id) + desc->count );
    Range subset;
    subset.merge( s, e );
    
    hid_t handle = mhdf_openDenseTagData( filePtr, tag.name, gname, &num_val, &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      return error(MB_FAILURE);
    }
    
    try {
      curr_data.clear();
      tag_values.reserve( subset.size() );
      const size_t data_buffer_size = bufferSize/sizeof(int);
      int* data_buffer = reinterpret_cast<int*>(dataBuffer);

      ReadHDF5Dataset reader( (tn + " dense vals").c_str(), handle, nativeParallel, mpiComm );
      reader.set_file_ids( subset, desc->start_id, data_buffer_size, H5T_NATIVE_INT );
      dbgOut.printf( 3, "Reading dense data for tag \"%s\" and group \"%s\" in %lu chunks\n",
        tag.name, fileInfo->elems[grp].handle, reader.get_read_count() );
      int nn = 0;
      // should normally only have one read call, unless sparse nature
      // of file_ids caused reader to do something strange
      while (!reader.done()) {
        dbgOut.printf( 3, "Reading chunk %d of \"%s\"/\"%s\"\n", ++nn, tag.name, fileInfo->elems[grp].handle );
        reader.read( data_buffer, count );
        curr_data.insert( curr_data.end(), data_buffer, data_buffer + count );
      }
    }
    catch (ReadHDF5Dataset::Exception) {
      return error(MB_FAILURE);
    }
    
    std::sort( curr_data.begin(), curr_data.end() );
    curr_data.erase( std::unique(curr_data.begin(), curr_data.end()), curr_data.end() );
    prev_data.clear();
    tag_values.swap( prev_data );
    std::set_union( prev_data.begin(), prev_data.end(),
                    curr_data.begin(), curr_data.end(),
                    std::back_inserter( tag_values ) );
  }
  
  return MB_SUCCESS;
}

ErrorCode ReadHDF5::read_tag_values_all( int tag_index,
                                         std::vector<int>& tag_values )
{
  mhdf_Status status;
  const mhdf_TagDesc& tag = fileInfo->tags[tag_index];
  long junk, num_val;
  
    // read sparse values
  if (tag.have_sparse) {
    hid_t handles[3];
    mhdf_openSparseTagData( filePtr, tag.name, &junk, &num_val, handles, &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      return error(MB_FAILURE);
    }
    
    mhdf_closeData( filePtr, handles[0], &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      mhdf_closeData( filePtr, handles[1], &status );
      return error(MB_FAILURE);
    }
    
    hid_t file_type = H5Dget_type( handles[1] );
    tag_values.resize( num_val );
    mhdf_readTagValuesWithOpt( handles[1], 0, num_val, file_type,
                               &tag_values[0], collIO, &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      H5Tclose( file_type );
      mhdf_closeData( filePtr, handles[1], &status );
      return error(MB_FAILURE);
    }
    H5Tconvert( file_type, H5T_NATIVE_INT, num_val, &tag_values[0], 0, H5P_DEFAULT );
    H5Tclose( file_type );
    
    mhdf_closeData( filePtr, handles[1], &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      return error(MB_FAILURE);
    }
  }
  
  std::sort( tag_values.begin(), tag_values.end() );
  tag_values.erase( std::unique(tag_values.begin(), tag_values.end()), tag_values.end() );
  
    // read dense values
  std::vector<int> prev_data, curr_data;
  for (int i = 0; i < tag.num_dense_indices; ++i) {
    int grp = tag.dense_elem_indices[i];
    const char* gname = 0;
    if (grp == -1)
      gname = mhdf_node_type_handle();
    else if (grp == -2)
      gname = mhdf_set_type_handle();
    else
      gname = fileInfo->elems[grp].handle;
    hid_t handle = mhdf_openDenseTagData( filePtr, tag.name, gname, &num_val, &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      return error(MB_FAILURE);
    }
    
    hid_t file_type = H5Dget_type( handle );
    curr_data.resize( num_val );
    mhdf_readTagValuesWithOpt( handle, 0, num_val, file_type, &curr_data[0], collIO, &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      H5Tclose( file_type );
      mhdf_closeData( filePtr, handle, &status );
      return error(MB_FAILURE);
    }
    
    H5Tconvert( file_type, H5T_NATIVE_INT, num_val, &curr_data[0], 0, H5P_DEFAULT );
    H5Tclose( file_type );
    mhdf_closeData( filePtr, handle, &status );
    if (mhdf_isError( &status )) {
      readUtil->report_error( "%s", mhdf_message( &status ) );
      return error(MB_FAILURE);
    }
 
    std::sort( curr_data.begin(), curr_data.end() );
    curr_data.erase( std::unique(curr_data.begin(), curr_data.end()), curr_data.end() );
    
    prev_data.clear();
    tag_values.swap( prev_data );
    std::set_union( prev_data.begin(), prev_data.end(),
                    curr_data.begin(), curr_data.end(),
                    std::back_inserter( tag_values ) );
  }
  
  return MB_SUCCESS;
}

} // namespace moab
