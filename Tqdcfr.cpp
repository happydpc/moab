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

#include "Tqdcfr.hpp"
#include "MBCore.hpp"
#include "MBRange.hpp"
#include "MBReadUtilIface.hpp"
#include "GeomTopoTool.hpp"
#include "MBTagConventions.hpp"
#include <assert.h>

const bool debug = false;
const int ACIS_DIMS[] = {-1, 3, -1, 2, -1, -1, 1, 0, -1, -1};
const char default_acis_dump_file[] = "dumped_acis.sat";
const char acis_dump_file_tag_name[] = "__ACISDumpFile";
const char Tqdcfr::geom_categories[][CATEGORY_TAG_NAME_LENGTH] = 
{"Vertex\0", "Curve\0", "Surface\0", "Volume\0"};
const MBEntityType Tqdcfr::group_type_to_mb_type[] = {
  MBENTITYSET, MBENTITYSET, MBENTITYSET, // group, body, volume
  MBENTITYSET, MBENTITYSET, MBENTITYSET, // surface, curve, vertex
  MBHEX, MBTET, MBPYRAMID, MBQUAD, MBTRI, MBEDGE, MBVERTEX};
const MBEntityType Tqdcfr::block_type_to_mb_type[] = {
  MBVERTEX,
  MBEDGE, MBEDGE, MBEDGE, MBEDGE, MBEDGE, MBEDGE, MBEDGE, MBEDGE, MBEDGE, MBEDGE,
  MBTRI, MBTRI, MBTRI, MBTRI, MBTRI, MBTRI, MBTRI, MBTRI, 
  MBQUAD, MBQUAD, MBQUAD, MBQUAD, MBQUAD, 
  MBTET, MBTET, MBTET, MBTET, MBTET, 
  MBPYRAMID, MBPYRAMID, MBPYRAMID, MBPYRAMID, MBPYRAMID, 
  MBHEX, MBHEX, MBHEX, MBHEX, MBHEX, MBHEX, MBMAXTYPE
};

// mapping from mesh packet type to moab type
const MBEntityType Tqdcfr::mp_type_to_mb_type[] = {
  MBHEX, MBHEX, MBHEX, MBHEX, MBHEX, MBHEX, MBHEX, MBHEX, 
  MBTET, MBTET, MBTET, MBTET, MBTET, MBTET, MBTET, MBTET, 
  MBPYRAMID, MBPYRAMID, MBPYRAMID, MBPYRAMID, 
  MBQUAD, MBQUAD, MBQUAD, MBQUAD, 
  MBTRI, MBTRI, MBTRI, MBTRI, 
  MBEDGE, MBEDGE, MBVERTEX
};

const int Tqdcfr::cub_elem_num_verts[] = {
  1, // sphere
  2, 2, 3, // bars
  2, 2, 3, // beams
  2, 2, 3, // truss
  2, // spring
  3, 3, 6, 7, // tris
  3, 3, 6, 7, // trishells
  4, 4, 8, 9, // shells
  4, 4, 5, 8, 9, // quads
  4, 4, 8, 10, 14, // tets
  5, 5, 8, 13, 18, // pyramids
  8, 8, 9, 20, 27, 12, // hexes (incl. hexshell at end)
  0};

char *Tqdcfr::BLOCK_NODESET_OFFSET_TAG_NAME = "BLOCK_NODESET_OFFSET";
char *Tqdcfr::BLOCK_SIDESET_OFFSET_TAG_NAME = "BLOCK_SIDESET_OFFSET";

#define RR if (MB_SUCCESS != result) return result

// acis dimensions for each entity type, to match
// enum {BODY, LUMP, SHELL, FACE, LOOP, COEDGE, EDGE, VERTEX, ATTRIB, UNKNOWN} 

MBReaderIface* Tqdcfr::factory( MBInterface* iface )
{ return new Tqdcfr( iface ); }

Tqdcfr::Tqdcfr(MBInterface *impl) 
    : cubFile(NULL), globalIdTag(0), cubIdTag(0), geomTag(0), uniqueIdTag(0), groupTag(0), 
      blockTag(0), nsTag(0), ssTag(0), attribVectorTag(0), entityNameTag(0),
      categoryTag(0), instance(this)
{
  assert(NULL != impl);
  mdbImpl = impl;
  std::string iface_name = "MBReadUtilIface";
  impl->query_interface(iface_name, reinterpret_cast<void**>(&readUtilIface));
  assert(NULL != readUtilIface);

  currNodeIdOffset = -1;
  for (MBEntityType this_type = MBVERTEX; this_type < MBMAXTYPE; this_type++)
    currElementIdOffset[this_type] = -1;

  mdbImpl->tag_get_handle(MATERIAL_SET_TAG_NAME, blockTag);
  mdbImpl->tag_get_handle(DIRICHLET_SET_TAG_NAME, nsTag);
  mdbImpl->tag_get_handle(NEUMANN_SET_TAG_NAME, ssTag);

  MBErrorCode result = mdbImpl->tag_get_handle("cubIdTag", cubIdTag);
  if (MB_SUCCESS != result || MB_TAG_NOT_FOUND == result) {
    int default_val = -1;
    result = mdbImpl->tag_create("cubIdTag", 4, MB_TAG_DENSE, 
                                 cubIdTag, &default_val);
    assert (MB_SUCCESS == result);
  }

  cubMOABVertexMap = NULL;
}

Tqdcfr::~Tqdcfr() 
{
  std::string iface_name = "MBReadUtilIface";
  mdbImpl->release_interface(iface_name, readUtilIface);

  if (NULL != cubMOABVertexMap) delete cubMOABVertexMap;
  
  if (0 != cubIdTag) mdbImpl->tag_delete(cubIdTag);
}

  
MBErrorCode Tqdcfr::load_file(const char *file_name,
                              const int*, const int) 
{
  MBErrorCode result;
  
    // open file
  cubFile = fopen(file_name, "rb");
  if (NULL == cubFile) return MB_FAILURE;
  
    // verify magic string
  FREADC(4);
  if (!(char_buf[0] == 'C' && char_buf[1] == 'U' && 
        char_buf[2] == 'B' && char_buf[3] == 'E')) 
    return MB_FAILURE;

    // ***********************
    // read model header type information...
    // ***********************
  if (debug) std::cout << "Reading file header." << std::endl;
  result = read_file_header(); RR;

  if (debug) std::cout << "Reading model entries." << std::endl;
  result = read_model_entries(); RR;
  
    // read model metadata
  if (debug) std::cout << "Reading model metadata." << std::endl;
  result = read_meta_data(fileTOC.modelMetaDataOffset, modelMetaData); RR;

  double data_version;
  int md_index = modelMetaData.get_md_entry(2, "DataVersion");
  if (-1 == md_index) data_version = 1.0;
  else data_version = modelMetaData.metadataEntries[md_index].mdDblValue;
  
    // ***********************
    // read mesh...
    // ***********************
  int index = find_model(mesh); 
  if (-1 == index) return MB_FAILURE;
  ModelEntry *mesh_model = &modelEntries[index];
  
    // first the header & metadata info
  if (debug) std::cout << "Reading mesh model header and metadata." << std::endl;
  result = mesh_model->read_header_info(this, data_version); RR;
  result = mesh_model->read_metadata_info(this); RR;

    // now read in mesh for each geometry entity
  for (int gindex = 0; 
       gindex < mesh_model->feModelHeader.geomArray.numEntities;
       gindex++) {
    Tqdcfr::GeomHeader *geom_header = &mesh_model->feGeomH[gindex];

      // read nodes
    if (debug) std::cout << "Reading geom index " << gindex << " mesh: nodes... ";
    result = read_nodes(gindex, mesh_model, geom_header); RR;
    
      // read elements
    if (debug) std::cout << "elements... ";
    result = read_elements(mesh_model, geom_header); RR;
    if (debug) std::cout << std::endl;
  }

    // ***********************
    // read acis records...
    // ***********************
  result = read_acis_records(); RR;

    // ***********************
    // read groups...
    // ***********************
  if (debug) std::cout << "Reading groups... ";
  for (int grindex = 0; 
       grindex < mesh_model->feModelHeader.groupArray.numEntities;
       grindex++) {
    GroupHeader *group_header = &mesh_model->feGroupH[grindex];
    result = read_group(grindex, mesh_model, group_header); RR;
  }
  if (debug) std::cout << mesh_model->feModelHeader.groupArray.numEntities 
                       << " read successfully." << std::endl;;
  
    // ***********************
    // read blocks...
    // ***********************
  if (debug) std::cout << "Reading blocks... ";
  for (int blindex = 0; 
       blindex < mesh_model->feModelHeader.blockArray.numEntities;
       blindex++) {
    BlockHeader *block_header = &mesh_model->feBlockH[blindex];
    result = read_block(data_version, mesh_model, block_header); RR;
  }
  if (debug) std::cout << mesh_model->feModelHeader.blockArray.numEntities 
                       << " read successfully." << std::endl;;
  

    // ***********************
    // read nodesets...
    // ***********************
  if (debug) std::cout << "Reading nodesets... ";
  for (int nsindex = 0; 
       nsindex < mesh_model->feModelHeader.nodesetArray.numEntities;
       nsindex++) {
    NodesetHeader *nodeset_header = &mesh_model->feNodeSetH[nsindex];
    result = read_nodeset(mesh_model, nodeset_header); RR;
  }
  if (debug) std::cout << mesh_model->feModelHeader.nodesetArray.numEntities 
                       << " read successfully." << std::endl;;

    // ***********************
    // read sidesets...
    // ***********************
  if (debug) std::cout << "Reading sidesets...";
  for (int ssindex = 0; 
       ssindex < mesh_model->feModelHeader.sidesetArray.numEntities;
       ssindex++) {
    SidesetHeader *sideset_header = &mesh_model->feSideSetH[ssindex];
    result = read_sideset(data_version, mesh_model, sideset_header); RR;
  }
  if (debug) std::cout << mesh_model->feModelHeader.sidesetArray.numEntities 
                       << " read successfully." << std::endl;;

  if (debug) {
    std::cout << "Read the following mesh:" << std::endl;
    std::string dum;
    mdbImpl->list_entities(0, 0);
  }

    // **************************
    // restore geometric topology
    // **************************
  GeomTopoTool gtt(mdbImpl);
  result = gtt.restore_topology();

    // convert blocks to nodesets/sidesets if tag is set
  result = convert_nodesets_sidesets();
  
  return result;
}

