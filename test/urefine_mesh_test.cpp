/*This function tests the AHF datastructures on CST meshes*/
#include <iostream>
#include <vector>
#include <algorithm>
#include "moab/Core.hpp"
#include "moab/Range.hpp"
#include "moab/MeshTopoUtil.hpp"
#include "moab/HalfFacetRep.hpp"
#include "../RefineMesh/moab/NestedRefine.hpp"
#include "TestUtil.hpp"

#ifdef USE_MPI
#include "moab/ParallelComm.hpp"
#include "MBParallelConventions.h"
#include "ReadParallel.hpp"
#include "moab/FileOptions.hpp"
#include "MBTagConventions.hpp"
#include "moab_mpi.h"
#endif

using namespace moab;

#define STRINGIFY_(X) #X
#define STRINGIFY(X) STRINGIFY_(X)

#ifdef USE_MPI
std::string read_options;
#endif

int number_tests_successful = 0;
int number_tests_failed = 0;

void handle_error_code(ErrorCode rv, int &number_failed, int &number_successful)
{
  if (rv == MB_SUCCESS) {
#ifdef USE_MPI
      int rank = 0;
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
      if (rank==0)
          std::cout << "Success";
#else
      std::cout << "Success";
#endif
    number_successful++;
  } else {
    std::cout << "Failure";
    number_failed++;
  }
}

ErrorCode test_adjacencies(Core *mb, NestedRefine *nr, int dim, Range verts, Range ents)
{
  Interface* mbImpl = mb;
  MeshTopoUtil mtu(mbImpl);
  ErrorCode error;

  if (dim == 1)
    {
      //1D Queries //
      //IQ1: For every vertex, obtain incident edges
      for (Range::iterator i = verts.begin(); i != verts.end(); ++i) {
          std::vector<EntityHandle> adjents;
          Range mbents, ahfents;
          error = nr->get_adjacencies( *i, 1, adjents);
          CHECK_ERR(error);
          error = mbImpl->get_adjacencies( &*i, 1, 1, false, mbents );
          CHECK_ERR(error);
          CHECK_EQUAL(adjents.size(),mbents.size());
          std::sort(adjents.begin(), adjents.end());
          std::copy(adjents.begin(), adjents.end(), range_inserter(ahfents));
          mbents = subtract(mbents, ahfents);
          CHECK(!mbents.size());
      }

      //NQ1:  For every edge, obtain neighbor edges
      for (Range::iterator i = ents.begin(); i != ents.end(); ++i) {
          std::vector<EntityHandle> adjents;
          Range mbents, ahfents;
          error = nr->get_adjacencies( *i, 1, adjents);
          CHECK_ERR(error);
          error = mtu.get_bridge_adjacencies( *i, 0, 1, mbents);
          CHECK_ERR(error);

          CHECK_EQUAL(adjents.size(), mbents.size());

          std::sort(adjents.begin(), adjents.end());
          std::copy(adjents.begin(), adjents.end(), range_inserter(ahfents));
          mbents = subtract(mbents, ahfents);
          CHECK(!mbents.size());
      }

    }
  else if (dim == 2)
    {

      // IQ21: For every vertex, obtain incident faces
      for (Range::iterator i = verts.begin(); i != verts.end(); ++i) {
          std::vector<EntityHandle> adjents;
          error = nr->get_adjacencies( *i, 2, adjents);
          CHECK_ERR(error);

          Range mbents, ahfents;
          error = mbImpl->get_adjacencies( &*i, 1, 2, false, mbents);
          CHECK_ERR(error);

          CHECK_EQUAL(adjents.size(), mbents.size());

          std::sort(adjents.begin(), adjents.end());
          std::copy(adjents.begin(), adjents.end(), range_inserter(ahfents));
          mbents = subtract(mbents, ahfents);
          CHECK(!mbents.size());
      }

      //NQ2: For every face, obtain neighbor faces
      for (Range::iterator i = ents.begin(); i != ents.end(); ++i) {
          std::vector<EntityHandle> adjents;
          Range mbents, ahfents;
          error = nr->get_adjacencies( *i, 2, adjents);

          CHECK_ERR(error);
          error = mtu.get_bridge_adjacencies( *i, 1, 2, mbents);
          CHECK_ERR(error);

          CHECK_EQUAL(adjents.size(), mbents.size());

          std::sort(adjents.begin(), adjents.end());
          std::copy(adjents.begin(), adjents.end(), range_inserter(ahfents));
          mbents = subtract(mbents, ahfents);
          CHECK(!mbents.size());
      }

    }
  else
    {
      std::vector<EntityHandle> adjents;
      Range mbents, ahfents;

      //IQ 31: For every vertex, obtain incident cells
      for (Range::iterator i = verts.begin(); i != verts.end(); ++i) {
          adjents.clear();
          error = nr->get_adjacencies( *i, 3, adjents);
          CHECK_ERR(error);
          mbents.clear();
          error = mbImpl->get_adjacencies(&*i, 1, 3, false, mbents);
          CHECK_ERR(error);

          CHECK_EQUAL(adjents.size(), mbents.size());

          std::sort(adjents.begin(), adjents.end());
          std::copy(adjents.begin(), adjents.end(), range_inserter(ahfents));
          mbents = subtract(mbents, ahfents);
          CHECK(!mbents.size());
      }

      //NQ3: For every cell, obtain neighbor cells
      for (Range::iterator i = ents.begin(); i != ents.end(); ++i) {
          adjents.clear();
          error = nr->get_adjacencies( *i, 3, adjents);
          CHECK_ERR(error);
          mbents.clear();
          error = mtu.get_bridge_adjacencies( *i, 2, 3, mbents);
          CHECK_ERR(error);

          CHECK_EQUAL(adjents.size(), mbents.size());

          std::sort(adjents.begin(), adjents.end());
          std::copy(adjents.begin(), adjents.end(), range_inserter(ahfents));
          mbents = subtract(mbents, ahfents);
          CHECK(!mbents.size());
      }
    }
return MB_SUCCESS;
}


