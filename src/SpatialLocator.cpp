#include "moab/SpatialLocator.hpp"
#include "moab/Interface.hpp"
#include "moab/ElemEvaluator.hpp"
#include "moab/AdaptiveKDTree.hpp"

namespace moab 
{

    SpatialLocator::SpatialLocator(Interface *impl, Range &elems, Tree *tree, ElemEvaluator *eval) 
            : mbImpl(impl), myElems(elems), myDim(-1), myTree(tree), elemEval(eval), iCreatedTree(false)
    {
      if (!myTree) {
        myTree = new AdaptiveKDTree(impl);
        iCreatedTree = true;
      }
      if (!elems.empty()) {
        myDim = mbImpl->dimension_from_handle(*elems.rbegin());
        ErrorCode rval = myTree->build_tree(myElems);
        if (MB_SUCCESS != rval) throw rval;
      }
    }

    ErrorCode SpatialLocator::add_elems(Range &elems) 
    {
      if (elems.empty() ||
          mbImpl->dimension_from_handle(*elems.begin()) != mbImpl->dimension_from_handle(*elems.rbegin()))
        return MB_FAILURE;
  
      myDim = mbImpl->dimension_from_handle(*elems.begin());
      myElems = elems;
      return MB_SUCCESS;
    }
    
#ifdef USE_MPI
    ErrorCode SpatialLocator::par_locate_points(Range &/*vertices*/,
                                                double /*rel_tol*/, double /*abs_tol*/) 
    {
      return MB_UNSUPPORTED_OPERATION;
    }

    ErrorCode SpatialLocator::par_locate_points(const double */*pos*/, int /*num_points*/,
                                                double /*rel_tol*/, double /*abs_tol*/) 
    {
      return MB_UNSUPPORTED_OPERATION;
    }
#endif
      
    ErrorCode SpatialLocator::locate_points(Range &verts,
                                            double rel_eps, double abs_eps) 
    {
      assert(!verts.empty() && mbImpl->type_from_handle(*verts.rbegin()) == MBVERTEX);
      std::vector<double> pos(3*verts.size());
      ErrorCode rval = mbImpl->get_coords(verts, &pos[0]);
      if (MB_SUCCESS != rval) return rval;
      rval = locate_points(&pos[0], verts.size(), rel_eps, abs_eps);
      if (MB_SUCCESS != rval) return rval;
      
      return MB_SUCCESS;
    }
    
    ErrorCode SpatialLocator::locate_points(const double *pos, int num_points,
                                            double rel_eps, double abs_eps) 
    {
        // initialize to tuple structure (p_ui, hs_ul, r[3]_d) (see header comments for locTable)
      locTable.initialize(1, 0, 1, 3, num_points);
      locTable.enableWriteAccess();

        // pass storage directly into locate_points, since we know those arrays are contiguous
      ErrorCode rval = locate_points(pos, num_points, locTable.vul_wr, locTable.vr_wr, NULL, rel_eps, abs_eps);
      std::fill(locTable.vi_wr, locTable.vi_wr+num_points, 0);
      if (MB_SUCCESS != rval) return rval;
      
      return MB_SUCCESS;
    }
      
    ErrorCode SpatialLocator::locate_points(Range &verts,
                                            EntityHandle *ents, double *params, bool *is_inside,
                                            double rel_eps, double abs_eps)
    {
      assert(!verts.empty() && mbImpl->type_from_handle(*verts.rbegin()) == MBVERTEX);
      std::vector<double> pos(3*verts.size());
      ErrorCode rval = mbImpl->get_coords(verts, &pos[0]);
      if (MB_SUCCESS != rval) return rval;
      return locate_points(&pos[0], verts.size(), ents, params, is_inside, rel_eps, abs_eps);
    }

    ErrorCode SpatialLocator::locate_points(const double *pos, int num_points,
                                            EntityHandle *ents, double *params, bool *is_inside,
                                            double rel_eps, double abs_eps)
    {

      if (rel_eps && !abs_eps) {
          // relative epsilon given, translate to absolute epsilon using box dimensions
        BoundBox box;
        myTree->get_bounding_box(box);
        abs_eps = rel_eps * box.diagonal_length();
      }
  
      EntityHandle closest_leaf;
      std::vector<double> dists;
      std::vector<EntityHandle> leaves;
      ErrorCode rval = MB_SUCCESS;

      for (int i = 0; i < num_points; i++) {
        int i3 = 3*i;
        ents[i] = 0;
        if (abs_eps) {
          rval = myTree->distance_search(pos+i3, abs_eps, leaves, abs_eps, &dists);
          if (MB_SUCCESS != rval) return rval;
          if (!leaves.empty()) {
              // get closest leaf
            double min_dist = *dists.begin();
            closest_leaf = *leaves.begin();
            std::vector<EntityHandle>::iterator vit = leaves.begin()+1;
            std::vector<double>::iterator dit = dists.begin()+1;
            for (; vit != leaves.end() && min_dist; vit++, dit++) {
              if (*dit < min_dist) {
                min_dist = *dit;
                closest_leaf = *vit;
              }
            }
            dists.clear();
            leaves.clear();
          }
        }
        else {
          rval = myTree->point_search(pos+i3, closest_leaf);
          if (MB_ENTITY_NOT_FOUND == rval) closest_leaf = 0;
          else if (MB_SUCCESS != rval) return rval;
        }

          // if no ElemEvaluator, just return the box
        if (!elemEval) {
          ents[i] = closest_leaf;
          params[i3] = params[i3+1] = params[i3+2] = -2;
          if (is_inside && closest_leaf) is_inside[i] = true;
          continue;
        }
    
          // find natural coordinates of point in element(s) in that leaf
        CartVect tmp_nat_coords; 
        Range range_leaf;
        rval = mbImpl->get_entities_by_dimension(closest_leaf, myDim, range_leaf, false);
        if(rval != MB_SUCCESS) return rval;

          // loop over the range_leaf
        bool tmp_inside;
        for(Range::iterator rit = range_leaf.begin(); rit != range_leaf.end(); rit++)
        {
          bool *is_ptr = (is_inside ? is_inside+i : &tmp_inside);      
          rval = elemEval->set_ent_handle(*rit); 
          if (MB_SUCCESS != rval) return rval;
          rval = elemEval->reverse_eval(pos+i3, abs_eps, params+i3, is_ptr);
          if (MB_SUCCESS != rval) return rval;
          if (*is_ptr) {
            ents[i] = *rit;
            break;
          }
        }
      }

      return MB_SUCCESS;
    }
    
} // namespace moab

