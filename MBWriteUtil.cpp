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

#ifdef WIN32
#pragma warning(disable : 4786)
#endif

#include "MBWriteUtil.hpp"
#include "MBCore.hpp"
#include "MBError.hpp"
#include "SequenceManager.hpp"
#include "ElementSequence.hpp"
#include "VertexSequence.hpp"
#include "TagServer.hpp"
#include "AEntityFactory.hpp"
#include "MBTagConventions.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

#ifdef WIN32
#  define stat _stat
#else
#  include <unistd.h>
#endif


MBWriteUtil::MBWriteUtil(MBCore* mdb, MBError* error_handler) 
    : MBWriteUtilIface(), mMB(mdb), mError(error_handler)
{
}

  //! Check if the specified file already exists.
  //! Returns MB_SUCCESS if file does not exist, MB_ALREADY_ALLOCATED
  //! if file does exist, or MB_FAILURE for some other error condition.
MBErrorCode MBWriteUtil::check_doesnt_exist( const char* file_name )
{
  struct stat s;
  if (0 == stat( file_name, &s ))
  {
    report_error( "%s: file already exists.\n", file_name );
    return MB_ALREADY_ALLOCATED;  
  }
  else if (errno == ENOENT)
  {
    return MB_SUCCESS;
  }
  else
  {
    return MB_FAILURE;
  }
}


MBErrorCode MBWriteUtil::get_node_arrays(
    const int num_arrays,
    const int num_nodes, 
    const MBRange& entities, 
    MBTag node_id_tag,
    const int start_node_id,
    std::vector<double*>& arrays)
{
  // check the data coming into the function
  // dimension should be proper
  if(num_arrays < 1 || num_arrays > 3)
    return MB_FAILURE;

  // there should be some entities
  //if(entities.empty())
  //  return MB_FAILURE;
  // The above necessitates annoying special cases for files 
  // w/out vertices (e.g. a kD-tree).  Return NULL array
  // pointers instead. - kraftcheck, 3-14-08
  if (entities.empty()) {
    arrays.clear();
    arrays.resize( num_arrays, NULL );
    return MB_SUCCESS;
  }

  // memory should already be allocated for us
  int tmp_num_arrays = 0;
  for (unsigned int i = 0; i < 3; i++)
    if (i+1 <= arrays.size() && NULL != arrays[i]) tmp_num_arrays++;
  if (0 == tmp_num_arrays)
    return MB_FAILURE;

  // get coordinate data
  MBErrorCode result = mMB->get_coords( entities, 
                   num_arrays < 1 || arrays.size() < 1 ? NULL : arrays[0],
                   num_arrays < 2 || arrays.size() < 2 ? NULL : arrays[1],
                   num_arrays < 3 || arrays.size() < 3 ? NULL : arrays[2] );
                   

  if (0 == node_id_tag || MB_SUCCESS != result) 
    return result;
  
    // now assign tags
  std::vector<int> ids(num_nodes);
  int node_id = start_node_id;
  for (int i = 0; i < num_nodes; i++) ids[i] = node_id++;
  result = mMB->tag_set_data(node_id_tag, entities, &ids[0]);

  return result;
}

