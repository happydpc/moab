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
#ifdef _DEBUG
// turn off warnings that say they debugging identifier has been truncated
// this warning comes up when using some STL containers
#pragma warning(disable : 4786)
#endif
#endif

#ifndef NETCDF_FILE
#  error Attempt to compile WriteSLAC with NetCDF disabled.
#endif

#include "WriteSLAC.hpp"

#include <utility>
#include <algorithm>
#include <time.h>
#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <assert.h>

#include "netcdf.hh"
#include "MBInterface.hpp"
#include "MBRange.hpp"
#include "MBCN.hpp"
#include "MBInternals.hpp"
#include "ExoIIUtil.hpp"
#include "MBTagConventions.hpp"
#include "MBWriteUtilIface.hpp"

#define INS_ID(stringvar, prefix, id) \
          sprintf(stringvar, prefix, id)

MBWriterIface* WriteSLAC::factory( MBInterface* iface )
  { return new WriteSLAC( iface ); }

WriteSLAC::WriteSLAC(MBInterface *impl) 
    : mbImpl(impl), ncFile(0), mCurrentMeshHandle(0)
{
  assert(impl != NULL);

  void* ptr = 0;
  impl->query_interface( "MBWriteUtilIface", &ptr );
  mWriteIface = reinterpret_cast<MBWriteUtilIface*>(ptr);

  // initialize in case tag_get_handle fails below
  //! get and cache predefined tag handles
  int dum_val = 0;
  MBErrorCode result = impl->tag_get_handle(MATERIAL_SET_TAG_NAME,  mMaterialSetTag);
  if (MB_TAG_NOT_FOUND == result)
    result = impl->tag_create(MATERIAL_SET_TAG_NAME, sizeof(int), MB_TAG_SPARSE, mMaterialSetTag,
                              &dum_val);
  
  result = impl->tag_get_handle(DIRICHLET_SET_TAG_NAME, mDirichletSetTag);
  if (MB_TAG_NOT_FOUND == result)
    result = impl->tag_create(DIRICHLET_SET_TAG_NAME, sizeof(int), MB_TAG_SPARSE, mDirichletSetTag,
                              &dum_val);
  
  result = impl->tag_get_handle(NEUMANN_SET_TAG_NAME,   mNeumannSetTag);
  if (MB_TAG_NOT_FOUND == result)
    result = impl->tag_create(NEUMANN_SET_TAG_NAME, sizeof(int), MB_TAG_SPARSE, mNeumannSetTag,
                              &dum_val);
  
  result = impl->tag_get_handle(HAS_MID_NODES_TAG_NAME, mHasMidNodesTag);
  if (MB_TAG_NOT_FOUND == result) {
    int dum_val_array[] = {0, 0, 0, 0};
    result = impl->tag_create(HAS_MID_NODES_TAG_NAME, 4*sizeof(int), MB_TAG_SPARSE, mHasMidNodesTag,
                              dum_val_array);
  }
  
  result = impl->tag_get_handle(GLOBAL_ID_TAG_NAME, mGlobalIdTag);
  if (MB_TAG_NOT_FOUND == result)
    result = impl->tag_create(GLOBAL_ID_TAG_NAME, sizeof(int), MB_TAG_SPARSE, mGlobalIdTag,
                              &dum_val);
  
  dum_val = -1;
  result = impl->tag_get_handle("__matSetIdTag", mMatSetIdTag);
  if (MB_TAG_NOT_FOUND == result)
    result = impl->tag_create("__matSetIdTag", sizeof(int), MB_TAG_DENSE, mMatSetIdTag,
                              &dum_val);
  

  impl->tag_create("WriteSLAC element mark", 1, MB_TAG_BIT, mEntityMark, NULL);

}

WriteSLAC::~WriteSLAC() 
{
  std::string iface_name = "MBWriteUtilIface";
  mbImpl->release_interface(iface_name, mWriteIface);

  mbImpl->tag_delete(mEntityMark);

  if (NULL != ncFile)
    delete ncFile;
}

void WriteSLAC::reset_matset(std::vector<WriteSLAC::MaterialSetData> &matset_info)
{
  std::vector<WriteSLAC::MaterialSetData>::iterator iter;
  
  for (iter = matset_info.begin(); iter != matset_info.end(); iter++)
  {
    delete (*iter).elements;
  }
}

