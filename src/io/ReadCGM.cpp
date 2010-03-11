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
#pragma warning(disable:4786)
#endif

#include "GeometryQueryTool.hpp"
#include "ModelQueryEngine.hpp"
#include "RefEntityName.hpp"
#include "GMem.hpp"

#include "RefGroup.hpp"
#include "RefVolume.hpp"
#include "RefFace.hpp"
#include "RefEdge.hpp"
#include "RefVertex.hpp"

#include "SenseEntity.hpp"
#include "Surface.hpp"
#include "Curve.hpp"
#include "Body.hpp"
#include "InitCGMA.hpp"

#include "MBCore.hpp"
#include "MBInterface.hpp"
#include "MBRange.hpp"
#include "MBTagConventions.hpp"
#include "FileOptions.hpp"

#include "GeomTopoTool.hpp"

#include <stdio.h>
#include <algorithm>
#include <assert.h>

#include "ReadCGM.hpp"
#include "MBReadUtilIface.hpp"

#define GF_CUBIT_FILE_TYPE    "CUBIT"
#define GF_STEP_FILE_TYPE     "STEP"
#define GF_IGES_FILE_TYPE     "IGES"
#define GF_ACIS_TXT_FILE_TYPE "ACIS_SAT"
#define GF_ACIS_BIN_FILE_TYPE "ACIS_SAB"
#define GF_OCC_BREP_FILE_TYPE "OCC"

MBReaderIface* ReadCGM::factory( MBInterface* iface )
{ return new ReadCGM( iface ); }

ReadCGM::ReadCGM(MBInterface *impl)
  : geom_tag(0), id_tag(0), name_tag(0), category_tag(0), faceting_tol_tag(0)
{
  assert(NULL != impl);
  mdbImpl = impl;
  myGeomTool = new GeomTopoTool(impl);
  void* ptr = 0;
  impl->query_interface( "MBReadUtilIface", &ptr );
  assert(NULL != ptr);
  readUtilIface = reinterpret_cast<MBReadUtilIface*>(ptr);

  MBErrorCode rval;

  // get some tag handles
  rval = mdbImpl->tag_create( GEOM_DIMENSION_TAG_NAME, sizeof(int), 
                            MB_TAG_SPARSE, MB_TYPE_INTEGER, geom_tag, 0, true ); 
  assert(!rval);
  rval = mdbImpl->tag_create( GLOBAL_ID_TAG_NAME, sizeof(int), MB_TAG_DENSE,
                            MB_TYPE_INTEGER, id_tag, 0, true );
  assert(!rval);
  rval = mdbImpl->tag_create( NAME_TAG_NAME, NAME_TAG_SIZE, MB_TAG_SPARSE,
                            MB_TYPE_OPAQUE, name_tag, 0, true );
  assert(!rval);

  rval = mdbImpl->tag_create( CATEGORY_TAG_NAME, CATEGORY_TAG_SIZE,
                            MB_TAG_SPARSE, MB_TYPE_OPAQUE, category_tag, 0, true );
  assert(!rval);
  rval = mdbImpl->tag_create("FACETING_TOL", sizeof(double), MB_TAG_SPARSE,
			     MB_TYPE_DOUBLE, faceting_tol_tag, 0, true );
  assert(!rval);
}

ReadCGM::~ReadCGM()
{
  std::string iface_name = "MBReadUtilIface";
  mdbImpl->release_interface(iface_name, readUtilIface);
  delete myGeomTool;
}


MBErrorCode ReadCGM::read_tag_values( const char* /* file_name */,
                                      const char* /* tag_name */,
                                      const FileOptions& /* opts */,
                                      std::vector<int>& /* tag_values_out */,
                                      const IDTag* /* subset_list */,
                                      int /* subset_list_length */ )
{
  return MB_NOT_IMPLEMENTED;
}



