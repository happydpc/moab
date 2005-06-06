#include "WriteGmsh.hpp"
#include "MBCN.hpp"
#include "MBTagConventions.hpp"
#include "MBParallelConventions.h"

#include <fstream>
#include <map>
#include <set>

MBWriterIface *WriteGmsh::factory( MBInterface* iface )
  { return new WriteGmsh( iface ); }

WriteGmsh::WriteGmsh(MBInterface *impl) 
    : mbImpl(impl)
{
  impl->query_interface("MBWriteUtilIface", reinterpret_cast<void**>(&mWriteIface));
}

WriteGmsh::~WriteGmsh() 
{
  mbImpl->release_interface("MBWriteUtilIface", mWriteIface);
}

// Type info indexed by type id used in file format.
const int hex_27_node_order[] =  {  
    0,  1,  2,  3,  4,  5,  6,  7,                 // corners
    8, 11, 12,  9, 13, 10, 14, 15, 16, 19, 17, 18, // edges
   24, 20, 23, 21, 22, 25,                         // faces
   26 };                                           // volume


  // A structure to store per-element information.
struct ElemInfo { 
  void set( int set, int id ) {
    while (count < set)
      sets[count++] = 0;
    sets[count++] = id;
  }
  int count;   // number of valid entries in sets[]
  int sets[3]; // ids of owning block, geom, and partition; respectively
  int id;      // global ID of element
  int type;    // Gmsh element type
};
  
    //! writes out a file