MBErrorCode WriteSLAC::write_file(const char *file_name, 
                                  const bool overwrite,
                                  const FileOptions&,
                                  const MBEntityHandle *ent_handles,
                                  const int num_sets,
                                  const std::vector<std::string>&, 
                                  const MBTag*,
                                  int,
                                  int )
{
  assert(0 != mMaterialSetTag &&
         0 != mNeumannSetTag &&
         0 != mDirichletSetTag);

    // check the file name
  if (NULL == strstr(file_name, ".ncdf"))
    return MB_FAILURE;

  std::vector<MBEntityHandle> matsets, dirsets, neusets, entities;

  fileName = file_name;
  
    // separate into material sets, dirichlet sets, neumann sets

  if (num_sets == 0) {
      // default to all defined sets
    MBRange this_range;
    mbImpl->get_entities_by_type_and_tag(0, MBENTITYSET, &mMaterialSetTag, NULL, 1, this_range);
    std::copy(this_range.begin(), this_range.end(), std::back_inserter(matsets));
    this_range.clear();
    mbImpl->get_entities_by_type_and_tag(0, MBENTITYSET, &mDirichletSetTag, NULL, 1, this_range);
    std::copy(this_range.begin(), this_range.end(), std::back_inserter(dirsets));
    this_range.clear();
    mbImpl->get_entities_by_type_and_tag(0, MBENTITYSET, &mNeumannSetTag, NULL, 1, this_range);
    std::copy(this_range.begin(), this_range.end(), std::back_inserter(neusets));
  }
  else {
    int dummy;
    for (const MBEntityHandle *iter = ent_handles; iter < ent_handles+num_sets; iter++) 
    {
      if (MB_SUCCESS == mbImpl->tag_get_data(mMaterialSetTag, &(*iter), 1, &dummy))
        matsets.push_back(*iter);
      else if (MB_SUCCESS == mbImpl->tag_get_data(mDirichletSetTag, &(*iter), 1, &dummy))
        dirsets.push_back(*iter);
      else if (MB_SUCCESS == mbImpl->tag_get_data(mNeumannSetTag, &(*iter), 1, &dummy))
        neusets.push_back(*iter);
    }
  }
  
    // if there is nothing to write just return.
  if (matsets.empty() && dirsets.empty() && neusets.empty())
    return MB_FILE_WRITE_ERROR;

  std::vector<WriteSLAC::MaterialSetData> matset_info;
  std::vector<WriteSLAC::DirichletSetData> dirset_info;
  std::vector<WriteSLAC::NeumannSetData> neuset_info;

  MeshInfo mesh_info;
  
  matset_info.clear();
  if(gather_mesh_information(mesh_info, matset_info, neuset_info, dirset_info,
                             matsets, neusets, dirsets) != MB_SUCCESS)
  {
    reset_matset(matset_info);
    return MB_FAILURE;
  }


  // try to open the file after gather mesh info succeeds
  ncFile = new NcFile(file_name, overwrite ? NcFile::Replace : NcFile::New );
  if (NULL == ncFile) {
    reset_matset(matset_info);
    return MB_FAILURE;
  }

  if( initialize_file(mesh_info) != MB_SUCCESS)
  {
    reset_matset(matset_info);
    return MB_FAILURE;
  }

  if( write_nodes(mesh_info.num_nodes, mesh_info.nodes, mesh_info.num_dim) != MB_SUCCESS )
  {
    reset_matset(matset_info);
    return MB_FAILURE;
  }

  if( write_matsets(mesh_info, matset_info, neuset_info) )
  {
    reset_matset(matset_info);
    return MB_FAILURE;
  }

  return MB_SUCCESS;
}