MBErrorCode Tqdcfr::convert_nodesets_sidesets() 
{

    // look first for the nodeset and sideset offset flags; if they're not
    // set, we don't need to convert
  int nodeset_offset, sideset_offset;
  MBTag tmp_tag;
  MBErrorCode result = mdbImpl->tag_get_handle(BLOCK_NODESET_OFFSET_TAG_NAME,
                                               tmp_tag);
  if (MB_SUCCESS != result) nodeset_offset = 0;
  else {
    result = mdbImpl->tag_get_data(tmp_tag, 0, 0, &nodeset_offset);
    if (MB_SUCCESS != result) return result;
  }

  result = mdbImpl->tag_get_handle(BLOCK_SIDESET_OFFSET_TAG_NAME,
                                   tmp_tag);
  if (MB_SUCCESS != result) sideset_offset = 0;
  else {
    result = mdbImpl->tag_get_data(tmp_tag, 0, 0, &sideset_offset);
    if (MB_SUCCESS != result) return result;
  }

  if (0 == nodeset_offset && 0 == sideset_offset) return MB_SUCCESS;

    // look for all blocks
  MBRange blocks;
  result = mdbImpl->get_entities_by_type_and_tag(0, MBENTITYSET,
                                                 &blockTag, NULL, 1,
                                                 blocks);
  if (MB_SUCCESS != result || blocks.empty()) return result;
  
    // get the id tag for them
  std::vector<int> block_ids(blocks.size());
  result = mdbImpl->tag_get_data(globalIdTag, blocks, &block_ids[0]);
  if (MB_SUCCESS != result) return result;

  int i = 0;
  MBRange::iterator rit = blocks.begin();
  MBRange new_nodesets, new_sidesets;
  std::vector<int> new_nodeset_ids, new_sideset_ids;
  for (; rit != blocks.end(); i++, rit++) {
    if (0 != nodeset_offset && block_ids[i] >= nodeset_offset && 
        (nodeset_offset > sideset_offset || block_ids[i] < sideset_offset)) {
        // this is a nodeset
      new_nodesets.insert(*rit);
      new_nodeset_ids.push_back(block_ids[i]);
    }
    else if (0 != sideset_offset && block_ids[i] >= sideset_offset && 
             (sideset_offset > nodeset_offset || block_ids[i] < nodeset_offset)) {
        // this is a sideset
      new_sidesets.insert(*rit);
      new_sideset_ids.push_back(block_ids[i]);
    }
  }

    // ok, have the new nodesets and sidesets; now remove the block tags, and
    // add nodeset and sideset tags
  MBErrorCode tmp_result = MB_SUCCESS;
  if (0 != nodeset_offset) {
    if (0 == nsTag) {
      int default_val = 0;
      tmp_result = instance->mdbImpl->tag_create(DIRICHLET_SET_TAG_NAME, 4, MB_TAG_SPARSE, 
                                                 instance->nsTag, &default_val);
      if (MB_SUCCESS != tmp_result) result = tmp_result;
    }
    if (MB_SUCCESS == tmp_result)
      tmp_result = mdbImpl->tag_set_data(nsTag, new_nodesets, 
                                         &new_nodeset_ids[0]);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
    tmp_result = mdbImpl->tag_delete_data(blockTag, new_nodesets);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
  }
  if (0 != sideset_offset) {
    if (0 == ssTag) {
      int default_val = 0;
      tmp_result = instance->mdbImpl->tag_create(NEUMANN_SET_TAG_NAME, 4, MB_TAG_SPARSE, 
                                                 instance->ssTag, &default_val);
      if (MB_SUCCESS != tmp_result) result = tmp_result;
    }
    if (MB_SUCCESS == tmp_result) 
      tmp_result = mdbImpl->tag_set_data(ssTag, new_sidesets, 
                                         &new_sideset_ids[0]);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
    tmp_result = mdbImpl->tag_delete_data(blockTag, new_sidesets);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
  }

  return result;
}

MBErrorCode Tqdcfr::read_nodeset(Tqdcfr::ModelEntry *model,
                                 Tqdcfr::NodesetHeader *nodeseth)  
{
  if (nodeseth->memTypeCt == 0) return MB_SUCCESS;

    // position file
  FSEEK(model->modelOffset+nodeseth->memOffset);
  
    // read ids for each entity type
  int this_type, num_ents;
  std::vector<MBEntityHandle> ns_entities, excl_entities;
  for (int i = 0; i < nodeseth->memTypeCt; i++) {
      // get how many and what type
    FREADI(2);
    this_type = int_buf[0];
    num_ents = int_buf[1];

      // now get the ids
    FREADI(num_ents);
    
    MBErrorCode result = get_entities(this_type+2, &int_buf[0], num_ents, 
                                      ns_entities, excl_entities);
    if (MB_SUCCESS != result) return result;
  }

    // and put entities into this nodeset's set
  MBErrorCode result = put_into_set(nodeseth->setHandle, ns_entities, excl_entities);
  if (MB_SUCCESS != result) return result;

  return result;
}

