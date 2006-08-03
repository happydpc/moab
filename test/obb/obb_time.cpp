#include "MBCore.hpp"
#include "MBCartVect.hpp"
#include "MBOrientedBox.hpp"
#include "MBOrientedBoxTreeTool.hpp"
#include <stdlib.h>
#include <iostream>
#include <cmath>
#include <time.h>
#include <signal.h>
#include <assert.h>

const int NUM_RAYS = 20000;
const int NUM_XSCT = 10000;

static void usage( )
{
  std::cerr << "obb_time [-r <int>] [-i <int>] <filename>" << std::endl
      << "  -r - Specify total rays to fire." << std::endl
      << "       Zero implies unbounded. Default: " << NUM_RAYS << std::endl
      << "  -i - Specify total intersecting rays to fire." << std::endl
      << "       Zero implies unbounded. Default: " << NUM_XSCT << std::endl
      << "  The input file should be generated using the '-s'" << std::endl
      << "  option with 'obb_test'" << std::endl;
  exit(1);
}

void generate_ray( const MBCartVect& sphere_center,
                   double sphere_radius,
                   MBCartVect& point,
                   MBCartVect& dir )
{
  const int H = RAND_MAX/2;
  point[0] = -(double)rand() ;// - H;
  point[1] = -(double)rand() ;// - H;
  point[2] = -(double)rand() ;// - H;
  point *= sphere_radius / (1.7320508075688772 * RAND_MAX);
  point += sphere_center;
  
  dir[0] = (double)rand() ;// - H;
  dir[1] = (double)rand() ;// - H;
  dir[2] = (double)rand() ;// - H;
  dir.normalize();
}

MBErrorCode read_tree( MBInterface* instance,
                       const char* filename,
                       MBEntityHandle& tree_root_out )
{
  MBErrorCode rval = instance->load_mesh( filename );
  if (MB_SUCCESS != rval)
    return rval;
  
  MBTag tag;
  rval = instance->tag_get_handle( "OBB_ROOT", tag );
  if (MB_SUCCESS != rval)
    return rval;
  
  int size;
  MBDataType type;
  rval = instance->tag_get_size( tag, size );
  if (MB_SUCCESS != rval)
    return rval;
  rval = instance->tag_get_data_type( tag, type );
  if (MB_SUCCESS != rval)
    return rval;

  if (size != sizeof(MBEntityHandle) || type != MB_TYPE_HANDLE)
    return MB_FAILURE;
  
  return instance->tag_get_data( tag, 0, 0, &tree_root_out );
}

// global variables for CLI options
int num_rays = NUM_RAYS;
int num_xsct = NUM_XSCT;
const char* filename = 0;

// global to make accessable to signal handler
int rays = 0, xsct = 0, gen = 0;
clock_t t;

extern "C" {
  void signal_handler( int ) {
    t = clock() - t;
    std::cout << filename << ":" << std::endl
              << rays << " of " << num_rays << " ray fires done" << std::endl
              << xsct << " of " << num_xsct << " intersecting fires" << std::endl
              << gen  << " unique rays used" << std::endl
              << (double)t/CLOCKS_PER_SEC << " seconds" << std::endl;
    exit(1);
  }
}

int main( int argc, char* argv[] )
{
  signal( SIGINT, &signal_handler );
  
  for (int i = 1; i < argc; ++i)
  {
    if (!strcmp( argv[i], "-r")) {
      ++i;
      if (i == argc || !argv[i][0]) {
        std::cerr << "Expected value following '-r'" << std::endl;
        usage();
      }
      char* end;
      long t = strtol( argv[i], &end, 0 );
      num_rays = (int)t;
      if (*end || t < 0 || num_rays != t) {
        std::cerr << "Expected positive integer following '-r'" << std::endl;
        usage();
      }
    }
    else if (!strcmp( argv[i], "-i")) {
      ++i;
      if (i == argc || !argv[i][0]) {
        std::cerr << "Expected value following '-i'" << std::endl;
        usage();
      }
      char* end;
      long t = strtol( argv[i], &end, 0 );
      num_xsct = (int)t;
      if (*end || t < 0 || num_xsct != t) {
        std::cerr << "Expected positive integer following '-i'" << std::endl;
        usage();
      }
    }
    else if (filename) {
      std::cerr << "Invalid options or multiple file names specified." << std::endl;
        usage();
    }
    else {
      filename = argv[i];
    }
  }
  if (!filename) {
    std::cerr << "No file name specified." << std::endl;
    usage();
  }
    
  MBCore instance;
  MBInterface* iface = &instance;
  MBEntityHandle root;
  MBErrorCode rval = read_tree( iface, filename, root );
  if (MB_SUCCESS != rval) {
    std::cerr << "Failed to read \"" <<filename<<'"'<<std::endl;
    return 2;
  }
  
  MBOrientedBoxTreeTool tool(iface);
  MBOrientedBox box;
  rval = tool.box( root, box );
  if (MB_SUCCESS != rval) {
    std::cerr << "Corrupt tree.  Cannot get box for root node." << std::endl;
    return 3;
  }
  
  const unsigned cached = 1000;
  std::vector<double> intersections;
  MBCartVect point, dir;
  std::vector<MBCartVect> randrays;
  randrays.reserve( cached );
  int cached_idx = 0;
  
  t = clock();
  for (;;) {
    if (!num_rays) {
      if (xsct >= num_xsct)
        break;
    }
    else if (!num_xsct) {
      if (rays >= num_rays)
        break;
    }
    else if (rays >= num_rays && xsct >= num_xsct)
      break;
    
    ++rays;
    MBCartVect point, dir;
    if (randrays.size() < cached) {
      generate_ray( box.center, box.outer_radius(), point, dir );
      ++gen;
    }
    else {
      point = randrays[cached_idx++];
      dir   = randrays[cached_idx++];
      cached_idx = cached_idx % randrays.size();
    }
    
    intersections.clear();
    rval = tool.ray_intersect_triangles( intersections, root, 1e-6, point.array(), dir.array() );
    if (MB_SUCCESS != rval) {
      std::cerr << "Rayfire #" << rays << " failed." << std::endl;
      return 4;
    }
    
    if (!intersections.empty()) {
      ++xsct;
    }
    
    if (randrays.size() < cached && 
        (!intersections.empty() || !num_xsct || xsct >= num_xsct)) {
      randrays.push_back( point );
      randrays.push_back( dir );
    }
  }

  t = clock() - t;
  std::cout << rays << " ray fires done" << std::endl
            << gen  << " unique rays used" << std::endl
            << xsct << " intersecting fires" << std::endl
            << (double)t/CLOCKS_PER_SEC << " seconds" << std::endl;
  
  return 0;
}




  