MBErrorCode WriteSLAC::gather_mesh_information(MeshInfo &mesh_info,
                                               std::vector<WriteSLAC::MaterialSetData> &matset_info,
                                               std::vector<WriteSLAC::NeumannSetData> &neuset_info,
                                               std::vector<WriteSLAC::DirichletSetData> &dirset_info,
                                               std::vector<MBEntityHandle> &matsets,
                                               std::vector<MBEntityHandle> &neusets,
                                               std::vector<MBEntityHandle> &dirsets)
{

  std::vector<MBEntityHandle>::iterator vector_iter, end_vector_iter;

  mesh_info.num_nodes = 0;
  mesh_info.num_elements = 0;
  mesh_info.num_matsets = 0;
  
  int id = 0;

  vector_iter= matsets.begin();
  end_vector_iter = matsets.end();

  mesh_info.num_matsets = matsets.size();

  std::vector<MBEntityHandle> parent_meshsets;

  // clean out the bits for the element mark
  mbImpl->tag_delete(mEntityMark);
  mbImpl->tag_create("WriteSLAC element mark", 1, MB_TAG_BIT, mEntityMark, NULL);

  int highest_dimension_of_element_matsets = 0;

  for(vector_iter = matsets.begin(); vector_iter != matsets.end(); vector_iter++)
  {
       
    WriteSLAC::MaterialSetData matset_data;
    matset_data.elements = new MBRange;

    //for the purpose of qa records, get the parents of these matsets 
    if( mbImpl->get_parent_meshsets( *vector_iter, parent_meshsets ) != MB_SUCCESS )
      return MB_FAILURE;

    // get all Entity Handles in the mesh set
    MBRange dummy_range;
    mbImpl->get_entities_by_handle(*vector_iter, dummy_range, true );



    // wait a minute, we are doing some filtering here that doesn't make sense at this level  CJS

      // find the dimension of the last entity in this range
    MBRange::iterator entity_iter = dummy_range.end();
    entity_iter = dummy_range.end();
    entity_iter--;
    int this_dim = MBCN::Dimension(TYPE_FROM_HANDLE(*entity_iter));
    entity_iter = dummy_range.begin();
    while (entity_iter != dummy_range.end() &&
           MBCN::Dimension(TYPE_FROM_HANDLE(*entity_iter)) != this_dim)
      entity_iter++;
    
    if (entity_iter != dummy_range.end())
      std::copy(entity_iter, dummy_range.end(), mb_range_inserter(*(matset_data.elements)));

    assert(matset_data.elements->begin() == matset_data.elements->end() ||
           MBCN::Dimension(TYPE_FROM_HANDLE(*(matset_data.elements->begin()))) == this_dim);
    
    // get the matset's id
    if(mbImpl->tag_get_data(mMaterialSetTag, &(*vector_iter), 1, &id) != MB_SUCCESS ) {
      mWriteIface->report_error("Couldn't get matset id from a tag for an element matset.");
      return MB_FAILURE;
    }
    
    matset_data.id = id; 
    matset_data.number_attributes = 0;
 
     // iterate through all the elements in the meshset
    MBRange::iterator elem_range_iter, end_elem_range_iter;
    elem_range_iter = matset_data.elements->begin();
    end_elem_range_iter = matset_data.elements->end();

      // get the entity type for this matset, verifying that it's the same for all elements
      // THIS ASSUMES HANDLES SORT BY TYPE!!!
    MBEntityType entity_type = TYPE_FROM_HANDLE(*elem_range_iter);
    end_elem_range_iter--;
    if (entity_type != TYPE_FROM_HANDLE(*(end_elem_range_iter++))) {
      mWriteIface->report_error("Entities in matset %i not of common type", id);
      return MB_FAILURE;
    }

    int dimension = -1;
    if(entity_type == MBQUAD || entity_type == MBTRI)
      dimension = 3;   // output shells by default
    else if(entity_type == MBEDGE)
      dimension = 2;
    else
      dimension = MBCN::Dimension(entity_type);

    if( dimension > highest_dimension_of_element_matsets )
      highest_dimension_of_element_matsets = dimension;

    matset_data.moab_type = mbImpl->type_from_handle(*(matset_data.elements->begin()));
    if (MBMAXTYPE == matset_data.moab_type) return MB_FAILURE;
    
    std::vector<MBEntityHandle> tmp_conn;
    mbImpl->get_connectivity(&(*(matset_data.elements->begin())), 1, tmp_conn);
    matset_data.element_type = 
      ExoIIUtil::get_element_type_from_num_verts(tmp_conn.size(), entity_type, dimension);
    
    if (matset_data.element_type == EXOII_MAX_ELEM_TYPE) {
      mWriteIface->report_error("Element type in matset %i didn't get set correctly", id);
      return MB_FAILURE;
    }
    
    matset_data.number_nodes_per_element = ExoIIUtil::VerticesPerElement[matset_data.element_type];

    // number of nodes for this matset
    matset_data.number_elements = matset_data.elements->size();

    // total number of elements
    mesh_info.num_elements += matset_data.number_elements;

    // get the nodes for the elements
    mWriteIface->gather_nodes_from_elements(*matset_data.elements, mEntityMark, mesh_info.nodes);

    if(!neusets.empty())
    {
      // if there are neusets, keep track of which elements are being written out
      for(MBRange::iterator iter = matset_data.elements->begin(); 
          iter != matset_data.elements->end(); ++iter)
      {
        unsigned char bit = 0x1;
        mbImpl->tag_set_data(mEntityMark, &(*iter), 1, &bit);
      }
    }

    matset_info.push_back( matset_data );
  
  }
 

  //if user hasn't entered dimension, we figure it out
  if( mesh_info.num_dim == 0 )
  {
    //never want 1 or zero dimensions
    if( highest_dimension_of_element_matsets < 2 )
      mesh_info.num_dim = 3;
    else
      mesh_info.num_dim = highest_dimension_of_element_matsets;
  }

  MBRange::iterator range_iter, end_range_iter;
  range_iter = mesh_info.nodes.begin();
  end_range_iter = mesh_info.nodes.end();

  mesh_info.num_nodes = mesh_info.nodes.size(); 

  //------dirsets--------
  
  vector_iter= dirsets.begin();
  end_vector_iter = dirsets.end();

  for(; vector_iter != end_vector_iter; vector_iter++)
  {
    
    WriteSLAC::DirichletSetData dirset_data;
    dirset_data.id = 0;
    dirset_data.number_nodes = 0;

    // get the dirset's id
    if(mbImpl->tag_get_data(mDirichletSetTag,&(*vector_iter), 1,&id) != MB_SUCCESS) {
      mWriteIface->report_error("Couldn't get id tag for dirset %i", id);
      return MB_FAILURE;
    }
    
    dirset_data.id = id; 

    std::vector<MBEntityHandle> node_vector;
    //get the nodes of the dirset that are in mesh_info.nodes
    if( mbImpl->get_entities_by_handle(*vector_iter, node_vector, true) != MB_SUCCESS ) {
      mWriteIface->report_error("Couldn't get nodes in dirset %i", id);
      return MB_FAILURE;
    }

    std::vector<MBEntityHandle>::iterator iter, end_iter;
    iter = node_vector.begin();
    end_iter= node_vector.end();
 
    int j=0; 
    unsigned char node_marked = 0;
    MBErrorCode result;
    for(; iter != end_iter; iter++)
    {
      if (TYPE_FROM_HANDLE(*iter) != MBVERTEX) continue;
      result = mbImpl->tag_get_data(mEntityMark, &(*iter), 1, &node_marked);
      if (MB_SUCCESS != result) {
        mWriteIface->report_error("Couldn't get mark data.");
        return result;
      }
      
      if(node_marked == 0x1) dirset_data.nodes.push_back( *iter );    
      j++;
    } 
    
    dirset_data.number_nodes = dirset_data.nodes.size(); 
    dirset_info.push_back( dirset_data );
  }

  //------neusets--------
  vector_iter= neusets.begin();
  end_vector_iter = neusets.end();

  for(; vector_iter != end_vector_iter; vector_iter++)
  {
    WriteSLAC::NeumannSetData neuset_data;

    // get the neuset's id
    if(mbImpl->tag_get_data(mNeumannSetTag,&(*vector_iter), 1,&id) != MB_SUCCESS)
      return MB_FAILURE;

    neuset_data.id = id; 
    neuset_data.mesh_set_handle = *vector_iter; 
 
    //get the sides in two lists, one forward the other reverse; starts with forward sense
      // by convention
    MBRange forward_elems, reverse_elems;
    if(get_neuset_elems(*vector_iter, 0, forward_elems, reverse_elems) == MB_FAILURE)
      return MB_FAILURE;

    MBErrorCode result = get_valid_sides(forward_elems, 1, neuset_data);
    if (MB_SUCCESS != result) {
      mWriteIface->report_error("Couldn't get valid sides data.");
      return result;
    }
    result = get_valid_sides(reverse_elems, -1, neuset_data);
    if (MB_SUCCESS != result) {
      mWriteIface->report_error("Couldn't get valid sides data.");
      return result;
    }
    
    neuset_data.number_elements = neuset_data.elements.size(); 
    neuset_info.push_back( neuset_data );
  }

    // get information about interior/exterior tets/hexes, and mark matset ids
  return gather_interior_exterior(mesh_info, matset_info, neuset_info);
}

