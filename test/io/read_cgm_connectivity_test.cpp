
#include <iostream>
#include "moab/Interface.hpp"
#ifndef IS_BUILDING_MB
#define IS_BUILDING_MB
#endif
#include "TestUtil.hpp"
#include "Internals.hpp"
#include "moab/Core.hpp"
#include "MBTagConventions.hpp"
#include "InitCGMA.hpp"
#include "GeometryQueryTool.hpp"
#include "moab/MeshTopoUtil.hpp"
using namespace moab;

#define CHKERR(A) do { if (MB_SUCCESS != (A)) { \
  std::cerr << "Failure (error code " << (A) << ") at " __FILE__ ":" \
            << __LINE__ << std::endl; \
  return A; } } while(false)


#ifdef MESHDIR
static const char input_cube[] = STRINGIFY(MESHDIR) "/io/cube.sat";
#else
static const char input_cube[] = "/io/cube.sat";
#endif

// Function used to load the test file
void read_file( Interface* moab, const char* input_file );

// List of tests in this file
void cube_verts_connectivity_test();
void cube_tris_connectivity_test();
void cube_tri_curve_coincidence_test();
void cube_edge_adjacencies_test();
void cube_tri_vertex_test();

//Other functions
ErrorCode match_tri_edges_w_curve( Interface* moab, Range tri_edges, Range curves);

int main(int /* argc */, char** /* argv */)
{
  int result = 0;
 
  result += RUN_TEST(cube_verts_connectivity_test);
  result += RUN_TEST(cube_tris_connectivity_test);
  result += RUN_TEST(cube_tri_curve_coincidence_test);
  result += RUN_TEST(cube_tri_curve_coincidence_test);
  result += RUN_TEST(cube_tri_vertex_test);
 
  return result;
}



void read_file( Interface* moab, const char* input_file )
{
  InitCGMA::initialize_cgma();
  GeometryQueryTool::instance()->delete_geometry();

  ErrorCode rval = moab->load_file( input_file );
  CHECK_ERR(rval);
}

void cube_verts_connectivity_test()
{

  ErrorCode rval;
  //Open the test file
  Core moab;
  Interface* mb = &moab;
  read_file( mb, input_cube);

  //Get all vertex handles from the mesh
  Range verts;
  rval = mb->get_entities_by_type( 0, MBVERTEX, verts);
  CHECK_ERR(rval);

  //Check that each vertex connects to less than 4 triangles and no more than 6

  for(Range::const_iterator i = verts.begin(); i!=verts.end(); i++)
    {
      std::vector<EntityHandle> adj_tris;
      rval = mb->get_adjacencies( &(*i), 1, 2, false, adj_tris );
      CHECK_ERR(rval);

      int adj_size = adj_tris.size();
      CHECK( adj_size >= 4 && adj_size <= 6);
    }
    
}

void cube_tris_connectivity_test()
{
  ErrorCode rval;
  //Open the test file
  Core moab;
  Interface* mb = &moab;
  read_file( mb, input_cube);

  //Get triangles from the mesh
  Range tris;
  rval = mb->get_entities_by_type( 0, MBTRI, tris);
  CHECK_ERR(rval);


  for(Range::const_iterator i = tris.begin()+1; i!=tris.end(); i++)
    {
      Range adj_tris;
      moab::MeshTopoUtil mu(mb);
      //Use Triangle edges to get all adjacent triangles
      rval = mu.get_bridge_adjacencies( *i, 1, 2, adj_tris);
      CHECK_ERR(rval);
      int number_of_adj_tris=adj_tris.size();      
      CHECK_EQUAL( 3, number_of_adj_tris);
      
      //Check that the entities we found from bridge_adjacencies
      //are triangles
      Range adj_tri_test = adj_tris.subset_by_type(MBTRI);
      int number_tris_in_adj_tris = adj_tri_test.size();
      CHECK_EQUAL( number_of_adj_tris, number_tris_in_adj_tris);
    
    }

}

void cube_tri_curve_coincidence_test()
{

  ErrorCode rval;
  //Open the test file
  Core moab;
  Interface* mb = &moab;
  read_file( mb, input_cube);

  //Get curves from the mesh
  Range curves;
  rval = mb->get_entities_by_type( 0, MBEDGE, curves);
  CHECK_ERR(rval);

  //Get triangles from the mesh
  Range tris;
  rval = mb->get_entities_by_type( 0, MBTRI, tris);
  CHECK_ERR(rval);

  for(Range::const_iterator i=tris.begin(); i!=tris.end(); i++)
    {
      //Get the any curve edges that are a part of the triangle
      Range tri_edges;
      rval = mb->get_adjacencies( &(*i), 1, 1, false, tri_edges);
      CHECK_ERR(rval);

      int num_of_tri_edges = tri_edges.size();
      CHECK_EQUAL(2, num_of_tri_edges);
      rval = match_tri_edges_w_curve( mb, tri_edges, curves);
      CHECK_ERR(rval);
  
    }

}

ErrorCode match_tri_edges_w_curve( Interface* moab, Range tri_edges, Range curves)
{

  ErrorCode rval;
  int match_counter=0;
  for(Range::const_iterator i=tri_edges.begin(); i!=tri_edges.end(); i++)
    {
      for(Range::const_iterator j=curves.begin(); j!=curves.end(); j++)
	{
          if( *i  == *j  ) match_counter++;
	}
    }

  int num_of_tri_edges = tri_edges.size();
  CHECK_EQUAL( num_of_tri_edges, match_counter );
  return MB_SUCCESS;
} 

void cube_edge_adjacencies_test()
{
  ErrorCode rval;
  //Open the test file
  Core moab;
  Interface* mb = &moab;
  read_file( mb, input_cube);

  //Get the curves 
  Range curves;
  rval = mb->get_entities_by_type(0, MBEDGE, curves);
  CHECK_ERR(rval);

  for(Range::const_iterator i=curves.begin(); i!=curves.end(); i++)
    {
      //Get triangle adjacent to each edge
      Range adj_tris;
      rval = mb->get_adjacencies( &(*i), 1, 2, false, adj_tris);
      CHECK_ERR(rval);
      
      int num_adj_tris = adj_tris.size();
      //Ensure that an edge isn't adjacent to more than two triangles
      CHECK( num_adj_tris <= 2);
    }

}

void cube_tri_vertex_test()
{
  ErrorCode rval;
  //Open the test file
  Core moab;
  Interface* mb = &moab;
  read_file( mb, input_cube);
 
  //Get all triangles
  Range tris;
  rval = mb->get_entities_by_type( 0, MBTRI, tris);
  CHECK_ERR(rval);

  for(Range::const_iterator i=tris.begin(); i!=tris.end(); i++)
    {
      //Get all triangle vertices
      Range verts;
      rval = mb->get_connectivity( &(*i), 1, verts);
      CHECK_ERR(rval);

      int number_of_verts = verts.size();
      CHECK( 3 == number_of_verts);
      CHECK(verts[0]!=verts[1]);
      CHECK(verts[1]!=verts[2]);      
      CHECK(verts[2]!=verts[0]);
    } 

}