MBErrorCode MBWriteUtil::get_node_array(
    const int which_array, /* 0->X, 1->Y, 2->Z */
    MBRange::const_iterator iter,
    const MBRange::const_iterator end,
    const size_t output_array_len,
    double* const output_array)
{
  // check the data coming into the function
  // dimension should be proper
  if(which_array < 0 || which_array > 2)
    return MB_FAILURE;

  // there should be some entities
  if(iter == end)
    return MB_FAILURE;

  // memory should already be allocated for us
  if (NULL == output_array || 0 == output_array_len)
    return MB_FAILURE;

  // Sequence iterators
  TypeSequenceManager::iterator seq_iter, seq_end;
  seq_iter = mMB->sequence_manager()->entity_map(MBVERTEX).begin();
  seq_end = mMB->sequence_manager()->entity_map(MBVERTEX).end();
  
  // loop over range, getting coordinate value
  double* output_iter = output_array;
  double* const output_end = output_array + output_array_len;
  while (iter != end)
  {
      // Find the sqeuence containing the current handle
    while (seq_iter != seq_end && (*seq_iter)->end_handle() < *iter)
      ++seq_iter;
    if (seq_iter == seq_end || *iter < (*seq_iter)->start_handle())
      return MB_FAILURE;
    
      // Determine how much of the sequence we want.
    MBRange::pair_iterator pair(iter);
    MBRange::const_iterator prev(end);
    --prev;
    MBEntityHandle range_end = pair->second;
    MBEntityHandle sequence_end = (*seq_iter)->end_handle();
    MBEntityHandle end_handle = range_end > sequence_end ? sequence_end : range_end;
    if (end_handle > *prev)
      end_handle = *prev;
    MBEntityHandle count = end_handle - *iter + 1;
    
      // Get offset in sequence to start at
    assert( *iter >= (*seq_iter)->start_handle() );
    MBEntityHandle offset = *iter - (*seq_iter)->start_handle();
    
      // Get coordinate arrays from sequence
    double* coord_array[3];
    static_cast<VertexSequence*>(*seq_iter)
      ->get_coordinate_arrays( coord_array[0], coord_array[1], coord_array[2]);
    
      // Copy data to ouput buffer
    if (output_iter + count > output_end)
      return MB_FAILURE;
    memcpy( output_iter, coord_array[which_array] + offset, count * sizeof(double) );
    
      // Iterate
    output_iter += count;
    iter += count;
  }

  return MB_SUCCESS;
}

MBErrorCode MBWriteUtil::get_element_array(
    const int num_elements, 
    const int verts_per_element,
    MBTag node_id_tag,
    const MBRange& elements, 
    MBTag element_id_tag,
    int start_element_id,
    int* element_array)
{

  // check the data we got
  if(num_elements < 1)
    return MB_FAILURE;
  if(verts_per_element < 1)
    return MB_FAILURE;
  if(elements.empty())
    return MB_FAILURE;
  if(!element_array)
    return MB_FAILURE;

  TagServer* tag_server = mMB->tag_server();

  MBRange::const_iterator range_iter = elements.begin();
  MBRange::const_iterator range_iter_end = elements.end();

  TypeSequenceManager::iterator seq_iter, seq_iter_end;
  MBEntityType current_type = TYPE_FROM_HANDLE(*range_iter);
 
  seq_iter = mMB->sequence_manager()->entity_map(current_type).begin();
  seq_iter_end = mMB->sequence_manager()->entity_map(current_type).end();

  // lets find the entity sequence which holds the first entity
  TypeSequenceManager::iterator seq_iter_lookahead = seq_iter;
  seq_iter_lookahead++;
  for( ; seq_iter_lookahead != seq_iter_end && 
      (*seq_iter_lookahead)->start_handle() < *range_iter; )
  {
    ++seq_iter;
    ++seq_iter_lookahead;
  }

  // a look ahead iterator
  MBRange::const_iterator range_iter_lookahead = range_iter;

  // our main loop
  for(; range_iter != range_iter_end && seq_iter != seq_iter_end; /* ++ is handled in loop*/ )
  {
    // find a range that fits in the current entity sequence
    for(; range_iter_lookahead != range_iter_end && 
        *range_iter_lookahead <= (*seq_iter)->end_handle(); 
        ++range_iter_lookahead)
    {}
  
    if(current_type != TYPE_FROM_HANDLE(*range_iter))
    {
      current_type = TYPE_FROM_HANDLE(*range_iter);
      seq_iter = mMB->sequence_manager()->entity_map(current_type).begin();
      seq_iter_end = mMB->sequence_manager()->entity_map(current_type).end();

      // lets find the entity sequence which holds the first entity of this type
      TypeSequenceManager::const_iterator seq_iter_lookahead = seq_iter;
      seq_iter_lookahead++;
      for( ; seq_iter_lookahead != seq_iter_end && 
          (*seq_iter_lookahead)->start_handle() < *range_iter; )
      {
        ++seq_iter;
        ++seq_iter_lookahead;
      }
    }

    int i = static_cast<ElementSequence*>(*seq_iter)->nodes_per_element();

    // get the connectivity array
    MBEntityHandle* conn_array = 
      static_cast<ElementSequence*>(*seq_iter)->get_connectivity_array();
 
    MBEntityHandle start_handle = (*seq_iter)->start_handle();

    for(MBRange::const_iterator tmp_iter = range_iter; 
        tmp_iter != range_iter_lookahead;
        ++tmp_iter)
    {
      // set the element id tag
      tag_server->set_data(element_id_tag, *tmp_iter, &start_element_id);
      ++start_element_id;

      // for each node
      for(int j=0; j<i; j++)
      {
        MBEntityHandle node = *(conn_array + j + i*(*tmp_iter - start_handle));
        tag_server->get_data(node_id_tag, node, element_array);
        element_array++;
      }
    }

    // go to the next entity sequence
    ++seq_iter;
    // start with the next entities
    range_iter = range_iter_lookahead;
  }

  return MB_SUCCESS;
}