MBErrorCode WriteGmsh::write_file(const char *file_name,
                                  const bool overwrite,
                                  const MBEntityHandle *output_list,
                                  const int num_sets,
                                  std::vector<std::string>& ,
                                  int )
{
  MBErrorCode rval;
  MBTag global_id = 0, block_tag = 0, geom_tag = 0, prtn_tag = 0;

  if (!overwrite)
  {
    rval = mWriteIface->check_doesnt_exist( file_name );
    if (MB_SUCCESS != rval)
      return rval;
  }
  
    // Get tags
  mbImpl->tag_get_handle( GLOBAL_ID_TAG_NAME, global_id );
  mbImpl->tag_get_handle( MATERIAL_SET_TAG_NAME, block_tag );
  if (global_id) 
    mbImpl->tag_get_handle( GEOM_DIMENSION_TAG_NAME, geom_tag );
  mbImpl->tag_get_handle( PARALLEL_PARTITION_TAG_NAME, prtn_tag );
  
    // Define arrays to hold entity sets of interest
  MBRange sets[3];
  MBTag set_tags[] = { block_tag, geom_tag, prtn_tag };
  MBTag set_ids[] = { block_tag, 0 /*global_id*/, prtn_tag };
  
    // Get entities to write
  MBRange elements, nodes;
  if (!output_list)
  {
    rval = mbImpl->get_entities_by_dimension( 0, 0, nodes, false );
    if (MB_SUCCESS != rval)
      return rval;
    for (int d = 1; d < 3; ++d)
    {
      MBRange tmp_range;
      rval = mbImpl->get_entities_by_dimension( 0, d, tmp_range, false );
      if (MB_SUCCESS != rval)
        return rval;
      elements.merge( tmp_range );
    }
    
    for (int s = 0; s < 3; ++s)
      if (set_tags[s]) 
      {
        rval = mbImpl->get_entities_by_type_and_tag( 0, MBENTITYSET, set_tags+s, 0, 1, sets[s] );
        if (MB_SUCCESS != rval) return rval;
      }
  }
  else
  {
    for (int i = 0; i < num_sets; ++i)
    {
      MBEntityHandle set = output_list[i];
      for (int d = 1; d < 3; ++d)
      {
        MBRange tmp_range, tmp_nodes;
        rval = mbImpl->get_entities_by_dimension( set, d, tmp_range, true );
        if (rval != MB_SUCCESS)
          return rval;
        elements.merge( tmp_range );
        rval = mbImpl->get_adjacencies( tmp_range, set, false, tmp_nodes );
        if (rval != MB_SUCCESS)
          return rval;
        nodes.merge( tmp_nodes );
      }
      
      for (int s = 0; s < 3; ++s)
        if (set_tags[s])
        {
          MBRange tmp_range;
          rval = mbImpl->get_entities_by_type_and_tag( set, MBENTITYSET, set_tags+s, 0, 1, tmp_range );
          if (MB_SUCCESS != rval) return rval;
          sets[s].merge( tmp_range );
          int junk;
          rval = mbImpl->tag_get_data( set_tags[s], &set, 1, &junk );
          if (MB_SUCCESS == rval)
            sets[s].insert( set );
        }
    }
  }

  if (elements.empty())
  {
    mWriteIface->report_error( "Nothing to write.\n" );
    return  MB_ENTITY_NOT_FOUND;
  }
  
    // get global IDs for all elements.  
    // First try to get from tag.  If tag is not defined or not set
    // for all elements, use handle value instead.
  std::vector<int> global_id_array(elements.size());
  std::vector<int>::iterator id_iter;
  if (!global_id || MB_SUCCESS !=
            mbImpl->tag_get_data( global_id, elements, &global_id_array[0] ) )
  {
    id_iter = global_id_array.begin();
    for (MBRange::iterator i = elements.begin(); i != elements.end(); ++i, ++id_iter)
      *id_iter = mbImpl->id_from_handle( *i );
  }
  
    // Figure out the maximum ID value so we know where to start allocating
    // new IDs when we encounter ID conflits.
  int max_id = 0;
  for (id_iter = global_id_array.begin(); id_iter != global_id_array.end(); ++id_iter)
    if (*id_iter > max_id)
      max_id = *id_iter;
  
    // Initialize ElemInfo struct for each element
  std::map<MBEntityHandle,ElemInfo> elem_sets; // per-element info
  std::set<int> elem_global_ids;               // temporary for finding duplicate IDs
  id_iter = global_id_array.begin();
    // Iterate backwards to give highest-dimension entities first dibs for
    // a conflicting ID.
  for (MBRange::reverse_iterator i = elements.rbegin(); i != elements.rend(); ++i)
  {
    int id = *id_iter; ++id_iter;
    if (!elem_global_ids.insert(id).second)
      id = max_id++;
      
    ElemInfo& ei = elem_sets[*i];
    ei.count = 0;
    ei.id = id;
    
    MBEntityType type = mbImpl->type_from_handle( *i );
    int num_vtx;
    const MBEntityHandle* conn;
    rval = mbImpl->get_connectivity( *i, conn, num_vtx );
    if (MB_SUCCESS != rval)
      return rval;
      
    switch (type) 
    {
      case MBEDGE:    ei.type = num_vtx == 2 ? 1 :                      8; break;
      case MBTRI:     ei.type = num_vtx == 3 ? 2 :                      9; break;
      case MBQUAD:    ei.type = num_vtx == 4 ? 3 :                     10; break;
      case MBPYRAMID: ei.type = num_vtx == 5 ? 7 :                      0; break;
      case MBPRISM:   ei.type = num_vtx == 6 ? 6 :                      0; break;
      case MBTET:     ei.type = num_vtx == 4 ? 4 : num_vtx == 10 ? 11 : 0; break;
      case MBHEX:     ei.type = num_vtx == 8 ? 5 : num_vtx == 27 ? 12 : 0; break;
      default:        ei.type = 0;
    }
    
    if (ei.type == 0) 
    {
      mWriteIface->report_error( "Gmem file format does not support element "
                                 " of type %s with %d vertices.\n",
                                 MBCN::EntityTypeName( type ), num_vtx );
      return MB_FILE_WRITE_ERROR;
    }
  }
    // Don't need these any more, free memory.
  elem_global_ids.clear();
  global_id_array.clear();
  
    // For each material set, geometry set, or partition; store
    // the ID of the set on each element.
  for (int s = 0; s < 3; ++s)
  {
    if (!set_tags[s])
      continue;
      
    for (MBRange::iterator i = sets[s].begin(); i != sets[s].end(); ++i)
    {
      int id;
      if (set_ids[s]) 
      {
        rval = mbImpl->tag_get_data( set_ids[s], &*i, 1, &id );
        if (MB_SUCCESS != rval)
          return rval;
      }
      else
      {
        id = mbImpl->id_from_handle( *i );
      }
      
      MBRange elems;
      rval = mbImpl->get_entities_by_handle( *i, elems );
      if (MB_SUCCESS != rval)
        return rval;

      elems = elems.intersect( elements );
      for (MBRange::iterator j = elems.begin(); j != elems.end(); ++j)
        elem_sets[*j].set( s, id );
    }
  }


    // Create file
  std::ofstream out( file_name );
  if (!out)
    return MB_FILE_DOES_NOT_EXIST;

    
    // Write header
  out << "$MeshFormat" << std::endl;
  out << "2.0 0 " << sizeof(double) << std::endl;
  out << "$EndMeshFormat" << std::endl;

  
    // Write nodes
  out << "$Nodes" << std::endl;
  out << nodes.size() << std::endl;
  std::vector<double> coords(3*nodes.size());
  rval = mbImpl->get_coords( nodes, &coords[0] );
  if (MB_SUCCESS != rval)
    return rval;
  std::vector<double>::iterator c = coords.begin();
  for (MBRange::iterator i = nodes.begin(); i != nodes.end(); ++i)
  {
    out << mbImpl->id_from_handle( *i );
    out << " " << *c; ++c;
    out << " " << *c; ++c;
    out << " " << *c; ++c;
    out << std::endl;
  }
  out << "$EndNodes" << std::endl;
  coords.clear();
  

    // Write elements
  out << "$Elements" << std::endl;
  out << elem_sets.size() << std::endl;
  for (std::map<MBEntityHandle,ElemInfo>::iterator i = elem_sets.begin();
       i != elem_sets.end(); ++i)
  {
    int num_vtx;
    const MBEntityHandle* conn;
    rval = mbImpl->get_connectivity( i->first, conn, num_vtx );
    if (MB_SUCCESS != rval)
      return rval;
    out << i->second.id << ' ' << i->second.type << ' ' << i->second.count;
    for (int j = 0; j < i->second.count; ++j)
      out << ' ' << i->second.sets[j];
      
      // special case for Hex27 - need to re-order vertices
    if (i->second.type == 12)
    {
      for (int j = 0; j < 27; ++j)
        out << ' ' << mbImpl->id_from_handle( conn[hex_27_node_order[j]] );
    }
    else
    {  
      for (int j = 0; j < num_vtx; ++j)
        out << ' ' << mbImpl->id_from_handle( conn[j] );
    }
    out << std::endl;
  }
  out << "$EndElements" << std::endl;
  
    
    // done
  return MB_SUCCESS;
}