MBErrorCode Tqdcfr::read_sideset(const double data_version,
                                 Tqdcfr::ModelEntry *model,
                                 Tqdcfr::SidesetHeader *sideseth)  
{
  if (sideseth->memCt == 0) return MB_SUCCESS;

    // position file
  FSEEK(model->modelOffset+sideseth->memOffset);
  
    // read ids for each entity type
  int this_type, num_ents, sense_size;

  std::vector<MBEntityHandle> ss_entities, excl_entities;
  std::vector<double> ss_dfs;
  if (data_version <= 1.0) {
    for (int i = 0; i < sideseth->memTypeCt; i++) {
        // get how many and what type
      FREADI(3);
      this_type = int_buf[0];
      num_ents = int_buf[1];
      sense_size = int_buf[2];

        // now get the ids
      FREADI(num_ents);
    
      MBErrorCode result = get_entities(this_type+2, &int_buf[0], num_ents, 
                                        ss_entities, excl_entities);
      if (MB_SUCCESS != result) return result;

      if (sense_size == 1) {
          // byte-size sense flags; make sure read ends aligned...
        int read_length = (num_ents / 8) * 8;
        if (read_length < num_ents) read_length += 8;
        FREADC(read_length);
      
      }
      else if (sense_size == 2) {
          // int-size sense flags
        FREADI(num_ents);
      }

        // now do something with them...
      process_sideset_10(this_type, num_ents, sense_size, ss_entities, sideseth);
    }

  }
  else {
    for (int i = 0; i < sideseth->memTypeCt; i++) {
        // get how many and what type
      FREADI(1);
      num_ents = int_buf[0];

        // get the types, and ids
      std::vector<int> mem_types(num_ents), mem_ids(num_ents);
      FREADIA(num_ents, &mem_types[0]);
      FREADIA(num_ents, &mem_ids[0]);

        // byte-size sense flags; make sure read ends aligned...
      int read_length = (num_ents / 8) * 8;
      if (read_length < num_ents) read_length += 8;
      FREADC(read_length);

        // wrt entities
      FREADI(1);
      int num_wrts = int_buf[0];
      std::vector<int> wrt_ents(num_wrts);
      FREADIA(num_wrts, &wrt_ents[0]);
      
      std::vector<MBEntityHandle> ss_entities;
      MBErrorCode result = get_entities(&mem_types[0], &mem_ids[0], num_ents, false,
                                        ss_entities);
      if (MB_SUCCESS != result) return result;

      result = process_sideset_11(ss_entities, wrt_ents, sideseth);
      if (MB_SUCCESS != result) return result;
    }
  }

    // now set the dist factors
  if (sideseth->numDF > 0) {
      // have to read dist factors
    FREADD(sideseth->numDF);
    MBTag distFactorTag;
    double *dum_val = NULL;
    MBErrorCode result = mdbImpl->tag_create("distFactor", sizeof(double*), MB_TAG_SPARSE, distFactorTag, 
                              &dum_val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    double *dist_data = new double[ss_dfs.size()];
    memcpy(dist_data, &dbl_buf[0], sideseth->numDF*sizeof(double));
    result = mdbImpl->tag_set_data(distFactorTag, &sideseth->setHandle, 1, &dist_data);
    if (MB_SUCCESS != result) return result;
  }
  
  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::process_sideset_10(const int this_type, const int num_ents,
                                       const int sense_size,
                                       std::vector<MBEntityHandle> &ss_entities,
                                       Tqdcfr::SidesetHeader *sideseth) 
{
  std::vector<MBEntityHandle> forward, reverse;
  if (this_type == 3 // surface
      && sense_size == 1 // byte size 
      ) {
      // interpret sense flags w/o reference to anything
    for (int i = 0; i < num_ents; i++) {
      if ((int) char_buf[i] == 0) forward.push_back(ss_entities[i]);
      else if ((int) char_buf[i] == 1) reverse.push_back(ss_entities[i]);
      if ((int) char_buf[i] == -1) { // -1 means "unknown", which means both
        forward.push_back(ss_entities[i]);
        reverse.push_back(ss_entities[i]);
      }
    }
  }
  else if (this_type == 4 // curve
           && sense_size == 2 // int32 size
           ) {
    for (int i = 0; i < num_ents; i++) {
      if (int_buf[i] == 0) forward.push_back(ss_entities[i]);
      else if (int_buf[i] == 1) reverse.push_back(ss_entities[i]);
      if (int_buf[i] == -1) { // -1 means "unknown", which means both
        forward.push_back(ss_entities[i]);
        reverse.push_back(ss_entities[i]);
      }
    }
  }
  
    // now actually put them in the set
  MBErrorCode result = MB_SUCCESS;
  if (!forward.empty()) {
    MBErrorCode tmp_result = mdbImpl->add_entities(sideseth->setHandle, &forward[0], forward.size());
    if (tmp_result != MB_SUCCESS) result = tmp_result;
  }
  if (!reverse.empty()) {
      // need to make a new set, add a reverse sense tag, then add to the sideset
    MBEntityHandle reverse_set;
    MBErrorCode tmp_result = mdbImpl->create_meshset(MESHSET_SET, reverse_set);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
    tmp_result = mdbImpl->add_entities(reverse_set, &reverse[0], reverse.size());
    if (tmp_result != MB_SUCCESS) result = tmp_result;
    int def_val = 1;
    MBTag sense_tag;
    tmp_result = mdbImpl->tag_create("SENSE", sizeof(int), 
                                     MB_TAG_SPARSE, sense_tag, &def_val);
    if (tmp_result != MB_SUCCESS && tmp_result != MB_ALREADY_ALLOCATED) result = tmp_result;
    def_val = -1;
    tmp_result = mdbImpl->tag_set_data(sense_tag, &reverse_set, 1, &def_val);
    if (tmp_result != MB_SUCCESS) result = tmp_result;
    tmp_result = mdbImpl->add_entities(sideseth->setHandle, &reverse_set, 1);
    if (tmp_result != MB_SUCCESS) result = tmp_result;
  }

  return result;
}

MBErrorCode Tqdcfr::process_sideset_11(std::vector<MBEntityHandle> &ss_entities,
                                       std::vector<int> &wrt_ents,
                                       Tqdcfr::SidesetHeader *sideseth)
{
  std::vector<MBEntityHandle> forward, reverse;

  int num_ents = ss_entities.size();
  std::vector<int>::iterator wrt_it = wrt_ents.begin();
  
  for (int i = 0; i < num_ents; i++) {
    
    int num_wrt = 0;
    if (!wrt_ents.empty()) num_wrt = *wrt_it++;
    for (int j = 0; j < num_wrt; j++) wrt_it += 2;
    forward.push_back(ss_entities[i]);
      // assume here that if it's in the list twice, we get both senses
    if (num_wrt > 1) {
      forward.push_back(ss_entities[i]);
      reverse.push_back(ss_entities[i]);
    }
    else {
        // else interpret the sense flag
      if ((int) char_buf[i] == 0) forward.push_back(ss_entities[i]);
      else if ((int) char_buf[i] == 1) reverse.push_back(ss_entities[i]);
      if ((int) char_buf[i] == -1) { // -1 means "unknown", which means both
        forward.push_back(ss_entities[i]);
        reverse.push_back(ss_entities[i]);
      }
    }
  }
  
    // now actually put them in the set
  MBErrorCode result = MB_SUCCESS;
  if (!forward.empty()) {
    MBErrorCode tmp_result = mdbImpl->add_entities(sideseth->setHandle, &forward[0], forward.size());
    if (tmp_result != MB_SUCCESS) result = tmp_result;
  }
  if (!reverse.empty()) {
      // need to make a new set, add a reverse sense tag, then add to the sideset
    MBEntityHandle reverse_set;
    MBErrorCode tmp_result = mdbImpl->create_meshset(MESHSET_SET, reverse_set);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
    tmp_result = mdbImpl->add_entities(reverse_set, &reverse[0], reverse.size());
    if (tmp_result != MB_SUCCESS) result = tmp_result;
    int def_val = 1;
    MBTag sense_tag;
    tmp_result = mdbImpl->tag_create("SENSE", sizeof(int), 
                                     MB_TAG_SPARSE, sense_tag, &def_val);
    if (tmp_result != MB_SUCCESS && tmp_result != MB_ALREADY_ALLOCATED) result = tmp_result;
    def_val = -1;
    tmp_result = mdbImpl->tag_set_data(sense_tag, &reverse_set, 1, &def_val);
    if (tmp_result != MB_SUCCESS) result = tmp_result;
    tmp_result = mdbImpl->add_entities(sideseth->setHandle, &reverse_set, 1);
    if (tmp_result != MB_SUCCESS) result = tmp_result;
  }

  return result;
}

MBErrorCode Tqdcfr::read_block(const double data_version,
                               Tqdcfr::ModelEntry *model,
                               Tqdcfr::BlockHeader *blockh)  
{
  if (blockh->memCt == 0) return MB_SUCCESS;
  
    // position file
  FSEEK(model->modelOffset+blockh->memOffset);
  
    // read ids for each entity type
  int this_type, num_ents;
  std::vector<MBEntityHandle> block_entities, excl_entities;
  for (int i = 0; i < blockh->memTypeCt; i++) {
      // get how many and what type
    FREADI(2);
    this_type = int_buf[0];
    num_ents = int_buf[1];

      // now get the ids
    FREADI(num_ents);

    MBErrorCode result = get_entities(this_type+2, &int_buf[0], num_ents, 
                                      block_entities, excl_entities);
    if (MB_SUCCESS != result) return result;
  }
  
    // and put entities into this block's set
  MBErrorCode result = put_into_set(blockh->setHandle, block_entities, excl_entities);
  if (MB_SUCCESS != result) return result;
  
    // read attribs if there are any
  if (blockh->attribOrder > 0) {
    MBTag block_attribs;
    
    FREADD(blockh->attribOrder);
      // now do something with them...
    MBErrorCode result = mdbImpl->tag_create("Block_Attributes", 
                                             blockh->attribOrder*sizeof(double), MB_TAG_SPARSE, 
                                             block_attribs, NULL);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    result = mdbImpl->tag_set_data(block_attribs, &(blockh->setHandle), 1,
                                   &(dbl_buf[0]));
    if (MB_SUCCESS != result) return result;
  }

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::read_group(const int group_index,
                               Tqdcfr::ModelEntry *model,
                               Tqdcfr::GroupHeader *grouph)  
{
    // position file
  FSEEK(model->modelOffset+grouph->memOffset);
  
    // read ids for each entity type
  int this_type, num_ents;
  std::vector<MBEntityHandle> grp_entities, excl_entities;
  for (int i = 0; i < grouph->memTypeCt; i++) {
      // get how many and what type
    FREADI(2);
    this_type = int_buf[0];
    num_ents = int_buf[1];

      // now get the ids
    FREADI(num_ents);
    
      // get the entities in this group
    MBErrorCode result = get_entities(this_type, &int_buf[0], num_ents, grp_entities, excl_entities);
    if (MB_SUCCESS != result) return result;
  }

    // now add the entities
  MBErrorCode result = put_into_set(grouph->setHandle, grp_entities, excl_entities);
  if (MB_SUCCESS != result) return result;
  
    // now get group names, if any
  int md_index = model->groupMD.get_md_entry(group_index, "NAME");
  if (-1 != md_index) {
    MetaDataContainer::MetaDataEntry *md_entry = model->groupMD.metadataEntries+md_index;
    if (0 == entityNameTag) {
      result = mdbImpl->tag_get_handle("NAME", entityNameTag);
      if (MB_SUCCESS != result || 0 == entityNameTag) {
        char *dum_val = NULL;
        result = mdbImpl->tag_create("NAME", sizeof(char*), MB_TAG_SPARSE, 
                                     entityNameTag, &dum_val);
      }
    }
    if (0 == entityNameTag) return MB_FAILURE;
    char *this_char = new char[md_entry->mdStringValue.length()+1];
    strcpy(this_char, md_entry->mdStringValue.c_str());
    result = mdbImpl->tag_set_data(entityNameTag, &grouph->setHandle, 1, &this_char);
    
      // look for extra names
    md_index = model->groupMD.get_md_entry(group_index, "NumExtraNames");
    if (-1 != md_index) {
      int num_names = model->groupMD.metadataEntries[md_index].mdIntValue;
      char extra_name_label[32];
      for (int i = 0; i < num_names; i++) {
        sprintf(extra_name_label, "ExtraName%d", i);
        md_index = model->groupMD.get_md_entry(group_index, extra_name_label);
        if (-1 != md_index) {
          md_entry = model->groupMD.metadataEntries+md_index;
          this_char = new char[md_entry->mdStringValue.length()+1];
          strcpy(this_char, md_entry->mdStringValue.c_str());
          MBTag extra_name_tag;
          result = mdbImpl->tag_get_handle(extra_name_label, extra_name_tag);
          if (MB_SUCCESS != result || 0 == extra_name_tag) {
            char *dum_val = NULL;
            result = mdbImpl->tag_create(extra_name_label, sizeof(char*), MB_TAG_SPARSE, 
                                         extra_name_tag, &dum_val);
          }
          result = mdbImpl->tag_set_data(extra_name_tag, &grouph->setHandle, 1, &this_char);
        }
      }
    }
  }
  
  return result;
}

MBErrorCode Tqdcfr::put_into_set(MBEntityHandle set_handle,
                                 std::vector<MBEntityHandle> &entities,
                                 std::vector<MBEntityHandle> &excl_entities)
{
    // and put entities into this block's set
  MBErrorCode result = mdbImpl->add_entities(set_handle, &entities[0], entities.size());
  if (MB_SUCCESS != result) return result;

    // check for excluded entities, and add them to a vector hung off the block if there
  MBTag excl_tag;
  if (!excl_entities.empty()) {
    MBErrorCode result = mdbImpl->tag_create("Exclude_Entities", 
                                             sizeof(std::vector<MBEntityHandle>), MB_TAG_SPARSE, 
                                             excl_tag, NULL);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    std::vector<MBEntityHandle> *new_vector = new std::vector<MBEntityHandle>;
    new_vector->swap(excl_entities);
    result = mdbImpl->tag_set_data(excl_tag, &set_handle, 1, &new_vector);
    if (MB_SUCCESS != result) return MB_FAILURE;
  }
  
  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::get_entities(const int *mem_types, 
                                 int *id_buf, const int id_buf_size,
                                 const bool is_group,
                                 std::vector<MBEntityHandle> &entities) 
{
  MBErrorCode tmp_result, result = MB_SUCCESS;
  
  for (int i = 0; i < id_buf_size; i++) {
    if (is_group)
      tmp_result = get_entities(mem_types[i], id_buf+i, 1, entities, entities);
    else
        // for blocks/nodesets/sidesets, use CSOEntityType, which is 2 greater than
        // group entity types
      tmp_result = get_entities(mem_types[i]+2, id_buf+i, 1, entities, entities);
    if (MB_SUCCESS != tmp_result) result = tmp_result;
  }
  return result;
}
  
MBErrorCode Tqdcfr::get_entities(const int this_type, 
                                 int *id_buf, const int id_buf_size,
                                 std::vector<MBEntityHandle> &entities,
                                 std::vector<MBEntityHandle> &excl_entities) 
{
  MBErrorCode result = MB_FAILURE;
  
  if (this_type >= GROUP && this_type <= VERTEX)
    result = get_ref_entities(this_type, id_buf, id_buf_size, entities);
  else if (this_type >= HEX && this_type <= NODE)
    result = get_mesh_entities(this_type, id_buf, id_buf_size, entities, excl_entities);

  return result;
}

MBErrorCode Tqdcfr::get_ref_entities(const int this_type, 
                                     int *id_buf, const int id_buf_size,
                                     std::vector<MBEntityHandle> &entities) 
{
  MBErrorCode result = MB_SUCCESS;
  MBTag tags[2] = {geomTag, globalIdTag};
  int tag_vals[2];
  const void *tag_vals_ptr[2] = {tag_vals, tag_vals+1};
  if (this_type == GROUP || this_type == BODY) tag_vals[0] = 4;
  else tag_vals[0] = 5 - this_type;
  
  MBRange tmp_ents;
  
  for (int i = 0; i < id_buf_size; i++) {
    tmp_ents.clear();
      // set id tag value
    tag_vals[1] = id_buf[i];
    
    MBErrorCode tmp_result = mdbImpl->get_entities_by_type_and_tag(0, MBENTITYSET, tags, 
                                                                   tag_vals_ptr, 2, tmp_ents);
    if (MB_SUCCESS != tmp_result && MB_TAG_NOT_FOUND != tmp_result) result = tmp_result;
    
    if (MB_SUCCESS == tmp_result && tmp_ents.size() == 1)
      entities.push_back(*tmp_ents.begin());
  }

  return result;
}

MBErrorCode Tqdcfr::get_mesh_entities(const int this_type, 
                                      int *id_buf, const int id_buf_size,
                                      std::vector<MBEntityHandle> &entities,
                                      std::vector<MBEntityHandle> &excl_entities) 
{
  MBErrorCode result = MB_SUCCESS;
  std::vector<MBEntityHandle> *ent_list = NULL;
  MBEntityType this_ent_type;
  if (this_type > 1000) {
    this_ent_type = group_type_to_mb_type[this_type-1000];
    ent_list = &excl_entities;
  }
  else {
    this_ent_type = group_type_to_mb_type[this_type];
    ent_list = &entities;
  }
  
    // get entities with this type, and get their cub id tags
  if (MBVERTEX == this_ent_type) {
      // use either vertex offset or cubMOABVertexMap
    if (NULL == cubMOABVertexMap) {
      for (int i = 0; i < id_buf_size; i++)
        ent_list->push_back(int_buf[i]+currNodeIdOffset);
    }
    else {
      for (int i = 0; i < id_buf_size; i++)
        ent_list->push_back((*cubMOABVertexMap)[int_buf[i]]);
    }
  }
  else {
    MBRange tmp_ents;
    result = mdbImpl->get_entities_by_type(0, this_ent_type, tmp_ents);
    if (MB_SUCCESS != result) return result;
    if (tmp_ents.empty() && 0 != id_buf_size) return MB_FAILURE;
  
    std::vector<int> cub_ids(id_buf_size);
    result = mdbImpl->tag_get_data(cubIdTag, tmp_ents, &cub_ids[0]);
    if (MB_SUCCESS != result && MB_TAG_NOT_FOUND != result) return result;
  
      // now go through id list, finding each entity by id
    for (int i = 0; i < id_buf_size; i++) {
      std::vector<int>::iterator vit = std::find(cub_ids.begin(), cub_ids.end(), id_buf[i]);
      if (vit != cub_ids.end()) {
        MBEntityHandle this_ent = tmp_ents[vit-cub_ids.begin()];
        if (mdbImpl->type_from_handle(this_ent) != MBMAXTYPE) ent_list->push_back(this_ent);
      }
      else {
        std::cout << "Warning: didn't find " << MBCN::EntityTypeName(this_ent_type) 
                  << " " << *vit << std::endl;
      }
    }
  }

  return result;
}

MBErrorCode Tqdcfr::read_nodes(const int gindex,
                               Tqdcfr::ModelEntry *model,
                               Tqdcfr::GeomHeader *entity) 
{
  if (entity->nodeCt == 0) return MB_SUCCESS;
  
    // get the ids & coords in separate calls to minimize memory usage
    // position the file
  FSEEK(model->modelOffset+entity->nodeOffset);
    // get node ids in int_buf
  FREADI(entity->nodeCt);

    // get a space for reading nodal data directly into MB, and read that data
  MBEntityHandle node_handle = 0;
  std::vector<double*> arrays;
  readUtilIface->get_node_arrays(3, entity->nodeCt,
                                 int_buf[0], MB_PROC_RANK, node_handle, arrays);
    // get node x's in arrays[0]
  FREADDA(entity->nodeCt, arrays[0]);
    // get node y's in arrays[1]
  FREADDA(entity->nodeCt, arrays[1]);
    // get node z's in arrays[2]
  FREADDA(entity->nodeCt, arrays[2]);

    // add these nodes into the entity's set
  MBRange dum_range(node_handle, 
                    node_handle+entity->nodeCt-1);
  MBErrorCode result = mdbImpl->add_entities(entity->setHandle, dum_range);
  if (MB_SUCCESS != result) return result;

    // check for id contiguity
  long unsigned int node_offset = mdbImpl->id_from_handle( node_handle);

  int max_id = -1;
  int contig;
  check_contiguous(entity->nodeCt, contig, max_id);

  if (NULL == cubMOABVertexMap) {
      // haven't needed one yet, see if we need to keep a map to orig cub ids

    if (contig == 1) node_offset -= int_buf[0];
    else if (contig == -1) node_offset -= int_buf[entity->nodeCt-1];
  
    if (contig && -1 == currNodeIdOffset)
      currNodeIdOffset = node_offset;
    else if ((contig && (long unsigned int) currNodeIdOffset != node_offset) ||
             !contig) {
      // node offsets no longer valid - need to build cub id - vertex handle map
      cubMOABVertexMap = new std::vector<MBEntityHandle>(node_offset+entity->nodeCt);

      if (-1 != currNodeIdOffset && currNodeIdOffset != (int) node_offset) {
          // now fill the missing values for vertices which already exist
        MBRange vrange;
        result = mdbImpl->get_entities_by_type(0, MBVERTEX, vrange); RR;
        MBRange::const_iterator rit = vrange.lower_bound(vrange.begin(), vrange.end(),
                                                         currNodeIdOffset);
        for (; *rit != node_offset; rit++)
          (*cubMOABVertexMap)[*rit] = *rit;
      }
    }
  }
  
  if (NULL != cubMOABVertexMap) {
      // expand the size if necessary
    if (max_id > (int) cubMOABVertexMap->size()-1) cubMOABVertexMap->resize(max_id+1);
    
      // now set the new values
    std::vector<int>::iterator vit;
    MBRange::iterator rit;
    for (vit = int_buf.begin(), rit = dum_range.begin(); rit != dum_range.end(); vit++, rit++) {
      assert(0 < *vit && *vit < (int)cubMOABVertexMap->size());
      (*cubMOABVertexMap)[*vit] = *rit;
    }
  }

    // set the dimension to at least zero (entity has at least nodes) on the geom tag
  int max_dim = 0;
  result = mdbImpl->tag_set_data(geomTag, &(entity->setHandle), 1, &max_dim);
  if (MB_SUCCESS != result) return result;
    // set the category tag just in case there're only vertices in this set
  result = mdbImpl->tag_set_data(categoryTag, &entity->setHandle, 1, 
                                 &geom_categories[0]);
  if (MB_SUCCESS != result) return result;

    // don't need cub ids for vertices because of cubMOABVertexMap

    // get fixed node data and assign
  int md_index = model->nodeMD.get_md_entry(gindex, "FixedNodes");
  if (-1 == md_index) return MB_SUCCESS;
  MetaDataContainer::MetaDataEntry *md_entry = model->nodeMD.metadataEntries+md_index;
  
  std::vector<int> fixed_flags(entity->nodeCt);
  std::fill(fixed_flags.begin(), fixed_flags.end(), 0);
  if (md_entry->mdDataType != 3) return MB_FAILURE;
    // if contiguous, we can use the node id as an offset
  if (1 == contig) {
    for (std::vector<int>::iterator vit = md_entry->mdIntArrayValue.begin();
         vit != md_entry->mdIntArrayValue.end(); vit++) {
      if (*vit - int_buf[0] < entity->nodeCt) return MB_FAILURE;
      fixed_flags[*vit - int_buf[0]] = 1;
    }
  }
    // else we have to find the position of each node in the node ids, then set the
    // equivalent position in the fixed flags
  else {
    int *buf_end = &int_buf[0]+entity->nodeCt;
    for (std::vector<int>::iterator vit = md_entry->mdIntArrayValue.begin();
         vit != md_entry->mdIntArrayValue.end(); vit++) {
      int *vit2 = std::find(&int_buf[0], buf_end, *vit);
      if (vit2 != buf_end) fixed_flags[vit2 - &int_buf[0]] = 1;
    }
  }

  static MBTag fixedFlagTag = 0;
  if (0 == fixedFlagTag) {
    result = mdbImpl->tag_get_handle("NodeFixed", fixedFlagTag);
    if (MB_SUCCESS != result || 0 == fixedFlagTag) {
      int dum_val = 0;
      result = mdbImpl->tag_create("NodeFixed", sizeof(int), MB_TAG_SPARSE, 
                                   fixedFlagTag, &dum_val);
      if (MB_SUCCESS != result) return result;
    }
  }
  result = mdbImpl->tag_set_data(fixedFlagTag, dum_range, &fixed_flags[0]);

  return result;
}

MBErrorCode Tqdcfr::read_elements(Tqdcfr::ModelEntry *model,
                                  Tqdcfr::GeomHeader *entity) 
{
  if (entity->elemTypeCt == 0) return MB_SUCCESS;
  
    // get data in separate calls to minimize memory usage
    // position the file
  FSEEK(model->modelOffset+entity->elemOffset);

  int int_type, nodes_per_elem, num_elem;
  int max_dim = -1;
  MBErrorCode result;
  for (int i = 0; i < entity->elemTypeCt; i++) {
      // for this elem type, get the type, nodes per elem, num elems
    FREADI(3);
    int_type = int_buf[0];
    nodes_per_elem = int_buf[1];
    num_elem = int_buf[2];

      // get MB element type from cub file's 
    MBEntityType elem_type = mp_type_to_mb_type[int_type];
    max_dim = (max_dim < MBCN::Dimension(elem_type) ? MBCN::Dimension(elem_type) : max_dim);
    
      // get element ids
    FREADI(num_elem);
    
      // check to see if ids are contiguous...
    int contig, max_id;
    check_contiguous(num_elem, contig, max_id);
    if (0 == contig)
      std::cout << "Element ids are not contiguous!" << std::endl;
    
      // get a space for reading connectivity data directly into MB
    MBEntityHandle *conn, start_handle;
    
    readUtilIface->get_element_array(num_elem, nodes_per_elem,
                                     elem_type, int_buf[0], MB_PROC_RANK, start_handle, conn);
        
    long unsigned int elem_offset;
    elem_offset = mdbImpl->id_from_handle( start_handle) - int_buf[0];
    if (-1 == currElementIdOffset[elem_type])
      currElementIdOffset[elem_type] = elem_offset;
    
      // now do something with them...

      // get the connectivity array
    int total_conn = num_elem * nodes_per_elem;
    FREADIA(total_conn, conn);

      // post-process connectivity into handles
    MBEntityHandle new_handle, dum_handle;
    int dum_err;
    for (i = 0; i < total_conn; i++) {
      if (NULL == cubMOABVertexMap)
        new_handle = CREATE_HANDLE(MBVERTEX, currNodeIdOffset+conn[i], dum_err);
      else new_handle = (*cubMOABVertexMap)[conn[i]];
      assert(MB_SUCCESS == 
             mdbImpl->handle_from_id(MBVERTEX, mdbImpl->id_from_handle(new_handle), 
                                     dum_handle));
      conn[i] = new_handle;
    }

      // add these elements into the entity's set
    MBRange dum_range(start_handle, start_handle+num_elem-1);
    result = mdbImpl->add_entities(entity->setHandle, dum_range);
    if (MB_SUCCESS != result) return result;

      // set cub ids
    result = mdbImpl->tag_set_data(cubIdTag, dum_range, &int_buf[0]);
    if (MB_SUCCESS != result) return result;
  }

    // set the dimension on the geom tag
  result = mdbImpl->tag_set_data(geomTag, &entity->setHandle, 1, &max_dim);
  if (MB_SUCCESS != result) return result;
  if (max_dim != -1) {
    result = mdbImpl->tag_set_data(categoryTag, &entity->setHandle, 1, 
                                   &geom_categories[max_dim]);
    if (MB_SUCCESS != result) return result;
  }

  return MB_SUCCESS;
}

void Tqdcfr::check_contiguous(const int num_ents, int &contig, int &max_id) 
{
  std::vector<int>::iterator id_it;
  int curr_id, i;
  max_id = -1;

    // check in forward-contiguous direction
  id_it = int_buf.begin();
  curr_id = *id_it++ + 1;
  for (i = 1; id_it != int_buf.end() && i < num_ents; id_it++, i++) {
    if (*id_it != curr_id) {
      i = 0;
      break;
    }
    curr_id++;
  }

    // if we got here and we're at the end of the loop, it's forward-contiguous
  if (i == num_ents) {
    max_id = int_buf[i-1];
    contig = 1;
    return;
  }

// check in reverse-contiguous direction
  id_it = int_buf.begin();
  curr_id = *id_it++ - 1;
  for (i = 1; id_it != int_buf.end() && i < num_ents; id_it++, i++) {
    if (*id_it != curr_id) {
      i = 0;
      break;
    }
    curr_id--;
  }


    // if we got here and we're at the end of the loop, it's reverse-contiguous
  if (i == num_ents) {
    max_id = int_buf[0];
    contig = -1;
    return;
  }

    // one final check, for contiguous but out of order
  int min_id = -1;
  max_id = -1;
  
    // need to loop over i, b/c int_buf is bigger than num_ents
  for (id_it = int_buf.begin(), i = 0; i < num_ents; id_it++, i++) {
    if (*id_it < min_id || -1 == min_id) min_id = *id_it;
    if (*id_it > max_id || -1 == max_id) max_id = *id_it;
  }
  if (max_id - min_id + 1 == num_ents) contig = min_id;

    // else it's not contiguous at all
  contig = 0;
}
  
void Tqdcfr::FEModelHeader::init(const int offset, Tqdcfr* instance ) 
{
  FSEEK(offset);
  FREADI(4);
  feEndian = instance->int_buf[0];
  feSchema = instance->int_buf[1];
  feCompressFlag = instance->int_buf[2];
  feLength = instance->int_buf[3];
  FREADI(3); geomArray.init(instance->int_buf);
  FREADI(2);
  nodeArray.metaDataOffset = instance->int_buf[0];
  elementArray.metaDataOffset = instance->int_buf[1];
  FREADI(3); groupArray.init(instance->int_buf);
  FREADI(3); blockArray.init(instance->int_buf);
  FREADI(3); nodesetArray.init(instance->int_buf);
  FREADI(3); sidesetArray.init(instance->int_buf);
  FREADI(1);
}

MBErrorCode Tqdcfr::read_file_header() 
{
    // read file header
  FSEEK(4);
  FREADI(6);
  fileTOC.fileEndian = int_buf[0];
  fileTOC.fileSchema = int_buf[1];
  fileTOC.numModels = int_buf[2];
  fileTOC.modelTableOffset = int_buf[3];
  fileTOC.modelMetaDataOffset = int_buf[4];
  fileTOC.activeFEModel = int_buf[5];
  if (debug) fileTOC.print();

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::read_model_entries() 
{
  
    // read model entries
  FSEEK(fileTOC.modelTableOffset);
  FREADI(fileTOC.numModels*6);
  modelEntries = new ModelEntry[fileTOC.numModels];
  if (NULL == modelEntries) return MB_FAILURE;
  std::vector<int>::iterator int_it = int_buf.begin();
  for (int i = 0; i < fileTOC.numModels; i++) {
    modelEntries[i].modelHandle = *int_it++;
    modelEntries[i].modelOffset = *int_it++;
    modelEntries[i].modelLength = *int_it++;
    modelEntries[i].modelType = *int_it++;
    modelEntries[i].modelOwner = *int_it++;
    modelEntries[i].modelPad = *int_it++;
    if (int_it == int_buf.end() && i != fileTOC.numModels-1) return MB_FAILURE;
    if (debug) modelEntries[i].print();
  }

  return MB_SUCCESS;
}

int Tqdcfr::find_model(const int model_type) 
{
  for (int i = 0; i < fileTOC.numModels; i++) 
    if (modelEntries[i].modelType == model_type) return i;
  
  return -1;
}

MBErrorCode Tqdcfr::read_meta_data(const int metadata_offset, 
                                   Tqdcfr::MetaDataContainer &mc) 
{
    // read the metadata header
  FSEEK(metadata_offset);
  FREADI(3);
  mc.mdSchema = int_buf[0];
  mc.compressFlag = int_buf[1];
  mc.numDatums = int_buf[2];

    // allocate space for the entries
  mc.metadataEntries = 
    new Tqdcfr::MetaDataContainer::MetaDataEntry[mc.numDatums];
  
    // now read the metadata values
  for (int i = 0; i < mc.numDatums; i++) {
    FREADI(2);
    mc.metadataEntries[i].mdOwner = int_buf[0];
    mc.metadataEntries[i].mdDataType = int_buf[1];
    
      // read the name string
    read_md_string(mc.metadataEntries[i].mdName);

    if (mc.metadataEntries[i].mdDataType == 0) {
        // integer
      FREADI(1);
      mc.metadataEntries[i].mdIntValue = int_buf[0];
    }
    else if (mc.metadataEntries[i].mdDataType == 1) {
        // string
      read_md_string(mc.metadataEntries[i].mdStringValue);
    }
    else if (mc.metadataEntries[i].mdDataType == 2) {
        // double
      FREADD(1);
      mc.metadataEntries[i].mdDblValue = dbl_buf[0];
    }
    else if (mc.metadataEntries[i].mdDataType == 3) {
        // int array
      FREADI(1);
      mc.metadataEntries[i].mdIntArrayValue.resize(int_buf[0]);
      FREADI(mc.metadataEntries[i].mdIntArrayValue.size());
      std::copy(int_buf.begin(), 
                int_buf.begin() + mc.metadataEntries[i].mdIntArrayValue.size(),
                mc.metadataEntries[i].mdIntArrayValue.begin());
    }
    else if (mc.metadataEntries[i].mdDataType == 4) {
        // double array
      FREADI(1);
      mc.metadataEntries[i].mdDblArrayValue.resize(int_buf[0]);
      FREADD(mc.metadataEntries[i].mdDblArrayValue.size());
      std::copy(dbl_buf.begin(), 
                dbl_buf.begin() + mc.metadataEntries[i].mdDblArrayValue.size(),
                mc.metadataEntries[i].mdDblArrayValue.begin());
    }
    else
      return MB_FAILURE;
  }
  if (debug) mc.print();

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::read_md_string(std::string &name) 
{
  FREADI(1);
  int str_size = int_buf[0];
  if (str_size > 0) {
    FREADC(str_size);
    if (char_buf.size() <= (unsigned int) str_size)
      char_buf.resize(str_size+1);
    char_buf[str_size] = '\0';
    name = (char *) &char_buf[0];
      // read pad if any
    int extra = str_size % sizeof(int);
    if (extra) {
        // read extra chars to end of pad
      str_size = sizeof(int) - extra;
      FREADC(str_size);
    }
  }

  return MB_SUCCESS;
}
  
MBErrorCode Tqdcfr::GeomHeader::read_info_header(const int model_offset, 
                                                 const Tqdcfr::FEModelHeader::ArrayInfo &info,
                                                 Tqdcfr* instance,
                                                 Tqdcfr::GeomHeader *&geom_headers) 
{
  geom_headers = new GeomHeader[info.numEntities];
  FSEEK(model_offset+info.tableOffset);
  int dum_int;
  MBErrorCode result;

  if (0 == instance->categoryTag) {
    static const char val[CATEGORY_TAG_NAME_LENGTH] = "\0";
    result = instance->mdbImpl->tag_create(CATEGORY_TAG_NAME, CATEGORY_TAG_NAME_LENGTH,
                                           MB_TAG_SPARSE, instance->categoryTag, val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
  }

  for (int i = 0; i < info.numEntities; i++) {

      // create an entity set for this entity
    result = instance->mdbImpl->create_meshset(MESHSET_SET, geom_headers[i].setHandle);
    if (MB_SUCCESS != result) return result;
    
    FREADI(8);
    geom_headers[i].nodeCt = instance->int_buf[0];
    geom_headers[i].nodeOffset = instance->int_buf[1];
    geom_headers[i].elemCt = instance->int_buf[2];
    geom_headers[i].elemOffset = instance->int_buf[3];
    geom_headers[i].elemTypeCt = instance->int_buf[4];
    geom_headers[i].elemLength = instance->int_buf[5];
    geom_headers[i].geomID = instance->int_buf[6];

      // set the dimension to -1; will have to reset later, after elements are read
    dum_int = -1;
    result = instance->mdbImpl->tag_set_data(instance->geomTag, 
                                             &(geom_headers[i].setHandle), 1, &dum_int);
    if (MB_SUCCESS != result) return result;

      // set a unique id tag
    result = instance->mdbImpl->tag_set_data(instance->uniqueIdTag, 
                                             &(geom_headers[i].setHandle), 1, 
                                             &(geom_headers[i].geomID));
    if (MB_SUCCESS != result) return result;
  }

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::GroupHeader::read_info_header(const int model_offset, 
                                           const Tqdcfr::FEModelHeader::ArrayInfo &info,
                                           Tqdcfr* instance,
                                           Tqdcfr::GroupHeader *&group_headers) 
{
  group_headers = new GroupHeader[info.numEntities];
  FSEEK(model_offset+info.tableOffset);
  int dum_int;
  MBErrorCode result;

  if (0 == instance->categoryTag) {
    static const char val[CATEGORY_TAG_NAME_LENGTH] = "\0";
    result = instance->mdbImpl->tag_create(CATEGORY_TAG_NAME, CATEGORY_TAG_NAME_LENGTH,
                                           MB_TAG_SPARSE, instance->categoryTag, val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
  }

  for (int i = 0; i < info.numEntities; i++) {

      // create an entity set for this entity
    result = instance->mdbImpl->create_meshset(MESHSET_SET, group_headers[i].setHandle);
    if (MB_SUCCESS != result) return result;
    static const char group_category[CATEGORY_TAG_NAME_LENGTH] = "Group\0";
    
    FREADI(6);
    group_headers[i].grpID = instance->int_buf[0];
    group_headers[i].grpType = instance->int_buf[1];
    group_headers[i].memCt = instance->int_buf[2];
    group_headers[i].memOffset = instance->int_buf[3];
    group_headers[i].memTypeCt = instance->int_buf[4];
    group_headers[i].grpLength = instance->int_buf[5];

      // set the group tag to 1 to signify this is a group
    dum_int = 1;
    result = instance->mdbImpl->tag_set_data(instance->groupTag, 
                                             &(group_headers[i].setHandle), 1, &dum_int);
    if (MB_SUCCESS != result) return result;
    result = instance->mdbImpl->tag_set_data(instance->categoryTag, 
                                             &(group_headers[i].setHandle), 1, 
                                             group_category);
    if (MB_SUCCESS != result) return result;

      // set a global id tag
    result = instance->mdbImpl->tag_set_data(instance->globalIdTag, 
                                             &(group_headers[i].setHandle), 1, 
                                             &(group_headers[i].grpID));
    if (MB_SUCCESS != result) return result;
        
    break;
  }

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::BlockHeader::read_info_header(const double data_version,
                                                  const int model_offset, 
                                                  const Tqdcfr::FEModelHeader::ArrayInfo &info,
                                                  Tqdcfr* instance,
                                                  Tqdcfr::BlockHeader *&block_headers) 
{
  block_headers = new BlockHeader[info.numEntities];
  FSEEK(model_offset+info.tableOffset);
  MBErrorCode result;

  if (0 == instance->categoryTag) {
    static const char val[CATEGORY_TAG_NAME_LENGTH] = "\0";
    result = instance->mdbImpl->tag_create(CATEGORY_TAG_NAME, CATEGORY_TAG_NAME_LENGTH,
                                           MB_TAG_SPARSE, instance->categoryTag, val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
  }

  for (int i = 0; i < info.numEntities; i++) {

      // create an entity set for this entity
    result = instance->mdbImpl->create_meshset(MESHSET_SET, block_headers[i].setHandle);
    if (MB_SUCCESS != result) return result;
    static const char material_category[CATEGORY_TAG_NAME_LENGTH] = "Material Set\0";
    
    FREADI(12);
    block_headers[i].blockID = instance->int_buf[0];
    block_headers[i].blockElemType = instance->int_buf[1];
    block_headers[i].memCt = instance->int_buf[2];
    block_headers[i].memOffset = instance->int_buf[3];
    block_headers[i].memTypeCt = instance->int_buf[4];
    block_headers[i].attribOrder = instance->int_buf[5]; // attrib order
    block_headers[i].blockCol = instance->int_buf[6];
    block_headers[i].blockMixElemType = instance->int_buf[7]; // mixed elem type
    block_headers[i].blockPyrType = instance->int_buf[8];
    block_headers[i].blockMat = instance->int_buf[9];
    block_headers[i].blockLength = instance->int_buf[10];
    block_headers[i].blockDim = instance->int_buf[11];

      // adjust element type for data version; older element types didn't include
      // 4 new trishell element types
    if (data_version <= 1.0 && block_headers[i].blockElemType >= 15)
      block_headers[i].blockElemType += 4;

      // set the material set tag and id tag both to id
    result = instance->mdbImpl->tag_set_data(instance->blockTag, &(block_headers[i].setHandle), 1, 
                                             &(block_headers[i].blockID));
    if (MB_SUCCESS != result) return result;
    result = instance->mdbImpl->tag_set_data(instance->globalIdTag, &(block_headers[i].setHandle), 1, 
                                             &(block_headers[i].blockID));
    if (MB_SUCCESS != result) return result;
    result = instance->mdbImpl->tag_set_data(instance->categoryTag, 
                                             &(block_headers[i].setHandle), 1, 
                                             material_category);
    if (MB_SUCCESS != result) return result;

      // check the number of vertices in the element type, and set the has mid nodes tag
      // accordingly
    int num_verts = cub_elem_num_verts[block_headers[i].blockElemType];
    block_headers[i].blockEntityType = block_type_to_mb_type[block_headers[i].blockElemType];
    if (num_verts != MBCN::VerticesPerEntity(block_headers[i].blockEntityType)) {
        // not a linear element; try to find hasMidNodes values
      int has_mid_nodes[] = {0, 0, 0, 0};
      static MBTag hasMidNodesTag = 0;
      if (0 == hasMidNodesTag) {
        result = instance->mdbImpl->tag_create(HAS_MID_NODES_TAG_NAME, 4*sizeof(int), MB_TAG_SPARSE, hasMidNodesTag,
                                               has_mid_nodes);
        if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
      }
      
      MBCN::HasMidNodes(block_headers[i].blockEntityType, num_verts, has_mid_nodes);

        // now set the tag on this set
      result = instance->mdbImpl->tag_set_data(hasMidNodesTag, &block_headers[i].setHandle, 1,
                                               has_mid_nodes);
      if (MB_SUCCESS != result) return result;
    }
  }

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::NodesetHeader::read_info_header(const int model_offset, 
                                             const Tqdcfr::FEModelHeader::ArrayInfo &info,
                                             Tqdcfr* instance,
                                             Tqdcfr::NodesetHeader *&nodeset_headers) 
{
  nodeset_headers = new NodesetHeader[info.numEntities];
  FSEEK(model_offset+info.tableOffset);
  MBErrorCode result;

  if (0 == instance->categoryTag) {
    static const char val[CATEGORY_TAG_NAME_LENGTH] = "\0";
    result = instance->mdbImpl->tag_create(CATEGORY_TAG_NAME, CATEGORY_TAG_NAME_LENGTH,
                                           MB_TAG_SPARSE, instance->categoryTag, val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
  }

  for (int i = 0; i < info.numEntities; i++) {

      // create an entity set for this entity
    result = instance->mdbImpl->create_meshset(MESHSET_SET, nodeset_headers[i].setHandle);
    if (MB_SUCCESS != result) return result;
    static const char dirichlet_category[CATEGORY_TAG_NAME_LENGTH] = "Dirichlet Set\0";
    
    FREADI(8);
    nodeset_headers[i].nsID = instance->int_buf[0];
    nodeset_headers[i].memCt = instance->int_buf[1];
    nodeset_headers[i].memOffset = instance->int_buf[2];
    nodeset_headers[i].memTypeCt = instance->int_buf[3];
    nodeset_headers[i].pointSym = instance->int_buf[4];  // point sym
    nodeset_headers[i].nsCol = instance->int_buf[5];
    nodeset_headers[i].nsLength = instance->int_buf[6];
      // pad

      // set the dirichlet set tag and id tag both to id
    result = instance->mdbImpl->tag_set_data(instance->nsTag, &(nodeset_headers[i].setHandle), 1, 
                                             &(nodeset_headers[i].nsID));
    if (MB_SUCCESS != result) return result;
    result = instance->mdbImpl->tag_set_data(instance->globalIdTag, &(nodeset_headers[i].setHandle), 1, 
                                             &(nodeset_headers[i].nsID));
    if (MB_SUCCESS != result) return result;
    result = instance->mdbImpl->tag_set_data(instance->categoryTag, 
                                             &(nodeset_headers[i].setHandle), 1, 
                                             dirichlet_category);
    if (MB_SUCCESS != result) return result;
        
  }

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::SidesetHeader::read_info_header(const int model_offset, 
                                             const Tqdcfr::FEModelHeader::ArrayInfo &info,
                                             Tqdcfr* instance,
                                             Tqdcfr::SidesetHeader *&sideset_headers) 
{
  sideset_headers = new SidesetHeader[info.numEntities];
  FSEEK(model_offset+info.tableOffset);
  MBErrorCode result;

  if (0 == instance->categoryTag) {
    static const char val[CATEGORY_TAG_NAME_LENGTH] = "\0";
    result = instance->mdbImpl->tag_create(CATEGORY_TAG_NAME, CATEGORY_TAG_NAME_LENGTH,
                                           MB_TAG_SPARSE, instance->categoryTag, val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
  }

  for (int i = 0; i < info.numEntities; i++) {

      // create an entity set for this entity
    result = instance->mdbImpl->create_meshset(MESHSET_SET, sideset_headers[i].setHandle);
    if (MB_SUCCESS != result) return result;
    static const char neumann_category[CATEGORY_TAG_NAME_LENGTH] = "Neumann Set\0";
    
    FREADI(8);
    sideset_headers[i].ssID = instance->int_buf[0];
    sideset_headers[i].memCt = instance->int_buf[1];
    sideset_headers[i].memOffset = instance->int_buf[2];
    sideset_headers[i].memTypeCt = instance->int_buf[3];
    sideset_headers[i].numDF = instance->int_buf[4]; // num dist factors
    sideset_headers[i].ssCol = instance->int_buf[5];
    sideset_headers[i].useShell = instance->int_buf[6];
    sideset_headers[i].ssLength = instance->int_buf[7];

      // set the neumann set tag and id tag both to id
    result = instance->mdbImpl->tag_set_data(instance->ssTag, &(sideset_headers[i].setHandle), 1, 
                                             &(sideset_headers[i].ssID));
    if (MB_SUCCESS != result) return result;
    result = instance->mdbImpl->tag_set_data(instance->globalIdTag, &(sideset_headers[i].setHandle), 1, 
                                             &(sideset_headers[i].ssID));
    if (MB_SUCCESS != result) return result;
    result = instance->mdbImpl->tag_set_data(instance->categoryTag, 
                                             &(sideset_headers[i].setHandle), 1, 
                                             neumann_category);
    if (MB_SUCCESS != result) return result;
        
  }

  return MB_SUCCESS;
}

void Tqdcfr::ModelEntry::print_geom_headers(const char *prefix,
                                           GeomHeader *header,
                                            const int num_headers)
{
  if (!debug) return;
  std::cout << prefix << std::endl;
  if (NULL != header)
    for (int i = 0; i < num_headers; i++) header[i].print();
}

void Tqdcfr::ModelEntry::print_group_headers(const char *prefix,
                                            GroupHeader *header,
                                            const int num_headers)
{
  if (!debug) return;
  std::cout << prefix << std::endl;
  if (NULL != header)
    for (int i = 0; i < num_headers; i++) header[i].print();
}

void Tqdcfr::ModelEntry::print_block_headers(const char *prefix,
                                            BlockHeader *header,
                                            const int num_headers)
{
  if (!debug) return;
  std::cout << prefix << std::endl;
  if (NULL != header)
    for (int i = 0; i < num_headers; i++) header[i].print();
}

void Tqdcfr::ModelEntry::print_nodeset_headers(const char *prefix,
                                              NodesetHeader *header,
                                            const int num_headers)
{
  if (!debug) return;
  std::cout << prefix << std::endl;
  if (NULL != header)
    for (int i = 0; i < num_headers; i++) header[i].print();
}

void Tqdcfr::ModelEntry::print_sideset_headers(const char *prefix,
                                              SidesetHeader *header,
                                            const int num_headers)
{
  if (!debug) return;
  std::cout << prefix << std::endl;
  if (NULL != header)
    for (int i = 0; i < num_headers; i++) header[i].print();
}
    
MBErrorCode Tqdcfr::ModelEntry::read_header_info( Tqdcfr* instance, const double data_version)
{
  feModelHeader.init(modelOffset, instance);
  int default_val = -1;
  MBErrorCode result;

  result = instance->mdbImpl->tag_create(GLOBAL_ID_TAG_NAME, 4, MB_TAG_DENSE, 
                                         instance->globalIdTag, &default_val);
  if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;

  if (feModelHeader.geomArray.numEntities > 0) {
    result = instance->mdbImpl->tag_create(GEOM_DIMENSION_TAG_NAME, 4, MB_TAG_SPARSE, 
                                           instance->geomTag, &default_val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    
    result = instance->mdbImpl->tag_create("UNIQUE_ID", 4, MB_TAG_SPARSE, 
                                           instance->uniqueIdTag, &default_val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    
    result = Tqdcfr::GeomHeader::read_info_header(modelOffset, 
                                                  feModelHeader.geomArray, 
                                                  instance,
                                                  feGeomH);
    print_geom_headers("Geom headers:", feGeomH, feModelHeader.geomArray.numEntities);
    if (MB_SUCCESS != result) return result;
  }
  
  if (feModelHeader.groupArray.numEntities > 0) {
    result = instance->mdbImpl->tag_create("GROUP_SET", 4, MB_TAG_SPARSE, 
                                           instance->groupTag, &default_val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    
    result = Tqdcfr::GroupHeader::read_info_header(modelOffset, 
                                                   feModelHeader.groupArray, 
                                                   instance,
                                                   feGroupH);
    print_group_headers("Group headers:", feGroupH, feModelHeader.groupArray.numEntities);
    if (MB_SUCCESS != result) return result;
  }

  if (feModelHeader.blockArray.numEntities > 0) {
    result = instance->mdbImpl->tag_create(MATERIAL_SET_TAG_NAME, 4, MB_TAG_SPARSE, 
                                           instance->blockTag, &default_val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    
    result = Tqdcfr::BlockHeader::read_info_header(data_version, modelOffset, 
                                                   feModelHeader.blockArray, 
                                                   instance,
                                                   feBlockH);
    if (MB_SUCCESS != result) return result;
    print_block_headers("Block headers:", feBlockH, feModelHeader.blockArray.numEntities);
  }
  if (feModelHeader.nodesetArray.numEntities > 0) {
    result = instance->mdbImpl->tag_create(DIRICHLET_SET_TAG_NAME, 4, MB_TAG_SPARSE, 
                                           instance->nsTag, &default_val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    
    result = Tqdcfr::NodesetHeader::read_info_header(modelOffset, 
                                                     feModelHeader.nodesetArray, 
                                                     instance,
                                                     feNodeSetH);
    if (MB_SUCCESS != result) return result;
    print_nodeset_headers("Nodeset headers:", feNodeSetH, feModelHeader.nodesetArray.numEntities);
  }
  if (feModelHeader.sidesetArray.numEntities > 0) {
    result = instance->mdbImpl->tag_create(NEUMANN_SET_TAG_NAME, 4, MB_TAG_SPARSE, 
                                           instance->ssTag, &default_val);
    if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;
    
    result = Tqdcfr::SidesetHeader::read_info_header(modelOffset, 
                                                     feModelHeader.sidesetArray, 
                                                     instance,
                                                     feSideSetH);
    print_sideset_headers("SideSet headers:", feSideSetH, feModelHeader.sidesetArray.numEntities);
  }

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::ModelEntry::read_metadata_info(Tqdcfr *tqd) 
{
  if (debug) std::cout << "Geom metadata:" << std::endl;
  tqd->read_meta_data(modelOffset+feModelHeader.geomArray.metaDataOffset,
                      geomMD);
  if (debug) std::cout << "Node metadata:" << std::endl;
  tqd->read_meta_data(modelOffset+feModelHeader.nodeArray.metaDataOffset,
                      nodeMD);
  if (debug) std::cout << "Elem metadata:" << std::endl;
  tqd->read_meta_data(modelOffset+feModelHeader.elementArray.metaDataOffset,
                      elementMD);
  if (debug) std::cout << "Group metadata:" << std::endl;
  tqd->read_meta_data(modelOffset+feModelHeader.groupArray.metaDataOffset,
                      groupMD);
  if (debug) std::cout << "Block metadata:" << std::endl;
  tqd->read_meta_data(modelOffset+feModelHeader.blockArray.metaDataOffset,
                      blockMD);
  if (debug) std::cout << "Nodeset metadata:" << std::endl;
  tqd->read_meta_data(modelOffset+feModelHeader.nodesetArray.metaDataOffset,
                      nodesetMD);
  if (debug) std::cout << "Sideset metadata:" << std::endl;
  tqd->read_meta_data(modelOffset+feModelHeader.sidesetArray.metaDataOffset,
                      sidesetMD);

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::read_acis_records() 
{

    // get the acis model location
  int acis_model_offset = 0, acis_model_length = 0, acis_model_handle = 1,
    acis_sat_type = 1;
  for (int i = 0; i < fileTOC.numModels; i++) {
    if (modelEntries[i].modelHandle == acis_model_handle &&
        modelEntries[i].modelType == acis_sat_type) {
      acis_model_offset = modelEntries[i].modelOffset;
      acis_model_length = modelEntries[i].modelLength;
      break;
    }
  }
  
  if (acis_model_length == 0) return MB_SUCCESS;
  
  std::vector<AcisRecord> records;

    // get name for acis dump file
  MBTag acis_file_tag;
  MBErrorCode rval;
  rval = mdbImpl->tag_get_handle( acis_dump_file_tag_name, acis_file_tag );
  const char* filename = default_acis_dump_file;
  if (MB_SUCCESS == rval)
    mdbImpl->tag_get_data( acis_file_tag, 0, 0, &filename );
  
  acisDumpFile = NULL;
  if (filename && *filename)
  {
    acisDumpFile = fopen( filename, "w+" );
  }

    // position the file at the start of the acis model
  FSEEK(acis_model_offset);

  int bytes_left = acis_model_length;
  
  struct AcisRecord this_record;
  reset_record(this_record);
  char *ret;

    // make the char buffer at least buf_size+1 long, to fit null char
  const int buf_size = 1023;
  
  CHECK_SIZE(char_buf, buf_size+1);
  
  while (0 != bytes_left) {
      // read the next buff characters, or bytes_left if smaller
    int next_buf = (bytes_left > buf_size ? buf_size : bytes_left);
    FREADC(next_buf);

    if (NULL != acisDumpFile)
      fwrite(&char_buf[0], sizeof(char), next_buf, acisDumpFile);
    
      // put null at end of string to stop searches 
    char_buf[next_buf] = '\0';
    int buf_pos = 0;

      // check for first read, and if so, get rid of the header
    if (bytes_left == acis_model_length) {
        // look for 3 newlines
      ret = strchr(&(char_buf[0]), '\n'); ret = strchr(ret+1, '\n'); ret = strchr(ret+1, '\n');
      if (NULL == ret) return MB_FAILURE;
      buf_pos += ret - &(char_buf[0]) + 1;
    }
      
    bytes_left -= next_buf;

      // now start grabbing records
    do {
      
        // get next occurrence of '#' (record terminator)
      ret = strchr(&(char_buf[buf_pos]), '#');
      if (NULL != ret) {
          // grab the string (inclusive of the record terminator and the line feed) and complete the record
        int num_chars = ret-&(char_buf[buf_pos])+2;
        this_record.att_string.append(&(char_buf[buf_pos]), num_chars);
        buf_pos += num_chars;
        process_record(this_record);

          // put the record in the list...
        records.push_back(this_record);

          // and reset the record
        reset_record(this_record);
      }
      else {
          // reached end of buffer; cache string then go get another; discard last character,
          // which will be the null character
        this_record.att_string.append(&(char_buf[buf_pos]), next_buf-buf_pos);
        buf_pos = next_buf;
      }
      
    }
    while (buf_pos < next_buf);
  }

  if (NULL != acisDumpFile)
    fwrite("\n======================\nSorted acis records:\n======================\n", 1, 68, acisDumpFile);
    
    // now interpret the records
  interpret_acis_records(records);
  
  if (NULL != acisDumpFile)
    fclose(acisDumpFile);

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::interpret_acis_records(std::vector<AcisRecord> &records) 
{
    // make a tag for the vector holding unrecognized attributes
  void *default_val = NULL;
  MBErrorCode result = 
    mdbImpl->tag_create("ATTRIB_VECTOR", sizeof(void*), MB_TAG_SPARSE, 
                        attribVectorTag, &default_val);
  if (MB_SUCCESS != result && MB_ALREADY_ALLOCATED != result) return result;

  int current_record = 0;

#define REC records[current_record]

  while (current_record != (int) records.size()) {

      // if this record's been processed, or if it's an attribute, continue
    if (REC.processed || REC.rec_type == Tqdcfr::ATTRIB) {
      current_record++;
      continue;
    }

    if (REC.rec_type == Tqdcfr::UNKNOWN) {
      REC.processed = true;
      current_record++;
      continue;
    }
    
      // it's a known, non-attrib rec type; parse for any attribs
    parse_acis_attribs(current_record, records);

    REC.processed = true;
    
    current_record++;
  }

  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::parse_acis_attribs(const int entity_rec_num,
                                       std::vector<AcisRecord> &records) 
{
  int num_read;
  std::vector<std::string> *attrib_vec = NULL;
  char temp_name[80], *name_tag = NULL;
  int id = -1;
  int uid = -1;
  int next_attrib = -1;
  MBErrorCode result;
  
  int current_attrib = records[entity_rec_num].first_attrib;
  if (-1 == current_attrib) return MB_SUCCESS;

  if (NULL != acisDumpFile) {
    fwrite("-----------------------------------------------------------------------\n", 1, 72, acisDumpFile);
    fwrite(records[entity_rec_num].att_string.c_str(), sizeof(char), 
           records[entity_rec_num].att_string.length(), acisDumpFile);
  }

  while (-1 != current_attrib) {
    if (records[current_attrib].rec_type != Tqdcfr::UNKNOWN &&
           (records[current_attrib].att_next != next_attrib ||
            records[current_attrib].att_ent_num != entity_rec_num)) return MB_FAILURE;
    
    if (NULL != acisDumpFile)
      fwrite(records[current_attrib].att_string.c_str(), sizeof(char), 
             records[current_attrib].att_string.length(), acisDumpFile);

      // is the attrib one we already recognize?
    if (strncmp(records[current_attrib].att_string.c_str(), "ENTITY_NAME", 11) == 0) {
        // parse name
      int num_chars;
      num_read = sscanf(records[current_attrib].att_string.c_str(), "ENTITY_NAME @%d %s", &num_chars, temp_name);
      if (num_read != 2)
        num_read = sscanf(records[current_attrib].att_string.c_str(), "ENTITY_NAME %d %s", &num_chars, temp_name);
      if (num_read != 2) return MB_FAILURE;

        // put the name on the entity
      name_tag = new char[num_chars+1];
      strcpy(name_tag, temp_name);
    }
    else if (strncmp(records[current_attrib].att_string.c_str(), "ENTITY_ID", 9) == 0) {
        // parse id
      int bounding_uid, bounding_sense;
      num_read = sscanf(records[current_attrib].att_string.c_str(), "ENTITY_ID 0 3 %d %d %d", 
                        &id, &bounding_uid, &bounding_sense);
      if (3 != num_read) {
          // try reading updated entity_id format, which has coordinate triple embedded in it too
        float dumx, dumy, dumz;
        num_read = sscanf(records[current_attrib].att_string.c_str(), 
                          "ENTITY_ID 3 %f %f %f 3 %d %d %d", 
                          &dumx, &dumy, &dumz, &id, &bounding_uid, &bounding_sense);
        num_read -= 3;
      }
      
      if (3 != num_read)  
        std::cout << "Warning: bad ENTITY_ID attribute in .sat file, record number " << entity_rec_num
                  << ", record follows:" << std::endl
                  << records[current_attrib].att_string.c_str() << std::endl;
      ;
    }
    else if (strncmp(records[current_attrib].att_string.c_str(), "UNIQUE_ID", 9) == 0) {
        // parse uid
      num_read = sscanf(records[current_attrib].att_string.c_str(), "UNIQUE_ID 1 0 1 %d", &uid);
      if (1 != num_read) return MB_FAILURE;
    }
    else {
      if (attrib_vec == NULL) attrib_vec = new std::vector<std::string>;
      attrib_vec->push_back(records[current_attrib].att_string);
    }

    records[current_attrib].processed = true;
    next_attrib = current_attrib;
    current_attrib = records[current_attrib].att_prev;
  }

    // at this point, there aren't entity sets for entity types which don't contain mesh
    // in this case, just return
  if (records[entity_rec_num].rec_type == aBODY ||
      (records[entity_rec_num].entity == 0 && uid == -1)) {
    return MB_SUCCESS;
      // Warning: couldn't resolve entity of type 1 because no uid was found.
      // ddriv: GeomTopoTool.cpp:172: MBErrorCode GeomTopoTool::separate_by_dimension(const MBRange&, MBRange*, void**): Assertion `false' failed.
      // xxx
  }
  
    // parsed the data; now put on mdb entities; first we need to find the entity
  if (records[entity_rec_num].entity == 0) {
      // get the choices of entity
    MBRange entities;
    if (uid == -1) return MB_FAILURE;
    const void *dum_ptr = &uid;
    result = mdbImpl->get_entities_by_type_and_tag(0, MBENTITYSET, &uniqueIdTag, 
                                                   &dum_ptr, 1, entities);
    if (MB_SUCCESS != result) return result;
    if (entities.size() != 1) return MB_FAILURE;
    else records[entity_rec_num].entity = *entities.begin();
  }
  
    // set the id
  if (id != -1) {
    result = mdbImpl->tag_set_data(globalIdTag, &(records[entity_rec_num].entity), 1, &id);
    if (MB_SUCCESS != result) return result;
  }
  
    // set the name
  if (NULL != name_tag) {
    if (0 == entityNameTag) {
      result = mdbImpl->tag_get_handle("NAME", entityNameTag);
      if (MB_SUCCESS != result || 0 == entityNameTag) {
        char *dum_val = NULL;
        result = mdbImpl->tag_create("NAME", sizeof(char*), MB_TAG_SPARSE, 
                                     entityNameTag, &dum_val);
      }
    }
    if (0 == entityNameTag) return MB_FAILURE;

    result = mdbImpl->tag_set_data(entityNameTag, &(records[entity_rec_num].entity), 1, &name_tag);
    if (MB_SUCCESS != result) return result;
  }

  if (NULL != attrib_vec) {
      // put the attrib vector in a tag on the entity
    std::vector<std::string> *dum_vec;
    result = mdbImpl->tag_get_data(attribVectorTag, &(records[entity_rec_num].entity), 1, &dum_vec);
    if (MB_SUCCESS != result && MB_TAG_NOT_FOUND != result) return result;
    if (MB_TAG_NOT_FOUND == result || dum_vec == NULL) {
        // put this list directly on the entity
      result = mdbImpl->tag_set_data(attribVectorTag, &(records[entity_rec_num].entity), 1, &attrib_vec);
      if (MB_SUCCESS != result) return result;
    }
    else {
        // copy this list over, and delete this list
      std::copy(attrib_vec->begin(), attrib_vec->end(), 
                std::back_inserter(*dum_vec));
      delete attrib_vec;
    }
  }
  
  return MB_SUCCESS;
}

MBErrorCode Tqdcfr::reset_record(AcisRecord &this_record) 
{
  this_record.rec_type = Tqdcfr::UNKNOWN;
  static std::string blank;
  this_record.att_string = blank;
  this_record.first_attrib = this_record.att_prev = 
    this_record.att_next = this_record.att_ent_num = -1;
  this_record.processed = false;
  this_record.entity = 0;

  return MB_SUCCESS;
}
  
MBErrorCode Tqdcfr::process_record(AcisRecord &this_record)
{
    // get the entity type
  const char *type_substr;

    // try attribs first, since the others have some common processing between them
  if ((type_substr = strstr(this_record.att_string.c_str(), "attrib")) != NULL && 
      type_substr-this_record.att_string.c_str() < 20) {
    this_record.rec_type = Tqdcfr::ATTRIB;
    bool simple_attrib = false;
    if ((type_substr = strstr(this_record.att_string.c_str(), "simple-snl-attrib")) != NULL)
      simple_attrib = true;
    else {
      this_record.rec_type = Tqdcfr::UNKNOWN;
      return MB_SUCCESS;
    }

      // find next space
    type_substr = strchr(type_substr, ' ');
    if (NULL == type_substr) return MB_FAILURE;
    
      // read the numbers from there
    int num_converted = sscanf(type_substr, " $-1 -1 $%d $%d $%d -1", &(this_record.att_prev), 
                               &(this_record.att_next), &(this_record.att_ent_num));
    if (num_converted != 3) return MB_FAILURE;
    
      // trim the string to the attribute, if it's a simple attrib
    if (simple_attrib) {
      type_substr = strstr(this_record.att_string.c_str(), "NEW_SIMPLE_ATTRIB");
      if (NULL == type_substr) return MB_FAILURE;
      type_substr = strstr(type_substr, "@");
      if (NULL == type_substr) return MB_FAILURE;
      type_substr = strstr(type_substr, " ") + 1;
      if (NULL == type_substr) return MB_FAILURE;
        // copy the rest of the string to a dummy string
      std::string dum_str(type_substr);
      this_record.att_string = dum_str;
    }
  }
  else {
      // else it's a topological entity, I think
    if ((type_substr = strstr(this_record.att_string.c_str(), "body")) != NULL 
        && type_substr-this_record.att_string.c_str() < 20) {
      this_record.rec_type = Tqdcfr::aBODY;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "lump")) != NULL  && 
             type_substr-this_record.att_string.c_str() < 20) {
      this_record.rec_type = Tqdcfr::LUMP;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "shell")) != NULL && 
             type_substr-this_record.att_string.c_str() < 20) {
        // don't care about shells
      this_record.rec_type = Tqdcfr::UNKNOWN;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "surface")) != NULL && 
             type_substr-this_record.att_string.c_str() < 20) {
        // don't care about surfaces
      this_record.rec_type = Tqdcfr::UNKNOWN;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "face")) != NULL && 
             type_substr-this_record.att_string.c_str() < 20) {
      this_record.rec_type = Tqdcfr::FACE;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "loop")) != NULL && 
             type_substr-this_record.att_string.c_str() < 20) {
        // don't care about loops
      this_record.rec_type = Tqdcfr::UNKNOWN;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "coedge")) != NULL && 
             type_substr-this_record.att_string.c_str() < 20) {
        // don't care about coedges
      this_record.rec_type = Tqdcfr::UNKNOWN;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "edge")) != NULL && 
             type_substr-this_record.att_string.c_str() < 20) {
      this_record.rec_type = Tqdcfr::aEDGE;
    }
    else if ((type_substr = strstr(this_record.att_string.c_str(), "vertex")) != NULL && 
             type_substr-this_record.att_string.c_str() < 20) {
      this_record.rec_type = Tqdcfr::aVERTEX;
    }
    else 
      this_record.rec_type = Tqdcfr::UNKNOWN;
    
    if (this_record.rec_type != Tqdcfr::UNKNOWN) {

        // print a warning if it looks like there are sequence numbers
      if (type_substr != this_record.att_string.c_str())
        std::cout << "Warning: acis file has sequence numbers!" << std::endl;

        // scan ahead to the next white space
      type_substr = strchr(type_substr, ' ');
      if (NULL == type_substr) return MB_FAILURE;
      
        // get the id of the first attrib
      int num_converted = sscanf(type_substr, " $%d", &(this_record.first_attrib));
      if (num_converted != 1) return MB_FAILURE;
    }
  }

  return MB_SUCCESS;
}

Tqdcfr::FileTOC::FileTOC()
    : fileEndian(0), fileSchema(0), numModels(0), modelTableOffset(0), 
      modelMetaDataOffset(0), activeFEModel(0) {}
    
void Tqdcfr::FileTOC::print()
{
  std::cout << "FileTOC:End, Sch, #Mdl, TabOff, "
            << "MdlMDOff, actFEMdl = ";
  std::cout << fileEndian << ", " << fileSchema << ", " << numModels 
            << ", " << modelTableOffset << ", " 
            << modelMetaDataOffset << ", " << activeFEModel << std::endl;
}

Tqdcfr::FEModelHeader::ArrayInfo::ArrayInfo()
    : numEntities(0), tableOffset(0), metaDataOffset(0) 
{}

      
void Tqdcfr::FEModelHeader::ArrayInfo::print()
{
  std::cout << "ArrayInfo:numEntities, tableOffset, metaDataOffset = "
            << numEntities << ", " << tableOffset << ", " << metaDataOffset << std::endl;
}

void Tqdcfr::FEModelHeader::ArrayInfo::init(const std::vector<int>& int_buf)
{
  numEntities = int_buf[0]; tableOffset = int_buf[1]; metaDataOffset = int_buf[2];
}

void Tqdcfr::FEModelHeader::print()
{
  std::cout << "FEModelHeader:feEndian, feSchema, feCompressFlag, feLength = "
            << feEndian << ", " << feSchema << ", " << feCompressFlag << ", " << feLength << std::endl;
        
  std::cout << "geomArray: "; geomArray.print();
  std::cout << "nodeArray: "; nodeArray.print();
  std::cout << "elementArray: "; elementArray.print();
  std::cout << "groupArray: "; groupArray.print();
  std::cout << "blockArray: "; blockArray.print();
  std::cout << "nodesetArray: "; nodesetArray.print();
  std::cout << "sidesetArray: "; sidesetArray.print();
}

Tqdcfr::GeomHeader::GeomHeader()
    : geomID(0), nodeCt(0), nodeOffset(0), elemCt(0), elemOffset(0), 
      elemTypeCt(0), elemLength(0), setHandle(0)
{}

void Tqdcfr::GeomHeader::print() 
{
  std::cout << "geomID = " << geomID << std::endl;
  std::cout << "nodeCt = " << nodeCt << std::endl;
  std::cout << "nodeOffset = " << nodeOffset << std::endl;
  std::cout << "elemCt = " << elemCt << std::endl;
  std::cout << "elemOffset = " << elemOffset << std::endl;
  std::cout << "elemTypeCt = " << elemTypeCt << std::endl;
  std::cout << "elemLength = " << elemLength << std::endl;
  std::cout << "setHandle = " << setHandle << std::endl;
}

Tqdcfr::GroupHeader::GroupHeader()
    : grpID(0), grpType(0), memCt(0), memOffset(0), memTypeCt(0), grpLength(0),
      setHandle(0)
{}

void Tqdcfr::GroupHeader::print() 
{
  std::cout << "grpID = " << grpID << std::endl;
  std::cout << "grpType = " << grpType << std::endl;
  std::cout << "memCt = " << memCt << std::endl;
  std::cout << "memOffset = " << memOffset << std::endl;
  std::cout << "memTypeCt = " << memTypeCt << std::endl;
  std::cout << "grpLength = " << grpLength << std::endl;
  std::cout << "setHandle = " << setHandle << std::endl;
}

Tqdcfr::BlockHeader::BlockHeader()
    : blockID(0), blockElemType(0), memCt(0), memOffset(0), memTypeCt(0), attribOrder(0), blockCol(0),
      blockMixElemType(0), blockPyrType(0), blockMat(0), blockLength(0), blockDim(0),
      setHandle(0), blockEntityType(MBMAXTYPE)
{}

void Tqdcfr::BlockHeader::print() 
{
  std::cout << "blockID = " << blockID << std::endl;
  std::cout << "blockElemType = " << blockElemType << std::endl;
  std::cout << "memCt = " << memCt << std::endl;
  std::cout << "memOffset = " << memOffset << std::endl;
  std::cout << "memTypeCt = " << memTypeCt << std::endl;
  std::cout << "attribOrder = " << attribOrder << std::endl;
  std::cout << "blockCol = " << blockCol << std::endl;
  std::cout << "blockMixElemType = " << blockMixElemType << std::endl;
  std::cout << "blockPyrType = " << blockPyrType << std::endl;
  std::cout << "blockMat = " << blockMat << std::endl;
  std::cout << "blockLength = " << blockLength << std::endl;
  std::cout << "blockDim = " << blockDim << std::endl;
  std::cout << "setHandle = " << setHandle << std::endl;
  std::cout << "blockEntityType = " << blockEntityType << std::endl;
    }

Tqdcfr::NodesetHeader::NodesetHeader()
    : nsID(0), memCt(0), memOffset(0), memTypeCt(0), pointSym(0), nsCol(0), nsLength(0),
      setHandle(0)
{}

void Tqdcfr::NodesetHeader::print() 
{
  std::cout << "nsID = " << nsID << std::endl;
  std::cout << "memCt = " << memCt << std::endl;
  std::cout << "memOffset = " << memOffset << std::endl;
  std::cout << "memTypeCt = " << memTypeCt << std::endl;
  std::cout << "pointSym = " << pointSym << std::endl;
  std::cout << "nsCol = " << nsCol << std::endl;
  std::cout << "nsLength = " << nsLength << std::endl;
  std::cout << "setHandle = " << setHandle << std::endl;
    }

Tqdcfr::SidesetHeader::SidesetHeader()
    : ssID(0), memCt(0), memOffset(0), memTypeCt(0), numDF(0), ssCol(0), useShell(0), ssLength(0),
      setHandle(0)
{}

void Tqdcfr::SidesetHeader::print() 
{
  std::cout << "ssID = " << ssID << std::endl;
  std::cout << "memCt = " << memCt << std::endl;
  std::cout << "memOffset = " << memOffset << std::endl;
  std::cout << "memTypeCt = " << memTypeCt << std::endl;
  std::cout << "numDF = " << numDF << std::endl;
  std::cout << "ssCol = " << ssCol << std::endl;
  std::cout << "useShell = " << useShell << std::endl;
  std::cout << "ssLength = " << ssLength << std::endl;
  std::cout << "setHandle = " << setHandle << std::endl;
}

Tqdcfr::MetaDataContainer::MetaDataEntry::MetaDataEntry()
    : mdOwner(0), mdDataType(0), mdIntValue(0), 
      mdName("(uninit)"), mdStringValue("(uninit)"), mdDblValue(0) 
{}

void Tqdcfr::MetaDataContainer::MetaDataEntry::print()
{
  std::cout << "MetaDataEntry:own, typ, name, I, D, S = "
            << mdOwner << ", " << mdDataType << ", " << mdName << ", " << mdIntValue << ", " 
            << mdDblValue << ", " << mdStringValue;
  unsigned int i;
  if (mdIntArrayValue.size()) {
    std::cout << std::endl << "IArray = " << mdIntArrayValue[0];
    for (i = 1; i < mdIntArrayValue.size(); i++)
      std::cout << ", " << mdIntArrayValue[i];
  }
  if (mdDblArrayValue.size()) {
    std::cout << std::endl << "DArray = " << mdDblArrayValue[0];
    for (i = 1; i < mdDblArrayValue.size(); i++)
      std::cout << ", " << mdDblArrayValue[i];
  }
  std::cout << std::endl;
}

void Tqdcfr::MetaDataContainer::print()
{
  std::cout << "MetaDataContainer:mdSchema, compressFlag, numDatums = "
            << mdSchema << ", " << compressFlag << ", " << numDatums << std::endl;

  for (int i = 0; i < numDatums; i++)
    metadataEntries[i].print();
}

Tqdcfr::MetaDataContainer::MetaDataContainer()
    : mdSchema(0), compressFlag(0), numDatums(0), metadataEntries(NULL) 
{}

Tqdcfr::MetaDataContainer::~MetaDataContainer()
{
  if (NULL != metadataEntries) delete [] metadataEntries;
}

int Tqdcfr::MetaDataContainer::get_md_entry(const int owner, const std::string &name) 
{
  for (int i = 0; i < numDatums; i++)
    if (owner == metadataEntries[i].mdOwner && name == metadataEntries[i].mdName) return i;
    
  return -1;
}

Tqdcfr::ModelEntry::ModelEntry()
    : modelHandle(0), modelOffset(0), 
      modelLength(0), modelType(0), modelOwner(0), modelPad(0),
      feGeomH(NULL), feGroupH(NULL), feBlockH(NULL), 
      feNodeSetH(NULL), feSideSetH(NULL)
{}

Tqdcfr::ModelEntry::~ModelEntry()
{
  delete [] feGeomH; delete [] feGroupH; delete [] feBlockH;
  delete [] feNodeSetH; delete [] feSideSetH;
}

void Tqdcfr::ModelEntry::print()
{
  std::cout << "ModelEntry: Han, Of, Len, Tp, Own, Pd = "
            << modelHandle << ", " << modelOffset << ", " << modelLength 
            << ", " << modelType << ", " << modelOwner << ", " << modelPad
            << std::endl;
}


#ifdef TEST_TQDCFR
#include "MBCore.hpp"
int main(int argc, char* argv[])
{

    // Check command line arg
  const char* file = "test/block.cub";
  if (argc < 2)
  {
    std::cout << "Usage: tqdcfr <cub_file_name>" << std::endl;
      //exit(1);
  }
  else
    file = argv[1];

  MBCore my_impl;
  MBInterface* mdbImpl = &my_impl;
  Tqdcfr my_tqd(&my_impl);

  MBErrorCode result = my_tqd.load_file(file, 0, 0);

  if (MB_SUCCESS == result) std::cout << "Success." << std::endl;
  else
    std::cout << "load_file returned error." << std::endl;
  
}
#endif