MBErrorCode MBWriteUtil::get_element_array(
    MBRange::const_iterator iter,
    const MBRange::const_iterator end,
    const int vertices_per_elem,
    MBTag node_id_tag,
    const size_t elem_array_size, 
    int *const element_array)
{

  // check the data we got
  if(iter == end)
    return MB_FAILURE;
  if(vertices_per_elem < 1)
    return MB_FAILURE;
  if(!element_array || elem_array_size < (unsigned)vertices_per_elem)
    return MB_FAILURE;

  TagServer* tag_server = mMB->tag_server();


  // Sequence iterators
  TypeSequenceManager::const_iterator seq_iter, seq_end;
  
  // loop over range, getting coordinate value
  MBEntityType current_type = MBMAXTYPE;
  int* output_iter = element_array;
  int*const output_end = element_array + elem_array_size;
  while (iter != end)
  {
      // Make sure we have the right sequence list (and get the sequence 
      // list for the first iteration.)
    MBEntityType type = TYPE_FROM_HANDLE(*iter);
    if (type != current_type)
    {
      if (type >= MBENTITYSET || type < MBEDGE)
        return MB_FAILURE;
      seq_iter = mMB->sequence_manager()->entity_map(type).begin();
      seq_end  = mMB->sequence_manager()->entity_map(type).end();
      current_type = type;
    }
    
      // Find the sqeuence containing the current handle
    while (seq_iter != seq_end && (*seq_iter)->end_handle() < *iter)
      ++seq_iter;
    if (seq_iter == seq_end || *iter < (*seq_iter)->start_handle())
      return MB_FAILURE;
 
      // get the connectivity array
    MBEntityHandle* conn_array = NULL;
    int conn_size = static_cast<ElementSequence*>(*seq_iter)->nodes_per_element();
    conn_array = static_cast<ElementSequence*>(*seq_iter)->get_connectivity_array();
   
      // Determine how much of the sequence we want.
    MBRange::pair_iterator pair(iter);
    MBRange::const_iterator prev(end);
    --prev;
    MBEntityHandle range_end = pair->second;
    MBEntityHandle sequence_end = (*seq_iter)->end_handle();
    MBEntityHandle end_handle = range_end > sequence_end ? sequence_end : range_end;
    if (end_handle > *prev)
      end_handle = *prev;
    MBEntityHandle count = end_handle - *iter + 1;
    
      // Get offset in sequence to start at
    assert( *iter >= (*seq_iter)->start_handle() );
    MBEntityHandle offset = *iter - (*seq_iter)->start_handle();

      // Make sure sufficient space in output array
    if (output_iter + (count * conn_size) > output_end)
      return MB_FAILURE;

      // If the nodes per element match, do in one call
    conn_array += (conn_size * offset);
    if (vertices_per_elem == conn_size)
    {
      MBErrorCode rval = tag_server->get_data( node_id_tag, 
                                               conn_array,
                                               count * conn_size,
                                               output_iter );
      if (MB_SUCCESS != rval)
        return rval;
      
      output_iter += count * conn_size;
    }
      // Otherwise need to do one at a time
    else
    {
      int min = vertices_per_elem > conn_size ? conn_size : vertices_per_elem;
      for (MBEntityHandle i = 0; i < count; ++i)
      {
        MBErrorCode rval = tag_server->get_data( node_id_tag,
                                                 conn_array,
                                                 min,
                                                 output_iter );
        if (MB_SUCCESS != rval)
          return rval;

        output_iter += min;
        conn_array += conn_size;

        if (vertices_per_elem > conn_size) // need to pad
        {
          memset( output_iter, 0, sizeof(int) * (vertices_per_elem - conn_size) );
          output_iter += (vertices_per_elem - conn_size);
        }
      }
    }

    iter += count;
  }

  return MB_SUCCESS;
}