// copy geometry into mesh database
MBErrorCode ReadCGM::load_file(const char *cgm_file_name,
                      const MBEntityHandle* file_set,
                      const FileOptions& opts,
                      const MBReaderIface::IDTag* subset_list,
                      int subset_list_length,
                      const MBTag* file_id_tag)
{
  // blocks_to_load and num_blocks are ignored.
  MBErrorCode rval;

  if (subset_list && subset_list_length) {
    readUtilIface->report_error( "Reading subset of files not supported for CGM data." );
    return MB_UNSUPPORTED_OPERATION;
  }

  int norm_tol, DEFAULT_NORM = 5;
  double faceting_tol, DEFAULT_FACET_TOL = 0.001, len_tol, DEFAULT_LEN_TOL = 0.0;
  bool act_att = true;
  //check for the options.
  if (MB_SUCCESS != opts.get_int_option( "FACET_NORMAL_TOLERANCE", norm_tol ))
    norm_tol = DEFAULT_NORM;

  if (MB_SUCCESS != opts.get_real_option("FACET_DISTANCE_TOLERANCE", faceting_tol))
    faceting_tol = DEFAULT_FACET_TOL;

  if (MB_SUCCESS != opts.get_real_option("MAX_FACET_EDGE_LENGTH", len_tol))
    len_tol = DEFAULT_LEN_TOL;

  const char* name = "CGM_ATTRIBS";
  const char* value = "no";
  rval = opts.match_option(name,value); 
  if(MB_SUCCESS == opts.match_option(name,value)) 
    act_att = false; 

  // tag the file_set with the faceting_tol
  if (file_set) {
    rval = mdbImpl->tag_set_data( faceting_tol_tag, file_set, 1, &faceting_tol );
    if(MB_SUCCESS != rval) return rval;
  }

  // CGM data
  std::map<RefEntity*,MBEntityHandle> entmap[5]; // one for each dim, and one for groups
  std::map<RefEntity*,MBEntityHandle>::iterator ci;
  const char geom_categories[][CATEGORY_TAG_SIZE] = 
    {"Vertex\0", "Curve\0", "Surface\0", "Volume\0", "Group\0"};
  const char* const names[] = { "Vertex", "Curve", "Surface", "Volume"};
  DLIList<RefEntity*> entlist;
  DLIList<ModelEntity*> me_list;

  // Initialize CGM
  InitCGMA::initialize_cgma();

  if (act_att) {
    CGMApp::instance()->attrib_manager()->set_all_auto_read_flags( act_att );
    CGMApp::instance()->attrib_manager()->set_all_auto_actuate_flags( act_att );
  }

  CubitStatus s;

  // Get CGM file type
  const char* file_type = 0;
  file_type = get_geom_file_type( cgm_file_name );
  if (!file_type) {
    std::cerr << cgm_file_name << " : unknown file type, try '-t'" << std::endl;
    return MB_FAILURE;
  }
  else if(!strcmp(file_type ,"CUBIT"))
  {
    std::cerr << cgm_file_name << " : CUB files are currently not supported." << std::endl;
    return MB_FAILURE;
   }
  else {
    s = GeometryQueryTool::instance()->import_solid_model( cgm_file_name, file_type );
  }

  if (CUBIT_SUCCESS != s) {
    std::cerr << "Failed to read '" << cgm_file_name << "' of type '" << file_type << "'" << std::endl;
    return MB_FAILURE;
  }

  // create entity sets for all geometric entities
  for (int dim = 0; dim < 4; ++dim) {
    entlist.clean_out();
    GeometryQueryTool::instance()->ref_entity_list( names[dim], entlist, true );
    
    entlist.reset();
    for (int i = entlist.size(); i--; ) {
      RefEntity* ent = entlist.get_and_step();
      MBEntityHandle handle;
      rval = mdbImpl->create_meshset( dim == 1 ? MESHSET_ORDERED : MESHSET_SET, handle );
      if (MB_SUCCESS != rval)
        return rval;

      entmap[dim][ent] = handle;
      
      rval = mdbImpl->tag_set_data( geom_tag, &handle, 1, &dim );
      if (MB_SUCCESS != rval)
        return rval;
      int id = ent->id();
      rval = mdbImpl->tag_set_data( id_tag, &handle, 1, &id );
      if (MB_SUCCESS != rval)
        return rval;

      rval = mdbImpl->tag_set_data( category_tag, &handle, 1, &geom_categories[dim] );
      if (MB_SUCCESS != rval)
        return rval;
    }
  }
  
    // create topology for all geometric entities
  for (int dim = 1; dim < 4; ++dim) {
    for (ci = entmap[dim].begin(); ci != entmap[dim].end(); ++ci) {
      entlist.clean_out();
      ci->first->get_child_ref_entities( entlist );
    
      entlist.reset();
      for (int i = entlist.size(); i--; ) {
        RefEntity* ent = entlist.get_and_step();
        MBEntityHandle h = entmap[dim-1][ent];
        rval = mdbImpl->add_parent_child( ci->second, h );
        if (MB_SUCCESS != rval)
          return rval;
      }
    }
  }
  
    // store CoFace senses
  for (ci = entmap[2].begin(); ci != entmap[2].end(); ++ci) {
    RefFace* face = (RefFace*)(ci->first);
    BasicTopologyEntity *forward = 0, *reverse = 0;
    for (SenseEntity* cf = face->get_first_sense_entity_ptr();
         cf; cf = cf->next_on_bte()) {
      BasicTopologyEntity* vol = cf->get_parent_basic_topology_entity_ptr();
      if (cf->get_sense() == CUBIT_UNKNOWN || 
          cf->get_sense() != face->get_surface_ptr()->bridge_sense()) {
        if (reverse) {
          std::cout << "Surface " << face->id() << " has reverse senes " <<
                       "with multiple volume " << reverse->id() << " and " <<
                       "volume " << vol->id() << std::endl;
          return MB_FAILURE;
        }
        reverse = vol;
      }
      if (cf->get_sense() == CUBIT_UNKNOWN || 
          cf->get_sense() == face->get_surface_ptr()->bridge_sense()) {
        if (forward) {
          std::cout << "Surface " << face->id() << " has forward senes " <<
                       "with multiple volume " << forward->id() << " and " <<
                       "volume " << vol->id() << std::endl;
          return MB_FAILURE;
        }
        forward = vol;
      }
    }
    
    if (forward) {
      rval = myGeomTool->set_sense( ci->second, entmap[3][forward], true );
      if (MB_SUCCESS != rval)
        return rval;
    }
    if (reverse) {
      rval = myGeomTool->set_sense( ci->second, entmap[3][reverse], false );
      if (MB_SUCCESS != rval)
        return rval;
    }
  }

    // create entity sets for all ref groups
  std::vector<MBTag> extra_name_tags;
  DLIList<CubitString*> name_list;
  entlist.clean_out();
  GeometryQueryTool::instance()->ref_entity_list( "group", entlist );
  entlist.reset();
  for (int i = entlist.size(); i--; ) {
    RefEntity* grp = entlist.get_and_step();
    name_list.clean_out();
    RefEntityName::instance()->get_refentity_name( grp, name_list, true );
    if (name_list.size() == 0)
      continue;
    name_list.reset();
    CubitString name = *name_list.get();
    
    MBEntityHandle h;
    rval = mdbImpl->create_meshset( MESHSET_SET, h );
    if (MB_SUCCESS != rval)
      return rval;
    
    char namebuf[NAME_TAG_SIZE];
    memset( namebuf, '\0', NAME_TAG_SIZE );
    strncpy( namebuf, name.c_str(), NAME_TAG_SIZE - 1 );
    if (name.length() >= (unsigned)NAME_TAG_SIZE) 
      std::cout << "WARNING: group name '" << name.c_str() 
                << "' truncated to '" << namebuf << "'" << std::endl;
    rval = mdbImpl->tag_set_data( name_tag, &h, 1, namebuf );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
      
    int id = grp->id();
    rval = mdbImpl->tag_set_data( id_tag, &h, 1, &id );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
      
    rval = mdbImpl->tag_set_data( category_tag, &h, 1, &geom_categories[4] );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
      
    if (name_list.size() > 1) {
      for (int j = extra_name_tags.size(); j < name_list.size(); ++j) {
        sprintf( namebuf, "EXTRA_%s%d", NAME_TAG_NAME, j );
        MBTag t;
        rval = mdbImpl->tag_create( namebuf, NAME_TAG_SIZE, MB_TAG_SPARSE, MB_TYPE_OPAQUE, t, 0, true );
        assert(!rval);
        extra_name_tags.push_back(t);
      }
        
      for (int j = 0; j < name_list.size(); ++j) {
        name = *name_list.get_and_step();
        memset( namebuf, '\0', NAME_TAG_SIZE );
        strncpy( namebuf, name.c_str(), NAME_TAG_SIZE - 1 );
        if (name.length() >= (unsigned)NAME_TAG_SIZE) 
          std::cout << "WARNING: group name '" << name.c_str() 
                    << "' truncated to '" << namebuf << "'" << std::endl;
        rval = mdbImpl->tag_set_data( extra_name_tags[j], &h, 1, namebuf );
        if (MB_SUCCESS != rval)
          return MB_FAILURE;
      }
    }
      
    entmap[4][grp] = h;
  }
  
    // store contents for each group
  entlist.reset();
  for (ci = entmap[4].begin(); ci != entmap[4].end(); ++ci) {
    RefGroup* grp = (RefGroup*)(ci->first);
    entlist.clean_out();
    grp->get_child_ref_entities( entlist );
    
    MBRange entities;
    while (entlist.size()) {
      RefEntity* ent = entlist.pop();
      int dim = ent->dimension();

      if (dim < 0) {

	Body* body;
        if (entmap[4].find(ent) != entmap[4].end()){
          // child is another group; examine its contents
	  entities.insert( entmap[4][ent] );
	}
	else if( (body = dynamic_cast<Body*>(ent)) != NULL ){
	  // Child is a CGM Body, which presumably comprises some volumes--
	  // extract volumes as if they belonged to group.
	  DLIList<RefVolume*> vols;
	  body->ref_volumes( vols );
	  for( int vi = vols.size(); vi--; ){
	    RefVolume* vol = vols.get_and_step();
	    if( entmap[3].find(vol) != entmap[3].end() ){
	      entities.insert( entmap[3][vol] );
	    }
	    else{
	      std::cerr << "Warning: CGM Body has orphan RefVolume" << std::endl;
	    }
	  }	  
	}
	else{
	  // otherwise, warn user.
	  std::cerr << "Warning: A dim<0 entity is being ignored by ReadCGM." << std::endl;
	}

      }
      else if (dim < 4) {
        if (entmap[dim].find(ent) != entmap[dim].end())
          entities.insert( entmap[dim][ent] );
      }
    }
    
    if (!entities.empty()) {
      rval = mdbImpl->add_entities( ci->second, entities );
      if (MB_SUCCESS != rval)
        return MB_FAILURE;
    }
  }
  
    // done with volumes and groups
  entmap[3].clear();
  entmap[4].clear();
  
    // create geometry for all vertices and replace 
    // vertex set handles with vertex handles in map
  for (ci = entmap[0].begin(); ci != entmap[0].end(); ++ci) {
    CubitVector pos = dynamic_cast<RefVertex*>(ci->first)->coordinates();
    double coords[3] = {pos.x(), pos.y(), pos.z()};
    MBEntityHandle vh;
    rval = mdbImpl->create_vertex( coords, vh );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
    
    rval = mdbImpl->add_entities( ci->second, &vh, 1 );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
    
    ci->second = vh;
  }
  
    // create geometry for all curves
  GMem data;
  for (ci = entmap[1].begin(); ci != entmap[1].end(); ++ci) {
    RefEdge* edge = dynamic_cast<RefEdge*>(ci->first);
    Curve* curve = edge->get_curve_ptr();
    data.clean_out();
    edge->get_graphics( data, faceting_tol);
    if (CUBIT_SUCCESS != s)
      return MB_FAILURE;
      
    std::vector<CubitVector> points;
    for (int i = 0; i < data.pointListCount; ++i)
      points.push_back( CubitVector( data.point_list()[i].x,
                                     data.point_list()[i].y,
                                     data.point_list()[i].z ) );

      // need to reverse data?
    if (curve->bridge_sense() == CUBIT_REVERSED) 
      std::reverse( points.begin(), points.end() );
    
       // check for closed curve
    RefVertex *start_vtx, *end_vtx;
    start_vtx = edge->start_vertex();
    end_vtx = edge->end_vertex();
    
      // Special case for point curve
    if (points.size() < 2) {
      if (start_vtx != end_vtx || curve->measure() > GEOMETRY_RESABS ) {
        std::cerr << "Warning: No facetting for curve " << edge->id() << std::endl;
        continue;
      }
      MBEntityHandle h = entmap[0][start_vtx];
      rval = mdbImpl->add_entities( ci->second, &h, 1 );
      if (MB_SUCCESS != rval)
        return MB_FAILURE;
      continue;
    }
    
    const bool closed = (points.front() - points.back()).length() < GEOMETRY_RESABS;
    if (closed != (start_vtx == end_vtx)) {
      std::cerr << "Warning: topology and geometry inconsistant for possibly closed curve "
                << edge->id() << std::endl;
    }
    
      // check proximity of vertices to end coordinates
    if ((start_vtx->coordinates() - points.front()).length() > GEOMETRY_RESABS
     || (  end_vtx->coordinates() - points.back() ).length() > GEOMETRY_RESABS ) {
      std::cerr << "Warning: vertices not at ends of curve " << edge->id() << std::endl;
    }
    
      // create interior points
    std::vector<MBEntityHandle> verts, edges;
    verts.push_back( entmap[0][start_vtx] );
    for (size_t i = 1; i < points.size() - 1; ++i) {
      double coords[] = { points[i].x(), points[i].y(), points[i].z() };
      MBEntityHandle h;
      rval = mdbImpl->create_vertex( coords, h );
      if (MB_SUCCESS != rval)
        return MB_FAILURE;
      verts.push_back( h );
    }
    verts.push_back( entmap[0][end_vtx] );
    
      // create edges
    for (size_t i = 0; i < verts.size()-1; ++i) {
      MBEntityHandle h;
      rval = mdbImpl->create_element( MBEDGE, &verts[i], 2, h );
      if (MB_SUCCESS != rval)
        return MB_FAILURE;
      edges.push_back( h );
    }
    
      // if closed, remove duplicate
    if (verts.front() == verts.back())
      verts.pop_back();
    
    rval = mdbImpl->add_entities( ci->second, &verts[0], verts.size() );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
    rval = mdbImpl->add_entities( ci->second, &edges[0], edges.size() );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
  }
  
    // create geometry for all surfaces
  for (ci = entmap[2].begin(); ci != entmap[2].end(); ++ci) {
    RefFace* face = dynamic_cast<RefFace*>(ci->first);

    data.clean_out();
    CubitStatus s = face->get_graphics( data, norm_tol, faceting_tol, len_tol );

    if (CUBIT_SUCCESS != s)
      return MB_FAILURE;

      // declare array of all vertex handles
    std::vector<MBEntityHandle> verts( data.pointListCount, 0 );
    
      // get list of vertices in surface
    me_list.clean_out();
    ModelQueryEngine::instance()->query_model( *face, DagType::ref_vertex_type(), me_list );

      // for each vertex, find coincident point in facets
    for (int i = me_list.size(); i--; ) {
      RefVertex* vtx = dynamic_cast<RefVertex*>(me_list.get_and_step());
      CubitVector pos = vtx->coordinates();

      for (int j = 0; j < data.pointListCount; ++j) {
        CubitVector vpos( data.point_list()[j].x,
                          data.point_list()[j].y,
                          data.point_list()[j].z );
        if ((pos - vpos).length_squared() < GEOMETRY_RESABS*GEOMETRY_RESABS ) {
          if (verts[j])
            std::cerr << "Warning: Coincident vertices in surface " << face->id() << std::endl;
          verts[j] = entmap[0][vtx];
          break;
        }
      }
    }
    
      // now create vertices for the remaining points in the facetting
    for (int i = 0; i < data.pointListCount; ++i) {
      if (verts[i]) // if a geometric vertex
        continue;
      double coords[] = { data.point_list()[i].x,
                          data.point_list()[i].y,
                          data.point_list()[i].z };
      rval = mdbImpl->create_vertex( coords, verts[i] );
      if (MB_SUCCESS != rval)
        return rval;
    }
    
      // now create facets
    MBRange facets;
    std::vector<MBEntityHandle> corners;
    for (int i = 0; i < data.fListCount; i += data.facet_list()[i]+1) {
      int* facet = data.facet_list() + i;
      corners.resize( *facet );
      for (int j = 1; j <= *facet; ++j) {
        if (facet[j] >= (int)verts.size()) {
          std::cerr << "ERROR: Invalid facet data for surface " << face->id() << std::endl;
          return MB_FAILURE;
        }
        corners[j-1] = verts[facet[j]];
      }
      MBEntityType type;
      if (*facet == 3)
        type = MBTRI;
      else {
        std::cerr << "Warning: non-triangle facet in surface " << face->id() << std::endl;
	std::cerr << "  entity has " << *facet << " edges" << std::endl;
        if (*facet == 4)
          type = MBQUAD;
        else
          type = MBPOLYGON;
      }
      
      // if (surf->bridge_sense() == CUBIT_REVERSED)
      //   std::reverse( corners.begin(), corners.end() );
      
      MBEntityHandle h;
      rval = mdbImpl->create_element( type, &corners[0], corners.size(), h );
      if (MB_SUCCESS != rval)
        return MB_FAILURE;
        
      facets.insert( h );
    }
    
      // add vertices and facets to surface set
    rval = mdbImpl->add_entities( ci->second, &verts[0], verts.size() );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
    rval = mdbImpl->add_entities( ci->second, facets );
    if (MB_SUCCESS != rval)
      return MB_FAILURE;
  }

  return MB_SUCCESS;
}