MBErrorCode WriteSLAC::get_valid_sides(MBRange &elems, const int sense,
                                       WriteSLAC::NeumannSetData &neuset_data) 
{
    // this is where we see if underlying element of side set element is included in output 

  unsigned char element_marked = 0;
  MBErrorCode result;
  for(MBRange::iterator iter = elems.begin(); iter != elems.end(); iter++)
  {
      // should insert here if "side" is a quad/tri on a quad/tri mesh
    result = mbImpl->tag_get_data(mEntityMark, &(*iter), 1, &element_marked);
    if (MB_SUCCESS != result) {
      mWriteIface->report_error("Couldn't get mark data.");
      return result;
    }
    
    if(element_marked == 0x1)
    {
      neuset_data.elements.push_back( *iter );

        // TJT TODO: the sense should really be # edges + 1or2
      neuset_data.side_numbers.push_back((sense == 1 ? 1 : 2));
    }
    else //then "side" is probably a quad/tri on a hex/tet mesh
    {
      std::vector<MBEntityHandle> parents;
      int dimension = MBCN::Dimension( TYPE_FROM_HANDLE(*iter));

        //get the adjacent parent element of "side"
      if( mbImpl->get_adjacencies( &(*iter), 1, dimension+1, false, parents) != MB_SUCCESS ) {
        mWriteIface->report_error("Couldn't get adjacencies for neuset.");
        return MB_FAILURE;
      }
       
      if(!parents.empty())     
      {
          //make sure the adjacent parent element will be output
        for(unsigned int k=0; k<parents.size(); k++)
        {
          result = mbImpl->tag_get_data(mEntityMark, &(parents[k]), 1, &element_marked);
          if (MB_SUCCESS != result) {
            mWriteIface->report_error("Couldn't get mark data.");
            return result;
          }
        
          int side_no, this_sense, this_offset;
          if(element_marked == 0x1 &&
             mbImpl->side_number(parents[k], *iter, side_no, 
                                  this_sense, this_offset) == MB_SUCCESS &&
             this_sense == sense) {
            neuset_data.elements.push_back(parents[k]);
            neuset_data.side_numbers.push_back(side_no+1);
            break;
          }
        }
      }
      else
      {
        mWriteIface->report_error("No parent element exists for element in neuset %i", neuset_data.id);
        return MB_FAILURE;
      }
    }
  }

  return MB_SUCCESS;
}