MBErrorCode MBWriteUtil::get_poly_array_size(
      MBRange::const_iterator ,
      const MBRange::const_iterator ,
      int&  )
{
  return MB_NOT_IMPLEMENTED;
}

MBErrorCode MBWriteUtil::get_poly_arrays(
      MBRange::const_iterator& ,
      const MBRange::const_iterator ,
      const MBTag ,
      size_t& ,
      int *const ,
      size_t& ,
      int*const ,
      int&  )
{
  return MB_NOT_IMPLEMENTED;
}
  

      
MBErrorCode MBWriteUtil::gather_nodes_from_elements(
      const MBRange& elements,
      const MBTag node_bit_mark_tag,
      MBRange& nodes
      )
{

  if(elements.empty())
    return MB_SUCCESS;

  TagServer* tag_server = mMB->tag_server();

  // see if we need to use our own marking tag
  MBTag exporting_nodes_tag = 0;
  if(node_bit_mark_tag)
    exporting_nodes_tag = node_bit_mark_tag;
  else
  {
    mMB->tag_create("__MBWriteUtil::exporting_nodes", 1, MB_TAG_BIT, 
                     exporting_nodes_tag, NULL);
  }
  

  MBRange::const_iterator range_iter = elements.begin();
  MBRange::const_iterator range_iter_end = elements.end();

  TypeSequenceManager::const_iterator seq_iter, seq_iter_end;
  MBEntityType current_type = TYPE_FROM_HANDLE(*range_iter);
 
  seq_iter = mMB->sequence_manager()->entity_map(current_type).begin();
  seq_iter_end = mMB->sequence_manager()->entity_map(current_type).end();

  // lets find the entity sequence which holds the first entity
  TypeSequenceManager::const_iterator seq_iter_lookahead = seq_iter;
  seq_iter_lookahead++;
  for( ; seq_iter_lookahead != seq_iter_end && 
      (*seq_iter_lookahead)->start_handle() < *range_iter; )
  {
    ++seq_iter;
    ++seq_iter_lookahead;
  }

  // a look ahead iterator
  MBRange::const_iterator range_iter_lookahead = range_iter;

  // the x,y,z tag handles we need
  MBEntityHandle lower_bound = ~0, upper_bound = 0;
  
  // our main loop
  for(; range_iter != range_iter_end && seq_iter != seq_iter_end; /* ++ is handled in loop*/ )
  {
    // find a range that fits in the current entity sequence
    for(; range_iter_lookahead != range_iter_end && 
        *range_iter_lookahead <= (*seq_iter)->end_handle(); 
        ++range_iter_lookahead)
    {}
  
    if(current_type != TYPE_FROM_HANDLE(*range_iter))
    {
      current_type = TYPE_FROM_HANDLE(*range_iter);
      seq_iter = mMB->sequence_manager()->entity_map(current_type).begin();
      seq_iter_end = mMB->sequence_manager()->entity_map(current_type).end();

      // lets find the entity sequence which holds the first entity of this type
      TypeSequenceManager::const_iterator seq_iter_lookahead = seq_iter;
      seq_iter_lookahead++;
      for( ; seq_iter_lookahead != seq_iter_end && 
          (*seq_iter_lookahead)->start_handle() < *range_iter; )
      {
        ++seq_iter;
        ++seq_iter_lookahead;
      }
    }

    int i = static_cast<ElementSequence*>(*seq_iter)->nodes_per_element();

    // get the connectivity array
    MBEntityHandle* conn_array = NULL;
    conn_array = static_cast<ElementSequence*>(*seq_iter)->get_connectivity_array();
 
    MBEntityHandle start_handle = (*seq_iter)->start_handle();

    for(MBRange::const_iterator tmp_iter = range_iter; 
        tmp_iter != range_iter_lookahead;
        ++tmp_iter)
    {
      // for each node
      for(int j=0; j<i; j++)
      {
        MBEntityHandle node = *(conn_array + j + i*(*tmp_iter - start_handle));
        if(node < lower_bound)
          lower_bound = node;
        if(node > upper_bound)
          upper_bound = node;
        unsigned char bit = 0x1;
        tag_server->set_data(exporting_nodes_tag, &node, 1, &bit);
      }
    }

    // go to the next entity sequence
    ++seq_iter;
    // start with the next entities
    range_iter = range_iter_lookahead;
  }

  // we can get a REALLY long loop if lower_bound is zero
  assert(lower_bound != 0);
  // gather up all the nodes
  for(; upper_bound >= lower_bound; --upper_bound)
  {
    unsigned char node_marked=0;
    tag_server->get_data(exporting_nodes_tag, &upper_bound, 1, &node_marked);
    if(node_marked == 0x1)
      nodes.insert(upper_bound);
  }

  // clean up our own marking tag
  if(node_bit_mark_tag == 0)
    mMB->tag_delete(exporting_nodes_tag);

  return MB_SUCCESS;

}

  //! assign ids to input elements starting with start_id, written to id_tag
  //! if zero, assigns to GLOBAL_ID_TAG_NAME