const char* ReadCGM::get_geom_file_type( const char* name )
{
  FILE* file;
  const char* result = 0;

  file = fopen( name, "r" );
  if (file) {
    result = get_geom_fptr_type( file );
    fclose( file );
  }
 
  return result;
}

const char* ReadCGM::get_geom_fptr_type( FILE* file )
{
  static const char* CUBIT_NAME = GF_CUBIT_FILE_TYPE;
  static const char*  STEP_NAME = GF_STEP_FILE_TYPE;
  static const char*  IGES_NAME = GF_IGES_FILE_TYPE;
  static const char*   SAT_NAME = GF_ACIS_TXT_FILE_TYPE;
  static const char*   SAB_NAME = GF_ACIS_BIN_FILE_TYPE;
  static const char*  BREP_NAME = GF_OCC_BREP_FILE_TYPE;
 
  if (is_cubit_file(file))
    return CUBIT_NAME;
  else if (is_step_file(file))
    return STEP_NAME;
  else if (is_iges_file(file))
    return IGES_NAME;
  else if (is_acis_bin_file(file))
    return SAB_NAME;
  else if (is_acis_txt_file(file))
    return SAT_NAME;
  else if (is_occ_brep_file(file))
    return BREP_NAME;
  else
    return 0;
}    

int ReadCGM::is_cubit_file( FILE* file )
{
  unsigned char buffer[4];
  return !fseek(file, 0, SEEK_SET) &&
         fread(buffer, 4, 1, file) &&
         !memcmp(buffer, "CUBE", 4);
}

