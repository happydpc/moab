#ifndef MOAB_PARAMETRIZER_HPP
#define MOAB_PARAMETRIZER_HPP
#include "moab/Matrix3.hpp"
#include "moab/CartVect.hpp"
#include "moab/point_locater/linear_hex_map.hpp"
#include "moab/point_locater/linear_tet_map.hpp"
namespace moab { 

namespace element_utility {
//non-exported functionality
namespace { 

template< typename Moab, 
	  typename Entity_handle, typename Points>
void get_moab_points( Moab & moab, 
		      Entity_handle eh, 
		      Points & points){
	const Entity_handle* connectivity_begin;
	int num_vertices;
	moab.get_connectivity( eh, connectivity_begin, num_vertices);
	//TODO: This is hacky, it works correctly since
	//CartVect is only double d[ 3], with a default
	//constructor.. get_coords() should be
	//flexible enough to allow other types..
	points.resize( num_vertices);
	moab.get_coords( connectivity_begin, num_vertices, &(points[ 0][ 0]));
}

} // non-exported functionality

template< typename Element_map> 
class Element_parametrizer{
	public:
		//public types
		typedef Element_map Map;
	private: 
		typedef Element_parametrizer< Map> Self;
	public: //public functionality
	Element_parametrizer(): map(){}
 	Element_parametrizer( const Self & f): map( f.map) {}
	public:
		template< typename Moab, typename Entity_handle, typename Point>
		std::pair< bool, Point> operator()( Moab & moab,
						    const Entity_handle & eh, 
						    const Point & point, 
						    const double tol){
			typedef std::vector< moab::CartVect> Points;
			Points points;
			get_moab_points( moab, eh, points);
			return map( eh, points, point, tol);			
		}
	private: 
	Element_map map;
}; //class Element_parametrizer

class Parametrizer{
	private: 
		typedef Parametrizer Self;
		typedef moab::EntityHandle Entity_handle;
	public: //public functionality
	Parametrizer(): hex_map(), tet_map(){}
 	Parametrizer( const Self & f): hex_map( f.hex_map), 
				       tet_map( f.tet_map) {}
	public:
		template< typename Moab, typename Entity_handle, typename Point>
		std::pair< bool, Point> operator()( Moab & moab,
						    const Entity_handle & eh, 
						    const Point & point){
			//get entity
			typedef std::vector< moab::CartVect> Points;
		        Points points;
			get_moab_points( moab, eh, points);
			//get type
			switch( moab.type_from_handle( eh)){
 				case moab::MBHEX:
					return hex_map( eh, points, point);
				case moab::MBTET:
					return tet_map( eh, points, point);
				//case moab::SPECHEX:
					//return spectral_hex_map( eh, entity, 
					//			   point, tol);
				default:
				   std::cerr << "Element type not supported" 
					     << std::endl;
				  return make_pair( false, Point(3, 0.0));
			}
		}
	private: 
	Linear_hex_map< moab::Matrix3> hex_map;
	Linear_tet_map< Entity_handle, moab::Matrix3> tet_map;
	//Spec_hex_map tet_map< moab::Matrix3> spec_hex_map;
}; //class Parametrizer

}// namespace element_utility
} // namespace moab
#endif //MOAB_PARAMETRIZER_HPP