MBErrorCode WriteSLAC::write_nodes(const int num_nodes, const MBRange& nodes, const int dimension)
{
  //see if should transform coordinates
  MBErrorCode result;
  MBTag trans_tag;
  result = mbImpl->tag_get_handle( MESH_TRANSFORM_TAG_NAME, trans_tag);
  bool transform_needed = true;
  if( result == MB_TAG_NOT_FOUND )
    transform_needed = false;

  int num_coords_to_fill = transform_needed ? 3 : dimension;

  std::vector<double*> coord_arrays(3);
  coord_arrays[0] = new double[num_nodes];
  coord_arrays[1] = new double[num_nodes];
  coord_arrays[2] = NULL;

  if( num_coords_to_fill == 3 ) 
    coord_arrays[2] = new double[num_nodes];
 
  result = mWriteIface->get_node_arrays(dimension, num_nodes, nodes, 
                                        mGlobalIdTag, 0, coord_arrays);
  if(result != MB_SUCCESS)
  {
    delete [] coord_arrays[0];
    delete [] coord_arrays[1];
    if(coord_arrays[2]) delete [] coord_arrays[2];
    return result;
  }

  if( transform_needed )
  {
    double trans_matrix[16]; 
    result = mbImpl->tag_get_data( trans_tag, NULL, 0, trans_matrix ); 
    if (MB_SUCCESS != result) {
      mWriteIface->report_error("Couldn't get transform data.");
      return result;
    }
      
    for( int i=0; i<num_nodes; i++)
    {

      double vec1[3];
      double vec2[3];

      vec2[0] =  coord_arrays[0][i];
      vec2[1] =  coord_arrays[1][i];
      vec2[2] =  coord_arrays[2][i];

      for( int row=0; row<3; row++ )
      {
        vec1[row] = 0.0;
        for( int col = 0; col<3; col++ )
        {
          vec1[row] += ( trans_matrix[ (row*4)+col ] * vec2[col] );
        }
      }

      coord_arrays[0][i] = vec1[0];
      coord_arrays[1][i] = vec1[1];
      coord_arrays[2][i] = vec1[2];

    }
  }


  // write the nodes 
  NcVar *coord = ncFile->get_var("coords");
  if (NULL == coord) return MB_FAILURE;
  bool status = coord->put(coord_arrays[0], num_nodes, 1) != 0;
  if (!status)
    return MB_FAILURE;
  status = coord->set_cur(0, 1) != 0;
  if (!status)
    return MB_FAILURE;
  status = coord->put(&(coord_arrays[1][0]), num_nodes, 1) != 0;
  if (!status)
    return MB_FAILURE;
  status = coord->set_cur(0, 2) != 0;
  if (!status)
    return MB_FAILURE;
  status = coord->put(&(coord_arrays[2][0]), num_nodes, 1) != 0;
  if (!status)
    return MB_FAILURE;
  
  delete [] coord_arrays[0];
  delete [] coord_arrays[1];
  if(coord_arrays[2]) 
    delete [] coord_arrays[2];

  return MB_SUCCESS;

}


MBErrorCode WriteSLAC::gather_interior_exterior(MeshInfo &mesh_info,
                                                std::vector<WriteSLAC::MaterialSetData> &matset_data,
                                                std::vector<WriteSLAC::NeumannSetData> &neuset_data)
{
    // need to assign a tag with the matset id
  MBTag matset_id_tag;
  unsigned int i;
  int dum = -1;
  MBErrorCode result = mbImpl->tag_create("__matset_id", 4, MB_TAG_DENSE, matset_id_tag, &dum);
  if (MB_SUCCESS != result) return result;

  MBRange::iterator rit;
  mesh_info.num_int_hexes = mesh_info.num_int_tets = 0;
  
  for(i=0; i< matset_data.size(); i++)
  {
    WriteSLAC::MaterialSetData matset = matset_data[i];
    if (matset.moab_type == MBHEX)
      mesh_info.num_int_hexes += matset.elements->size();
    
    else if (matset.moab_type == MBTET)
      mesh_info.num_int_tets += matset.elements->size();
    else {
      std::cout << "WriteSLAC doesn't support elements of type " 
                << MBCN::EntityTypeName(matset.moab_type) << std::endl;
      continue;
    }
    
    for (rit = matset.elements->begin(); rit != matset.elements->end(); rit++) {
      result = mbImpl->tag_set_data(mMatSetIdTag, &(*rit), 1, &(matset.id));
      if (MB_SUCCESS != result) return result;
    }
  }

    // now go through the neumann sets, pulling out the hexes with faces on the 
    // boundary
  std::vector<MBEntityHandle>::iterator vit;
  for(i=0; i< neuset_data.size(); i++)
  {
    WriteSLAC::NeumannSetData neuset = neuset_data[i];
    for (vit = neuset.elements.begin(); vit != neuset.elements.end(); vit++) {
      if (TYPE_FROM_HANDLE(*vit) == MBHEX) mesh_info.bdy_hexes.insert(*vit);
      else if (TYPE_FROM_HANDLE(*vit) == MBTET) mesh_info.bdy_tets.insert(*vit);
    }
  }

    // now we have the number of bdy hexes and tets, we know how many interior ones
    // there are too
  mesh_info.num_int_hexes -= mesh_info.bdy_hexes.size();
  mesh_info.num_int_tets -= mesh_info.bdy_tets.size();

  return MB_SUCCESS;
}


