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

#include "MBCN.hpp"
#include "MBCNArrays.hpp"
#include <assert.h>

const char *MBCN::entityTypeNames[] = {
"Vertex",
"Edge",
"Tri",
"Quad",
"Polygon",
"Tet",
"Pyramid",
"Prism",
"Knife",
"Hex",
"Polyhedron",
"EntitySet",
"MaxType"
};

int MBCN::numberBasis = 0;

const MBDimensionPair MBCN::TypeDimensionMap[] = 
{
  MBDimensionPair(MBVERTEX,   MBVERTEX), 
  MBDimensionPair(MBEDGE,     MBEDGE), 
  MBDimensionPair(MBTRI,     MBPOLYGON),
  MBDimensionPair(MBTET,     MBPOLYHEDRON),
  MBDimensionPair(MBENTITYSET, MBENTITYSET), 
  MBDimensionPair(MBMAXTYPE, MBMAXTYPE)
};

MBEntityType MBCN::lower_bound( int dimension )
{
  if (dimension < 3)
    return static_cast<MBEntityType>(dimension);
  else if (dimension == 3)
    return MBTET;
  else
    return MBMAXTYPE;
}

  //! set the basis of the numbering system; may or may not do things besides setting the
//! member variable
void MBCN::SetBasis(const int in_basis) 
{
  numberBasis = in_basis;
}

//! return a type for the given name
MBEntityType MBCN::EntityTypeFromName(const char *name)
{
  for (MBEntityType i = MBVERTEX; i < MBMAXTYPE; i++) {
    if (0 == strcmp(name, entityTypeNames[i]))
      return i;
  }
  
  return MBMAXTYPE;
}

//! return the vertices of the specified sub entity
//! \param parent_conn Connectivity of parent entity
//! \param parent_type Entity type of parent entity
//! \param sub_dimension Dimension of sub-entity being queried
//! \param sub_index Index of sub-entity being queried
//! \param sub_entity_conn Connectivity of sub-entity, based on parent_conn and canonical
//!           ordering for parent_type
//! \param num_sub_vertices Number of vertices in sub-entity
void MBCN::SubEntityConn(const void *parent_conn, const MBEntityType parent_type,
                         const int sub_dimension,
                         const int sub_index,
                         void *sub_entity_conn, int &num_sub_vertices) 
{
  static int sub_indices[MB_MAX_SUB_ENTITY_VERTICES];
  
  SubEntityVertexIndices(parent_type, sub_dimension, sub_index, sub_indices);
  
  num_sub_vertices = VerticesPerEntity(SubEntityType(parent_type, sub_dimension, sub_index));
  void **parent_conn_ptr = static_cast<void **>(const_cast<void *>(parent_conn));
  void **sub_conn_ptr = static_cast<void **>(sub_entity_conn);
  for (int i = 0; i < num_sub_vertices; i++)
    sub_conn_ptr[i] = parent_conn_ptr[sub_indices[i]];
}

//! given an entity and a target dimension & side number, get that entity
int MBCN::AdjacentSubEntities(const MBEntityType this_type,
                                const int *source_indices,
                                const int num_source_indices,
                                const int source_dim,
                                const int target_dim,
                                std::vector<int> &index_list,
                                const int operation_type)
{
    // first get all the vertex indices
  std::vector<int> tmp_indices;
  const int* it1 = source_indices;

  assert(source_dim >= 0 && source_dim <= 3 &&
         target_dim >= 0 && target_dim <= 3 &&
           // make sure we're not stepping off the end of the array; 
         ((source_dim > 0 && 
           *it1 < mConnectivityMap[this_type][source_dim-1].num_sub_elements) ||
          (source_dim == 0 && 
           *it1 < mConnectivityMap[this_type][Dimension(this_type)-1].num_nodes_per_sub_element[0])) && 
         *it1 >= 0);


#define MUC MBCN::mUpConnMap[this_type][source_dim][target_dim]

    // if we're looking for the vertices of a single side, return them in
    // the canonical ordering; otherwise, return them in sorted order
  if (num_source_indices == 1 && 0 == target_dim && source_dim != target_dim) {

      // element of mConnectivityMap should be for this type and for one
      // less than source_dim, which should give the connectivity of that sub element
    const ConnMap &cm = mConnectivityMap[this_type][source_dim-1];
    std::copy(cm.conn[source_indices[0]],
              cm.conn[source_indices[0]]+cm.num_nodes_per_sub_element[source_indices[0]],
              std::back_inserter(index_list));
    return 0;
  }
              
    // now go through source indices, folding adjacencies into target list
  for (it1 = source_indices; it1 != source_indices+num_source_indices; it1++) {
      // *it1 is the side index
      // at start of iteration, index_list has the target list

      // if a union, or first iteration and index list was empty, copy the list
    if (operation_type == MBCN::UNION || 
        (it1 == source_indices && index_list.empty())) {
      std::copy(MUC.targets_per_source_element[*it1],
                MUC.targets_per_source_element[*it1]+
                MUC.num_targets_per_source_element[*it1],
                std::back_inserter(index_list));
    }
    else {
        // else we're intersecting, and have a non-empty list; intersect with this target list
      tmp_indices.clear();
      std::set_intersection(MUC.targets_per_source_element[*it1],
                            MUC.targets_per_source_element[*it1]+
                            MUC.num_targets_per_source_element[*it1],
                            index_list.begin(), index_list.end(),
                            std::back_inserter(tmp_indices));
      index_list.swap(tmp_indices);

        // if we're at this point and the list is empty, the intersection will be NULL;
        // return if so
      if (index_list.empty()) return 0;
    }
  }
  
  if (operation_type == MBCN::UNION && num_source_indices != 1) {
      // need to sort then unique the list
    std::sort(index_list.begin(), index_list.end());
    index_list.erase(std::unique(index_list.begin(), index_list.end()), 
                     index_list.end());
  }
  
  return 0;
}