ErrorCode refine_entities(Core *mb, int *level_degrees, const int num_levels)
{
  ErrorCode error;

  //Get the range of entities in the initial mesh
  //Dimension
  Range edges, faces, cells;
  error = mb->get_entities_by_dimension(0, 1, edges);
  CHECK_ERR(error);
  error = mb->get_entities_by_dimension(0, 2, faces);
  CHECK_ERR(error);
  error = mb->get_entities_by_dimension(0, 3, cells);
  CHECK_ERR(error);

  Range init_ents;
  int dim;
  if (edges.size())
    {
      dim = 1;
      init_ents = edges;
    }
  else if (faces.size())
    {
      dim = 2;
      init_ents = faces;
    }
  else if (cells.size())
    {
      dim = 3;
      init_ents = cells;
    }


  std::cout<<"Creating a hm object"<<std::endl;
  //Now generate the hierarchy
  NestedRefine uref(mb);
  EntityHandle *set = new EntityHandle[num_levels];

  std::cout<<"Starting hierarchy generation"<<std::endl;

  error = uref.generate_mesh_hierarchy(level_degrees, num_levels, set);
  CHECK_ERR(error);

  int factor=1;

  //Get the ranges of entities in each level
  for (int l=0; l<num_levels; l++)
    {
      Range verts, ents;

      error = mb->get_entities_by_type(set[l], MBVERTEX, verts);
      CHECK_ERR(error);

      error = mb->get_entities_by_dimension(set[l], dim, ents);
      CHECK_ERR(error);

      if (verts.empty() || ents.empty())
        std::cout<<"something not right"<<std::endl;

      std::cout<<std::endl;
      std::cout<<"Mesh size for level "<<l<<"  :: nverts = "<<verts.size()<<", nents = "<<ents.size()<<std::endl;

 //     for (Range::iterator v = verts.begin(); v!= verts.end(); v++)
   //     std::cout<<"v["<<(*v-*verts.begin())<<"] = "<<*v<<std::endl;

      //for (Range::iterator e = ents.begin(); e!= ents.end(); e++)
       // std::cout<<"ent["<<(*e-*ents.begin())<<"] = "<<*e<<std::endl;

      for (int d=0; d<dim; d++)
        factor *= level_degrees[l];

     int  expected_nents = factor*init_ents.size();

      assert(expected_nents == (int)ents.size());

      //Check adjacencies
      error = test_adjacencies(mb, &uref, dim, verts, ents);
      CHECK_ERR(error);
    }

  delete [] set;
  return MB_SUCCESS;
}