MBErrorCode MBWriteUtil::assign_ids(MBRange &elements,
                                    MBTag id_tag,
                                    const int start_id) 
{
  MBErrorCode result;
  if (0 == id_tag) {
      // get the global id tag
    result = mMB->tag_get_handle(GLOBAL_ID_TAG_NAME, id_tag);
    if (MB_TAG_NOT_FOUND == result) {
      int def_val = -1;
      result = mMB->tag_create(GLOBAL_ID_TAG_NAME, 4, MB_TAG_DENSE, id_tag, &def_val);
    }
    
    if (MB_SUCCESS != result) return result;
  }
  
    // now assign the ids
  int i;
  MBRange::iterator rit;
  MBErrorCode tmp_result;
  result = MB_SUCCESS;
  for (i = start_id, rit = elements.begin(); rit != elements.end(); rit++, i++) {
    tmp_result = mMB->tag_set_data(id_tag, &(*rit), 1, &i);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
  }
  
  return result;
}

MBErrorCode MBWriteUtil::report_error( const std::string& error )
{
  if(mError)
    return mError->set_last_error(error);
  else
    return MB_FAILURE;
}


MBErrorCode MBWriteUtil::report_error( const char* error, ... )
{
  va_list args;
  va_start(args, error);
  MBErrorCode result = mError->set_last_error(error, args);
  va_end(args);
  return result;
}


MBErrorCode MBWriteUtil::get_adjacencies( MBEntityHandle entity,
                                          MBTag id_tag,
                                          std::vector<int>& adj )
{
  MBErrorCode rval;
  const MBEntityHandle* adj_array;
  int num_adj, id;

  TagServer* tag_server = mMB->tag_server();
 
    // Get handles of adjacent entities 
  rval = mMB->a_entity_factory()->get_adjacencies( entity, adj_array, num_adj );
  if (MB_SUCCESS != rval)
  {
    adj.clear();
    return rval;
  }
  
    // Append IDs of adjacent entities -- skip meshsets
  adj.resize( num_adj );  // pre-allocate space
  adj.clear();            // clear used space
  
  const MBEntityHandle* const end = adj_array + num_adj;
  for (const MBEntityHandle* iter = adj_array; iter != end; ++iter)
  {
    if (TYPE_FROM_HANDLE( *iter ) != MBENTITYSET)
    {
      rval = tag_server->get_data( id_tag, iter, 1, &id );
      if (MB_SUCCESS != rval)
        return rval;
      adj.push_back( id );
    }
  }
  
  return MB_SUCCESS;
}