int MBCN::SideNumber(const void *parent_conn, 
                       const MBEntityType parent_type,
                       const void *child_conn,
                       const int child_num_verts,
                       const int child_dim,
                       int &side_no,
                       int &sense,
                       int &offset)
{
  int parent_dim = Dimension(parent_type);
  int parent_num_verts = VerticesPerEntity(parent_type);

  const int *parent_conn_i = static_cast<const int *>(parent_conn);
  const int *child_conn_i = static_cast<const int *>(child_conn);
  
  if (child_dim == 0) {
      // getting the vertex number - special case this
    
    const int *parent_it = 
      std::find(parent_conn_i, parent_conn_i+parent_num_verts, *child_conn_i);
    side_no = (parent_it != parent_conn_i+parent_num_verts) ? parent_it - parent_conn_i : -1;
    sense = 0;
    offset = 0;
    return 0;
  }
    
    // given a parent and child element, find the corresponding side number

    // dim_diff should be -1, 0 or 1 (same dimension, one less dimension, two less, resp.)
  if (0 > child_dim || 3 < child_dim) return 1;

    // different types of same dimension won't be the same
  if (parent_dim == child_dim &&
      parent_num_verts != child_num_verts) {
    side_no = -1;
    sense = 0;
    return 0;
  }

    // loop over the sub-elements, comparing to child connectivity
  int sub_conn[10];
  for (int i = 0; i < NumSubEntities(parent_type, child_dim); i++) {
    if (VerticesPerEntity(SubEntityType(parent_type, child_dim, i)) != 
        child_num_verts) continue;

      // for this sub-element, get the right vertex handles
    for (int j = 0; j < VerticesPerEntity(SubEntityType(parent_type, child_dim, i)); j++)
      sub_conn[j] = parent_conn_i[mConnectivityMap[parent_type][child_dim-1].conn[i][j]];
    
    bool they_match = ConnectivityMatch(child_conn_i, sub_conn, 
                                        VerticesPerEntity(SubEntityType(parent_type, child_dim, i)), 
                                        sense, offset);
    if (they_match) {
      side_no = i;
      return 0;
    }
  }

    // if we've gotten here, we don't match
  side_no = -1;

    // return value is still success, we didn't have any fatal errors or anything
  return 0;
}

