/**
 * MOAB, a Mesh-Oriented datABase, is a software component for creating,
 * storing and accessing finite element mesh data.
 * 
 * Copyright 2008 Sandia Corporation.  Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Coroporation, the U.S. Government
 * retains certain rights in this software.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 */

#include "SweptElementSeq.hpp"
#include "SweptVertexData.hpp"
#include "SweptElementData.hpp"
#include "MBInterface.hpp"
#include "MBReadUtilIface.hpp"
#include "MBCN.hpp"
#include "MBInternals.hpp"

SweptElementSeq::SweptElementSeq(MBEntityHandle start_handle,
                                 const int imin, const int jmin, const int kmin,
				 const int imax, const int jmax, const int kmax,
                                 const int* Cq ) 
  : ElementSequence( start_handle, 
                     ScdElementData::calc_num_entities( start_handle,
                                                        imax-imin,
                                                        jmax-jmin,
                                                        kmax-kmin ),
                     MBCN::VerticesPerEntity(TYPE_FROM_HANDLE(start_handle)),
                     new SweptElementData( start_handle, 
					   imin, jmin, kmin,
					   imax, jmax, kmax,
					   Cq ) )
{
}

SweptElementSeq::~SweptElementSeq() 
{
}

MBErrorCode SweptElementSeq::get_connectivity( 
                                        MBEntityHandle handle,
                                        std::vector<MBEntityHandle>& connect,
                                        bool /*topological*/ ) const
{
  int i, j, k;
  MBErrorCode rval = get_params( handle, i, j, k );
  if (MB_SUCCESS == rval)
    rval = get_params_connectivity( i, j, k, connect );
  return rval;
}

MBErrorCode SweptElementSeq::get_connectivity( 
                                        MBEntityHandle handle,
                                        MBEntityHandle const*& connect,
                                        int &connect_length,
                                        bool topo,
                                        std::vector<MBEntityHandle>* storage
                                        ) const
{
  if (!storage) {
    connect = 0;
    connect_length = 0;
    return MB_NOT_IMPLEMENTED;
  }
  
  storage->clear();
  MBErrorCode rval = get_connectivity( handle, *storage, topo );
  connect = &(*storage)[0];
  connect_length = storage->size();
  return rval;
}

MBErrorCode SweptElementSeq::set_connectivity( 
                                        MBEntityHandle,
                                        MBEntityHandle const*,
                                        int )
{
  return MB_NOT_IMPLEMENTED;
}

MBEntityHandle* SweptElementSeq::get_connectivity_array()
  { return 0; }

int SweptElementSeq::values_per_entity() const
  { return -1; } // never reuse freed handles for swept elements 

EntitySequence* SweptElementSeq::split( MBEntityHandle here )
  { return new SweptElementSeq( *this, here ); }

SequenceData* SweptElementSeq::create_data_subset( MBEntityHandle, MBEntityHandle ) const
  { return 0; }

void SweptElementSeq::get_const_memory_use( unsigned long& bytes_per_entity,
                                                 unsigned long& sequence_size ) const
{
  sequence_size = sizeof(*this);
//  bytes_per_entity = sdata()->get_memory_use() / sdata()->size();
}