MBErrorCode WriteSLAC::write_matsets(MeshInfo &mesh_info,
                                     std::vector<WriteSLAC::MaterialSetData> &matset_data,
                                     std::vector<WriteSLAC::NeumannSetData> &neuset_data)
{

  unsigned int i;
  std::vector<int> connect;
  const MBEntityHandle *connecth;
  int num_connecth;
  MBErrorCode result;
  
    // first write the interior hexes
  NcVar *hex_conn = NULL;
  if (mesh_info.bdy_hexes.size() != 0 || mesh_info.num_int_hexes != 0) {
    const char *hex_name = "hexahedron_interior";
    hex_conn = ncFile->get_var(hex_name);
    if (NULL == hex_conn) return MB_FAILURE;
  }
  connect.reserve(13);
  MBRange::iterator rit;

  int elem_num = 0;
  WriteSLAC::MaterialSetData matset;
  for (i = 0; i < matset_data.size(); i++) {
    matset = matset_data[i];
    if (matset.moab_type != MBHEX) continue;
    
    int id = matset.id;
    connect[0] = id;

    for (rit = matset.elements->begin(); rit != matset.elements->end(); rit++) {
        // skip if it's on the bdy
      if (mesh_info.bdy_hexes.find(*rit) != mesh_info.bdy_hexes.end()) continue;
      
        // get the connectivity of this element
      result = mbImpl->get_connectivity(*rit, connecth, num_connecth);
      if (MB_SUCCESS != result) return result;
      
        // get the vertex ids
      result = mbImpl->tag_get_data(mGlobalIdTag, connecth, num_connecth, &connect[1]);
      if (MB_SUCCESS != result) return result;
      
        // put the variable at the right position
      hex_conn->set_cur(elem_num++, 0);
      
        // write the data
      NcBool err = hex_conn->put(&connect[0], 1, 9);
      if(!err)
        return MB_FAILURE;
    }
  }

  NcVar *tet_conn = NULL;
  if (mesh_info.bdy_tets.size() != 0 || mesh_info.num_int_tets != 0) {
    const char *tet_name = "tetrahedron_interior";
    tet_conn = ncFile->get_var(tet_name);
    if (NULL == tet_conn) return MB_FAILURE;
  }

    // now the interior tets
  elem_num = 0;
  for (i = 0; i < matset_data.size(); i++) {
    matset = matset_data[i];
    if (matset.moab_type != MBTET) continue;
    
    int id = matset.id;
    connect[0] = id;

    for (rit = matset.elements->begin(); rit != matset.elements->end(); rit++) {
        // skip if it's on the bdy
      if (mesh_info.bdy_tets.find(*rit) != mesh_info.bdy_tets.end()) continue;
      
        // get the connectivity of this element
      result = mbImpl->get_connectivity(*rit, connecth, num_connecth);
      if (MB_SUCCESS != result) return result;
      
        // get the vertex ids
      result = mbImpl->tag_get_data(mGlobalIdTag, connecth, num_connecth, &connect[1]);
      if (MB_SUCCESS != result) return result;
      
        // put the variable at the right position
      tet_conn->set_cur(elem_num, 0);
      
        // write the data
      NcBool err = tet_conn->put(&connect[0], 1, 5);
      if(!err)
        return MB_FAILURE;
    }
  }
  
    // now the exterior hexes
  if (mesh_info.bdy_hexes.size() != 0) {
    const char *hex_name = "hexahedron_exterior";
    hex_conn = ncFile->get_var(hex_name);
    if (NULL == hex_conn) return MB_FAILURE;

    connect.reserve(15);
    int elem_num = 0;

      // write the elements
    for (rit = mesh_info.bdy_hexes.begin(); rit != mesh_info.bdy_hexes.end(); rit++) {
      
        // get the material set for this hex
        result = mbImpl->tag_get_data(mMatSetIdTag, &(*rit), 1, &connect[0]);
        if (MB_SUCCESS != result) return result;

          // get the connectivity of this element
        result = mbImpl->get_connectivity(*rit, connecth, num_connecth);
        if (MB_SUCCESS != result) return result;
      
          // get the vertex ids
        result = mbImpl->tag_get_data(mGlobalIdTag, connecth, num_connecth, &connect[1]);
        if (MB_SUCCESS != result) return result;

          // preset side numbers
        for (i = 9; i < 15; i++)
          connect[i] = -1;

          // now write the side numbers
        for (i = 0; i < neuset_data.size(); i++) {
          std::vector<MBEntityHandle>::iterator vit = 
            std::find(neuset_data[i].elements.begin(), neuset_data[i].elements.end(), *rit);
          while (vit != neuset_data[i].elements.end()) {
              // have a side - get the side # and put in connect array
            int side_no = neuset_data[i].side_numbers[vit-neuset_data[i].elements.begin()];
            connect[9+side_no] = neuset_data[i].id;
            vit++;
            vit = std::find(vit, neuset_data[i].elements.end(), *rit);
          }
        }
      
          // put the variable at the right position
        hex_conn->set_cur(elem_num, 0);
      
          // write the data
        NcBool err = hex_conn->put(&connect[0], 1, 15);
        if(!err)
          return MB_FAILURE;
    }
  }

    // now the exterior tets
  if (mesh_info.bdy_tets.size() != 0) {
    const char *tet_name = "tetrahedron_exterior";
    tet_conn = ncFile->get_var(tet_name);
    if (NULL == tet_conn) return MB_FAILURE;

    connect.reserve(9);
    int elem_num = 0;

      // write the elements
    for (rit = mesh_info.bdy_tets.begin(); rit != mesh_info.bdy_tets.end(); rit++) {
      
        // get the material set for this tet
        result = mbImpl->tag_get_data(mMatSetIdTag, &(*rit), 1, &connect[0]);
        if (MB_SUCCESS != result) return result;

          // get the connectivity of this element
        result = mbImpl->get_connectivity(*rit, connecth, num_connecth);
        if (MB_SUCCESS != result) return result;
      
          // get the vertex ids
        result = mbImpl->tag_get_data(mGlobalIdTag, connecth, num_connecth, &connect[1]);
        if (MB_SUCCESS != result) return result;

          // preset side numbers
        for (i = 5; i < 9; i++)
          connect[i] = -1;

          // now write the side numbers
        for (i = 0; i < neuset_data.size(); i++) {
          std::vector<MBEntityHandle>::iterator vit = 
            std::find(neuset_data[i].elements.begin(), neuset_data[i].elements.end(), *rit);
          while (vit != neuset_data[i].elements.end()) {
              // have a side - get the side # and put in connect array
            int side_no = neuset_data[i].side_numbers[vit-neuset_data[i].elements.begin()];
            connect[5+side_no] = neuset_data[i].id;
            vit++;
            vit = std::find(vit, neuset_data[i].elements.end(), *rit);
          }
        }
      
          // put the variable at the right position
        tet_conn->set_cur(elem_num, 0);
      
          // write the data
        NcBool err = tet_conn->put(&connect[0], 1, 9);
        if(!err)
          return MB_FAILURE;
    }
  }

  return MB_SUCCESS;
}