bool MBCN::ConnectivityMatch(const void *conn1,
                               const void *conn2,
                               const int num_vertices,
                               int &direct, int &offset)
{

  bool they_match;

  const int *conn1_i = static_cast<const int *>(conn1);
  const int *conn2_i = static_cast<const int *>(conn2);
  
    // special test for 2 handles, since we don't want to wrap the list in this
    // case
  if (num_vertices == 2) {
    they_match = false;
    if (conn1_i[0] == conn2_i[0] && conn1_i[1] == conn2_i[1]) {
      direct = 1;
      they_match = true;
      offset = 0;
    }
    else if (conn1_i[0] == conn2_i[1] && conn1_i[1] == conn2_i[0]) {
      they_match = true;
      direct = -1;
      offset = 1;
    }
  }

  else {
    const int *iter;
    iter = std::find(&conn2_i[0], &conn2_i[num_vertices], conn1_i[0]);
    if(iter == &conn2_i[num_vertices])
      return false;

    they_match = true;

    offset = iter - conn2_i;
    int i;

      // first compare forward
    for(i = 1; i<num_vertices; ++i)
    {
      if(conn1_i[i] != conn2_i[(offset+i)%num_vertices])
      {
        they_match = false;
        break;
      }
    }
  
    if(they_match == true)
    {
      direct = 1;
      return they_match;
    }
  
    they_match = true;
  
      // then compare reverse
    for(i = 1; i<num_vertices; i++)
    {
      if(conn1_i[i] != conn2_i[(offset+num_vertices-i)%num_vertices])
      {
        they_match = false;
        break;
      }
    }
    if (they_match)
    {
      direct = -1;
    }
  }

  return they_match;
}

  //! for an entity of this type and a specified subfacet (dimension and index), return
  //! the index of the higher order node for that entity in this entity's connectivity array
int MBCN::HONodeIndex(const MBEntityType this_type, const int num_verts,
                        const int subfacet_dim, const int subfacet_index) 
{
  int i;
  int has_mids[4];
  HasMidNodes(this_type, num_verts, has_mids);

    // if we have no mid nodes on the subfacet_dim, we have no index
  if (subfacet_index != -1 && !has_mids[subfacet_dim]) return -1;

    // put start index at last index (one less than the number of vertices 
    // plus the index basis)
  int index = VerticesPerEntity(this_type) - 1 + numberBasis;

    // for each subfacet dimension less than the target subfacet dim which has mid nodes, 
    // add the number of subfacets of that dimension to the index
  for (i = 1; i < subfacet_dim; i++)
    if (has_mids[i]) index += NumSubEntities(this_type, i);
    

    // now add the index of this subfacet, or one if we're asking about the entity as a whole
  if (subfacet_index == -1 && has_mids[subfacet_dim])
      // want the index of the last ho node on this subfacet
    index += NumSubEntities(this_type, subfacet_dim);
  
  else if (subfacet_index != -1 && has_mids[subfacet_dim])
    index += subfacet_index + 1 - numberBasis;

    // that's it
  return index;
}

  //! given data about an element and a vertex in that element, return the dimension
  //! and index of the sub-entity that the vertex resolves.  If it does not resolve a
  //! sub-entity, either because it's a corner node or it's not in the element, -1 is
  //! returned in both return values
void MBCN::HONodeParent(const void *elem_conn, const MBEntityType elem_type,
                          const int num_verts, const void *ho_node,
                          int &parent_dim, int &parent_index) 
{
  const int *elem_conn_i = static_cast<const int *>(elem_conn);
  const int *ho_node_i = static_cast<const int *>(ho_node);

    // pre-set return values to error values
  parent_dim = -1;
  parent_index = -1;

    // check for invalid input
  if (NULL == elem_conn || NULL == ho_node || elem_type < MBVERTEX ||
      elem_type > MBMAXTYPE || num_verts < VerticesPerEntity(elem_type))
    return;
  
    // find the index of the node in the connectivity array
  int ho_index = std::find(elem_conn_i, elem_conn_i+num_verts, *ho_node_i) - 
    elem_conn_i;
  if (ho_index < VerticesPerEntity(elem_type) || ho_index >= num_verts)
    return;
    
    // given the number of verts and the element type, get the hasmidnodes solution
  int has_mids[4];
  HasMidNodes(elem_type, num_verts, has_mids);

  int index = VerticesPerEntity(elem_type)-1;

    // keep a running sum of the ho node indices for this type of element, and stop
    // when you get to the dimension which has the ho node
  for (int i = 1; i <= Dimension(elem_type); i++) {
    if (has_mids[i]) {
      if (ho_index <= index + NumSubEntities(elem_type, i)) {
          // the ho_index resolves an entity of dimension i, so set the return values
          // and break out of the loop
        parent_dim = i;
        parent_index = ho_index - index - 1;
        break;
      }
      else if( NumSubEntities(elem_type, i) == 0 && ho_index == index+1 ) //mid region node case
      {
        parent_dim = i;
        parent_index = 0; 
      }


      else index += NumSubEntities(elem_type, i);
    }
  }
  
  return;
}