int ReadCGM::is_step_file( FILE* file )
{
  unsigned char buffer[9];
  return !fseek(file, 0, SEEK_SET) &&
         fread(buffer, 9, 1, file) &&
         !memcmp(buffer, "ISO-10303", 9);
}

int ReadCGM::is_iges_file( FILE* file )
{
  unsigned char buffer[10];
  return !fseek(file, 72, SEEK_SET) &&
         fread(buffer, 10, 1, file) &&
         !memcmp(buffer, "S      1", 8);
}

int ReadCGM::is_acis_bin_file( FILE* file )
{
  char buffer[15];
  return !fseek(file, 0, SEEK_SET) &&
         fread(buffer, 15, 1, file) &&
         !memcmp(buffer, "ACIS BinaryFile", 9);
}

int ReadCGM::is_acis_txt_file( FILE* file )
{
  char buffer[5];
  int version, length;
 
  if (fseek(file,0,SEEK_SET) ||
      2 != fscanf( file, "%d %*d %*d %*d %d ", &version, &length ))
    return 0;
   
  if (version < 1 || version >0xFFFF)
    return 0;
 
    // Skip appliation name
  if (fseek(file, length, SEEK_CUR))
    return 0;
   
    // Read length of version string followed by first 5 characters
  if (2 != fscanf(file, "%d %4s", &length, buffer))
    return 0;

  return !strcmp( buffer, "ACIS" );
}

int ReadCGM::is_occ_brep_file( FILE* file )
{
  unsigned char buffer[6];
  return !fseek(file, 0, SEEK_SET) &&
         fread(buffer, 6, 1, file) &&
         !memcmp(buffer, "DBRep_", 6);
}

void ReadCGM::tokenize( const std::string& str,
                        std::vector<std::string>& tokens,
                        const char* delimiters )
{
  std::string::size_type last = str.find_first_not_of( delimiters, 0 );
  std::string::size_type pos  = str.find_first_of( delimiters, last );
  while (std::string::npos != pos && std::string::npos != last) {
    tokens.push_back( str.substr( last, pos - last ) );
    last = str.find_first_not_of( delimiters, pos );
    pos  = str.find_first_of( delimiters, last );
    if(std::string::npos == pos)
      pos = str.size();
  }
}