MBErrorCode WriteSLAC::initialize_file(MeshInfo &mesh_info)
{
    // perform the initializations

  NcDim *coord_size, *ncoords;
    // initialization to avoid warnings on linux
  NcDim *hexinterior = NULL, *hexinteriorsize, *hexexterior = NULL, *hexexteriorsize;
  NcDim *tetinterior = NULL, *tetinteriorsize, *tetexterior = NULL, *tetexteriorsize;
  
  if (!(coord_size = ncFile->add_dim("coord_size", (long)mesh_info.num_dim)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define number of dimensions");
    return (MB_FAILURE);
  }

  if (!(ncoords = ncFile->add_dim("ncoords", (long)mesh_info.num_nodes)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define number of nodes");
    return (MB_FAILURE);
  }

  if (0 != mesh_info.num_int_hexes &&
      !(hexinterior = ncFile->add_dim("hexinterior", (long)mesh_info.num_int_hexes)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define number of interior hex elements");
    return (MB_FAILURE);
  }

  if (!(hexinteriorsize = ncFile->add_dim("hexinteriorsize", (long)9)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define interior hex element size");
    return (MB_FAILURE);
  }

  if (0 != mesh_info.bdy_hexes.size() &&
      !(hexexterior = ncFile->add_dim("hexexterior", (long)mesh_info.bdy_hexes.size())))
  {
    mWriteIface->report_error("WriteSLAC: failed to define number of exterior hex elements");
    return (MB_FAILURE);
  }

  if (!(hexexteriorsize = ncFile->add_dim("hexexteriorsize", (long)15)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define exterior hex element size");
    return (MB_FAILURE);
  }

  if (0 != mesh_info.num_int_tets &&
      !(tetinterior = ncFile->add_dim("tetinterior", (long)mesh_info.num_int_tets)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define number of interior tet elements");
    return (MB_FAILURE);
  }

  if (!(tetinteriorsize = ncFile->add_dim("tetinteriorsize", (long)5)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define interior tet element size");
    return (MB_FAILURE);
  }

  if (0 != mesh_info.bdy_tets.size() &&
      !(tetexterior = ncFile->add_dim("tetexterior", (long)mesh_info.bdy_tets.size())))
  {
    mWriteIface->report_error("WriteSLAC: failed to define number of exterior tet elements");
    return (MB_FAILURE);
  }

  if (!(tetexteriorsize = ncFile->add_dim("tetexteriorsize", (long)9)))
  {
    mWriteIface->report_error("WriteSLAC: failed to define exterior tet element size");
    return (MB_FAILURE);
  }

/* ...and some variables */

  if (0 != mesh_info.num_int_hexes &&
      NULL == ncFile->add_var("hexahedron_interior", ncLong, 
                              hexinterior, hexinteriorsize))
  {
    mWriteIface->report_error("WriteSLAC: failed to create connectivity array for interior hexes.");
    return (MB_FAILURE);
  }

  if (0 != mesh_info.bdy_hexes.size() &&
      NULL == ncFile->add_var("hexahedron_exterior", ncLong, 
                              hexexterior, hexexteriorsize))
  {
    mWriteIface->report_error("WriteSLAC: failed to create connectivity array for exterior hexes.");
    return (MB_FAILURE);
  }

  if (0 != mesh_info.num_int_tets &&
      NULL == ncFile->add_var("tetrahedron_interior", ncLong, 
                              tetinterior, tetinteriorsize))
  {
    mWriteIface->report_error("WriteSLAC: failed to create connectivity array for interior tets.");
    return (MB_FAILURE);
  }

  if (0 != mesh_info.bdy_tets.size() &&
      NULL == ncFile->add_var("tetrahedron_exterior", ncLong, 
                              tetexterior, tetexteriorsize))
  {
    mWriteIface->report_error("WriteSLAC: failed to create connectivity array for exterior tets.");
    return (MB_FAILURE);
  }

/* node coordinate arrays: */

   if (ncFile->add_var("coords", ncDouble, ncoords, coord_size) == NULL)
   {
     mWriteIface->report_error("WriteSLAC: failed to define node coordinate array");
     return (MB_FAILURE);
   }

   return MB_SUCCESS;
}


MBErrorCode WriteSLAC::open_file(const char* filename)
{
   // not a valid filname
   if(strlen((const char*)filename) == 0)
   {
     mWriteIface->report_error("Output filename not specified");
      return MB_FAILURE;
   }

   ncFile = new NcFile(filename, NcFile::Replace);

   // file couldn't be opened
   if(ncFile == NULL)
   {
     mWriteIface->report_error("Cannot open %s", filename);
     return MB_FAILURE;
   }
   return MB_SUCCESS;
}

MBErrorCode WriteSLAC::get_neuset_elems(MBEntityHandle neuset, int current_sense,
                                        MBRange &forward_elems, MBRange &reverse_elems) 
{
  MBRange ss_elems, ss_meshsets;

    // get the sense tag; don't need to check return, might be an error if the tag
    // hasn't been created yet
  MBTag sense_tag = 0;
  mbImpl->tag_get_handle("SENSE", sense_tag);

    // get the entities in this set
  MBErrorCode result = mbImpl->get_entities_by_handle(neuset, ss_elems, true);
  if (MB_FAILURE == result) return result;
  
    // now remove the meshsets into the ss_meshsets; first find the first meshset,
  MBRange::iterator range_iter = ss_elems.begin();
  while (TYPE_FROM_HANDLE(*range_iter) != MBENTITYSET && range_iter != ss_elems.end())
    range_iter++;
  
    // then, if there are some, copy them into ss_meshsets and erase from ss_elems
  if (range_iter != ss_elems.end()) {
    std::copy(range_iter, ss_elems.end(), mb_range_inserter(ss_meshsets));
    ss_elems.erase(range_iter, ss_elems.end());
  }
  

    // ok, for the elements, check the sense of this set and copy into the right range
    // (if the sense is 0, copy into both ranges)

    // need to step forward on list until we reach the right dimension
  MBRange::iterator dum_it = ss_elems.end();
  dum_it--;
  int target_dim = MBCN::Dimension(TYPE_FROM_HANDLE(*dum_it));
  dum_it = ss_elems.begin();
  while (target_dim != MBCN::Dimension(TYPE_FROM_HANDLE(*dum_it)) &&
         dum_it != ss_elems.end()) 
    dum_it++;

  if (current_sense == 1 || current_sense == 0)
    std::copy(dum_it, ss_elems.end(), mb_range_inserter(forward_elems));
  if (current_sense == -1 || current_sense == 0)
    std::copy(dum_it, ss_elems.end(), mb_range_inserter(reverse_elems));
  
    // now loop over the contained meshsets, getting the sense of those and calling this
    // function recursively
  for (range_iter = ss_meshsets.begin(); range_iter != ss_meshsets.end(); range_iter++) {

      // first get the sense; if it's not there, by convention it's forward
    int this_sense;
    if (0 == sense_tag ||
        MB_FAILURE == mbImpl->tag_get_data(sense_tag, &(*range_iter), 1, &this_sense))
      this_sense = 1;
      
      // now get all the entities on this meshset, with the proper (possibly reversed) sense
    get_neuset_elems(*range_iter, this_sense*current_sense,
                      forward_elems, reverse_elems);
  }
  
  return result;
}


  