ErrorCode create_single_entity(Core *mb, EntityType type)
{
  ErrorCode error;
  Interface* mbImpl = mb;
  if (type == MBEDGE)
    {
      const double coords[] = {0.0,0.0,0.0,
                              1.0,0.0,0.0};
      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

      const int conn[] = {0, 1};
      const size_t num_elems = sizeof(conn)/sizeof(conn[0])/2;

      std::cout<<"Specify verts and ents"<<std::endl;

      EntityHandle verts[num_vtx], edges[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      std::cout<<"Created vertices"<<std::endl;

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[2];
          c[0] = verts[conn[0]]; c[1] = verts[conn[1]];

          error = mbImpl->create_element(MBEDGE, c, 2, edges[i]);
          if (error != MB_SUCCESS) return error;
        }

      std::cout<<"Created ents"<<std::endl;

    }
  else if (type == MBTRI)
    {
      const double coords[] = {0,0,0,
                              1,0,0,
                              0,1,0};
      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

      const int conn[] = {0, 1, 2};
      const size_t num_elems = sizeof(conn)/sizeof(int)/3;

      EntityHandle verts[num_vtx], faces[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[3];
          for (int j=0; j<3; j++)
            c[j] = verts[conn[3*i+j]];

          error = mbImpl->create_element(MBTRI, c, 3, faces[i]);
          if (error != MB_SUCCESS) return error;
        }
    }
  else if (type == MBQUAD)
    {
          const double coords[] = {0,0,0,
                                  1,0,0,
                                  1,1,0,
                                  0,1,0};
          const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

          const int conn[] = {0, 1, 2, 3};
          const size_t num_elems = sizeof(conn)/sizeof(int)/3;

          EntityHandle verts[num_vtx], faces[num_elems];
          for (size_t i=0; i< num_vtx; ++i)
            {
              error = mbImpl->create_vertex(coords+3*i, verts[i]);
              if (error != MB_SUCCESS) return error;
            }

          for (size_t i=0; i< num_elems; ++i)
            {
              EntityHandle c[4];
              for (int j=0; j<4; j++)
                c[j] = verts[conn[j]];

              error = mbImpl->create_element(MBQUAD, c, 4, faces[i]);
              if (error != MB_SUCCESS) return error;

            }
    }
  else if (type == MBTET)
    {
      const double coords[] = {0,0,0,
                              1,0,0,
                              0,1,0,
                              0,0,1};
      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

      const int conn[] = {0, 1, 2, 3};
      const size_t num_elems = sizeof(conn)/sizeof(int)/3;

      EntityHandle verts[num_vtx], cells[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[4];
          for (int j=0; j<4; j++)
            c[j] = verts[conn[j]];

          error = mbImpl->create_element(MBTET, c, 4, cells[i]);
          if (error != MB_SUCCESS) return error;
        }
    }
  else if (type == MBPRISM)
    {
      const double coords[] = {0,0,0,
                              1,0,0,
                              0,1,0,
                              0,0,1,
                              1,0,1,
                              0,1,1};
      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

      const int conn[] = {0, 1, 2, 3, 4, 5};
      const size_t num_elems = sizeof(conn)/sizeof(int)/3;

      EntityHandle verts[num_vtx], cells[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[6];
          for (int j=0; j<6; j++)
            c[j] = verts[conn[j]];

          error = mbImpl->create_element(MBPRISM, c, 6, cells[i]);
          if (error != MB_SUCCESS) return error;
        }
    }
  else if (type == MBHEX)
  {
    const double coords[] = {0,0,0,
                            1,0,0,
                            1,1,0,
                            0,1,0,
                            0,0,1,
                            1,0,1,
                            1,1,1,
                            0,1,1};
    const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

    const int conn[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const size_t num_elems = sizeof(conn)/sizeof(int)/3;

    EntityHandle verts[num_vtx], cells[num_elems];
    for (size_t i=0; i< num_vtx; ++i)
      {
        error = mbImpl->create_vertex(coords+3*i, verts[i]);
        if (error != MB_SUCCESS) return error;
      }

    for (size_t i=0; i< num_elems; ++i)
      {
        EntityHandle c[8];
        for (int j=0; j<8; j++)
          c[j] = verts[conn[j]];

        error = mbImpl->create_element(MBHEX, c, 8, cells[i]);
        if (error != MB_SUCCESS) return error;
      }
    }
  return MB_SUCCESS;
}

ErrorCode create_mesh(Core *mb, EntityType type)
{
  ErrorCode error;
  Interface* mbImpl = mb;
  if (type == MBEDGE)
    {
      const double coords[] = {0,0,0,
                              1,0,0,
                              0,1,0,
                              -1,0,0,
                               0,-1,0};
      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

      const int conn[] = {1,0,
                         0,3,
                         2,0,
                         0,4};
      const size_t num_elems = sizeof(conn)/sizeof(int)/2;

      EntityHandle verts[num_vtx], edges[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[2];
          c[0] = verts[conn[2*i]]; c[1] = verts[conn[2*i+1]];

          error = mbImpl->create_element(MBEDGE, c, 2, edges[i]);
          if (error != MB_SUCCESS) return error;
        }

    }
  else if (type == MBTRI)
    {
     const double coords[] = {0,0,0,
                              1,-1,0,
                              1,1,0,
                              -1,1,0,
                              -1,-1,0,
                              0,0,1};

      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

     const int conn[] = {0, 1, 2,
                         0,2,3,
                         0,3,4,
                         0,4,1,
                         0,5,3,
                         0,2,5};

      const size_t num_elems = sizeof(conn)/sizeof(int)/3;

      EntityHandle verts[num_vtx], faces[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[3];
          for (int j=0; j<3; j++)
            c[j] = verts[conn[3*i+j]];

          error = mbImpl->create_element(MBTRI, c, 3, faces[i]);
          if (error != MB_SUCCESS) return error;
        }
    }
  else if (type == MBQUAD)
    {
       /*   const double coords[] = {0,0,0,
                                  1,0,0,
                                  1,1,0,
                                  0,1,0,
                                  -1,1,0,
                                  -1,0,0,
                                  -1,-1,0,
                                  0,-1,0,
                                  1,-1,0,
                                  0,1,1,
                                  0,0,1,
                                  0,-1,1};*/
      const double coords[] = {0,0,0,
                                        1,0,0,
                                        1,1,0,
                                        0,-1,0,
                                        -1,1,0,
                                        -1,0,0,
                                        0,0,1,
                                        0,-1,1};

          const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

         /* const int conn[] = {0, 1, 2, 3,
                             0,3,4,5,
                             7,8,1,0,
                             6,7,0,5,
                             0,3,9,10,
                             0,10,11,7};*/
          const int conn[] = {0, 1, 2, 3,
                              5,0,3,4,
                              0,3,7,6 };
          const size_t num_elems = sizeof(conn)/sizeof(int)/4;

          EntityHandle verts[num_vtx], faces[num_elems];
          for (size_t i=0; i< num_vtx; ++i)
            {
              error = mbImpl->create_vertex(coords+3*i, verts[i]);
              if (error != MB_SUCCESS) return error;
            }

          for (size_t i=0; i< num_elems; ++i)
            {
              EntityHandle c[4];
              for (int j=0; j<4; j++)
                c[j] = verts[conn[4*i+j]];

              error = mbImpl->create_element(MBQUAD, c, 4, faces[i]);
              if (error != MB_SUCCESS) return error;

            }
    }
  else if (type == MBTET)
    {
      const double coords[] = {0,0,0,
                              1,0,0,
                              0,1,0,
                              0,0,1};
      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

      const int conn[] = {0, 1, 2, 3};
      const size_t num_elems = sizeof(conn)/sizeof(int)/3;

      EntityHandle verts[num_vtx], cells[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[4];
          for (int j=0; j<4; j++)
            c[j] = verts[conn[j]];

          error = mbImpl->create_element(MBTET, c, 4, cells[i]);
          if (error != MB_SUCCESS) return error;
        }
    }
  else if (type == MBPRISM)
    {
      const double coords[] = {0,0,0,
                              1,0,0,
                              0,1,0,
                              0,0,1,
                              1,0,1,
                              0,1,1};
      const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

      const int conn[] = {0, 1, 2, 3, 4, 5};
      const size_t num_elems = sizeof(conn)/sizeof(int)/3;

      EntityHandle verts[num_vtx], cells[num_elems];
      for (size_t i=0; i< num_vtx; ++i)
        {
          error = mbImpl->create_vertex(coords+3*i, verts[i]);
          if (error != MB_SUCCESS) return error;
        }

      for (size_t i=0; i< num_elems; ++i)
        {
          EntityHandle c[6];
          for (int j=0; j<6; j++)
            c[j] = verts[conn[j]];

          error = mbImpl->create_element(MBPRISM, c, 6, cells[i]);
          if (error != MB_SUCCESS) return error;
        }
    }
  else if (type == MBHEX)
  {
    const double coords[] = {0,0,0,
                            1,0,0,
                            1,1,0,
                            0,1,0,
                            0,0,1,
                            1,0,1,
                            1,1,1,
                            0,1,1};
    const size_t num_vtx = sizeof(coords)/sizeof(double)/3;

    const int conn[] = {0, 1, 2, 3, 4, 5, 6, 7};
    const size_t num_elems = sizeof(conn)/sizeof(int)/3;

    EntityHandle verts[num_vtx], cells[num_elems];
    for (size_t i=0; i< num_vtx; ++i)
      {
        error = mbImpl->create_vertex(coords+3*i, verts[i]);
        if (error != MB_SUCCESS) return error;
      }

    for (size_t i=0; i< num_elems; ++i)
      {
        EntityHandle c[8];
        for (int j=0; j<8; j++)
          c[j] = verts[conn[j]];

        error = mbImpl->create_element(MBHEX, c, 8, cells[i]);
        if (error != MB_SUCCESS) return error;
      }
    }
  return MB_SUCCESS;
}

ErrorCode test_entities(int mesh_type, EntityType type, int *level_degrees, int num_levels)
{
  ErrorCode error;
  Core mb;

  //Create entities
  if (mesh_type == 1){
      error = create_single_entity(&mb, type);
      if (error != MB_SUCCESS) return error;
      std::cout<<"Entity created successfully"<<std::endl;
    }
  else if (mesh_type == 2)
    {
      error = create_mesh(&mb, type);
      if (error != MB_SUCCESS) return error;
      std::cout<<"Small mesh created successfully"<<std::endl;
    }

  //Generate hierarchy
  error = refine_entities(&mb, level_degrees, num_levels);
  if (error != MB_SUCCESS) return error;

  return MB_SUCCESS;

}
ErrorCode test_1D()
{
  ErrorCode error;

  std::cout<<"Testing EDGE"<<std::endl;
  EntityType type = MBEDGE;

  std::cout<<"Testing single entity"<<std::endl;
  int deg[3] = {2,3,5};
  int len = sizeof(deg) / sizeof(int);
  error = test_entities(1, type, deg, len);
  CHECK_ERR(error);

  std::cout<<std::endl;
  std::cout<<"Testing a small mesh"<<std::endl;
  error = test_entities(2, type, deg, len);
  CHECK_ERR(error);

  return MB_SUCCESS;
}

ErrorCode test_2D()
{
  ErrorCode error;

 std::cout<<"Testing TRI"<<std::endl;
  EntityType type = MBTRI;

  std::cout<<"Testing single entity"<<std::endl;
  int deg[3] = {2,3,5};
  int len = sizeof(deg) / sizeof(int);
  error = test_entities(1, type, deg, len);
  CHECK_ERR(error);

  std::cout<<std::endl;
  std::cout<<"Testing a small mesh"<<std::endl;
  error = test_entities(2, type, deg, len);
  CHECK_ERR(error);

  std::cout<<std::endl;
  std::cout<<"Testing QUAD"<<std::endl;
  type = MBQUAD;

  std::cout<<"Testing single entity"<<std::endl;
  error = test_entities(1, type, deg, len);
  CHECK_ERR(error);

  std::cout<<std::endl;
  std::cout<<"Testing a small mesh"<<std::endl;
  error = test_entities(2, type, deg, len);
  CHECK_ERR(error);

  return MB_SUCCESS;
}

ErrorCode test_3D()
{
  ErrorCode error;

  std::cout<<"Testing TET"<<std::endl;
  EntityType type = MBTET;

  int deg = 3;
  error = test_entities(1, type, &deg, 1);
  CHECK_ERR(error);

  return MB_SUCCESS;
}



ErrorCode test_mesh(const char* filename)
{
  Core moab;
  Interface* mbImpl = &moab;
  ErrorCode error;

#ifdef USE_MPI
    int procs = 1;
    MPI_Comm_size(MPI_COMM_WORLD, &procs);

    if (procs > 1){
    read_options = "PARALLEL=READ_PART;PARTITION=PARALLEL_PARTITION;PARALLEL_RESOLVE_SHARED_ENTS;";

    error = mbImpl->load_file(filename, 0, read_options.c_str());
    CHECK_ERR(error);
    }
    else if (procs == 1) {
#endif
    error = mbImpl->load_file(filename);
    CHECK_ERR(error);
#ifdef USE_MPI
    }
#endif

    //Generate hierarchy
    int deg = 2;
    error = refine_entities(&moab, &deg, 1);
    if (error != MB_SUCCESS) return error;

    return MB_SUCCESS;
}

int main(int argc, char *argv[])
{
#ifdef USE_MPI
    MPI_Init(&argc, &argv);

    int nprocs, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif


  ErrorCode result;

  result = test_1D();
  handle_error_code(result, number_tests_failed, number_tests_successful);
  std::cout<<"\n";

  result = test_2D();
  handle_error_code(result, number_tests_failed, number_tests_successful);
  std::cout<<"\n";

#ifdef USE_MPI
    MPI_Finalize();
#endif

  return number_tests_failed;
}

