/****************************************************************************
 * Copyright (c) 2018-2020 by the Cabana authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the Cabana library. Cabana is distributed under a   *
 * BSD 3-clause license. For the licensing terms see the LICENSE file in    *
 * the top-level directory.                                                 *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef CABANA_EXPERIMENTAL_NEIGHBOR_LIST_HPP
#define CABANA_EXPERIMENTAL_NEIGHBOR_LIST_HPP

#include <Cabana_NeighborList.hpp>
#include <Cabana_Slice.hpp>

#include <ArborX.hpp>

#include <Kokkos_Core.hpp>

#include <cassert>

namespace Cabana
{
namespace Experimental
{
namespace stdcxx20
{
template <class T>
struct remove_cvref
{
    using type = std::remove_cv_t<std::remove_reference_t<T>>;
};

template <class T>
using remove_cvref_t = typename remove_cvref<T>::type;
} // namespace stdcxx20

namespace Impl
{
template <typename Slice,
          typename = std::enable_if_t<Cabana::is_slice<Slice>::value>>
struct SubsliceAndRadius
{
    using slice_type = Slice;
    using memory_space = typename Slice::memory_space;
    Slice slice;
    using size_type = typename Slice::size_type;
    size_type first;
    size_type last;
    using value_type = typename Slice::value_type;
    value_type radius;
};

template <typename Slice, typename = std::enable_if_t<Cabana::is_slice<
                              std::remove_reference_t<Slice>>::value>>
auto makePredicates(
    Slice &&slice, typename stdcxx20::remove_cvref_t<Slice>::size_type first,
    typename stdcxx20::remove_cvref_t<Slice>::size_type last,
    typename stdcxx20::remove_cvref_t<Slice>::value_type radius )
{
    return Impl::SubsliceAndRadius<stdcxx20::remove_cvref_t<Slice>>{
        std::forward<Slice>( slice ), first, last, radius};
}
} // namespace Impl
} // namespace Experimental
} // namespace Cabana

namespace ArborX
{
namespace Traits
{
template <typename Slice>
struct Access<Slice, PrimitivesTag,
              std::enable_if_t<Cabana::is_slice<Slice>::value>>
{
    using memory_space = typename Slice::memory_space;
    using size_type = typename Slice::size_type;
    static size_type size( Slice const &x ) { return x.size(); }
    static KOKKOS_FUNCTION Point get( Slice const &x, size_type i )
    {
        return {static_cast<float>( x( i, 0 ) ),
                static_cast<float>( x( i, 1 ) ),
                static_cast<float>( x( i, 2 ) )};
    }
};
template <typename SliceLike>
struct Access<SliceLike, PredicatesTag>
{
    using memory_space = typename SliceLike::memory_space;
    using size_type = typename SliceLike::size_type;
    static KOKKOS_FUNCTION size_type size( SliceLike const &x )
    {
        return x.last - x.first;
    }
    static KOKKOS_FUNCTION auto get( SliceLike const &x, size_type i )
    {
        assert( i < size( x ) );
        auto const point =
            Access<typename SliceLike::slice_type, PrimitivesTag>::get(
                x.slice, x.first + i );
        return attach( intersects( Sphere{point, x.radius} ), (int)i );
    }
};

} // namespace Traits
} // namespace ArborX

namespace Cabana
{
namespace Experimental
{
namespace Impl
{

// Custom callback for ArborX::BVH::query()
template <typename Tag>
struct NeighborDiscriminatorCallback;

template <>
struct NeighborDiscriminatorCallback<Cabana::FullNeighborTag>
{
    using tag = ArborX::Details::InlineCallbackTag;
    template <typename Predicate, typename OutputFunctor>
    KOKKOS_FUNCTION void operator()( Predicate const &predicate, int i,
                                     OutputFunctor const &out ) const
    {
        if ( getData( predicate ) != i ) // discard self-collision
        {
            out( i );
        }
    }
};

template <>
struct NeighborDiscriminatorCallback<Cabana::HalfNeighborTag>
{
    using tag = ArborX::Details::InlineCallbackTag;
    template <typename Predicate, typename OutputFunctor>
    KOKKOS_FUNCTION void operator()( Predicate const &predicate, int i,
                                     OutputFunctor const &out ) const
    {
        if ( getData( predicate ) > i )
        {
            out( i );
        }
    }
};

} // namespace Impl

template <typename MemorySpace, typename Tag>
struct CrsGraph
{
    Kokkos::View<int *, MemorySpace> col_ind;
    Kokkos::View<int *, MemorySpace> row_ptr;
    typename MemorySpace::size_type shift;
    typename MemorySpace::size_type total;
};

template <typename DeviceType, typename Slice, typename Tag>
auto makeNeighborList( Tag, Slice const &coordinate_slice,
                       typename Slice::size_type first,
                       typename Slice::size_type last,
                       typename Slice::value_type radius )
{
    using MemorySpace = typename DeviceType::memory_space;
    using ExecutionSpace = typename DeviceType::execution_space;
    ExecutionSpace space{};

    ArborX::BVH<MemorySpace> bvh( space, coordinate_slice );

    Kokkos::View<int *, DeviceType> indices(
        Kokkos::view_alloc( "indices", Kokkos::WithoutInitializing ), 0 );
    Kokkos::View<int *, DeviceType> offset(
        Kokkos::view_alloc( "offset", Kokkos::WithoutInitializing ), 0 );
    bvh.query( space,
               Impl::makePredicates( coordinate_slice, first, last, radius ),
               Impl::NeighborDiscriminatorCallback<Tag>{}, indices, offset );

    return CrsGraph<typename DeviceType::memory_space, Tag>{
        std::move( indices ), std::move( offset ), first, bvh.size()};
}

} // namespace Experimental

template <typename MemorySpace, typename Tag>
class NeighborList<Experimental::CrsGraph<MemorySpace, Tag>>
{
    using size_type = std::size_t;
    using crs_graph_type = Experimental::CrsGraph<MemorySpace, Tag>;

  public:
    using memory_space = MemorySpace;
    static KOKKOS_FUNCTION size_type
    numNeighbor( crs_graph_type const &crs_graph, size_type p )
    {
        assert( (int)p >= 0 && p < crs_graph.total );
        p -= crs_graph.shift;
        if ( (int)p < 0 || p >= crs_graph.row_ptr.size() - 1 )
            return 0;
        return crs_graph.row_ptr( p + 1 ) - crs_graph.row_ptr( p );
    }
    static KOKKOS_FUNCTION size_type
    getNeighbor( crs_graph_type const &crs_graph, size_type p, size_type n )
    {
        assert( (int)p >= 0 && p < crs_graph.total );
        assert( n < numNeighbor( crs_graph, p ) );
        p -= crs_graph.shift;
        return crs_graph.col_ind( crs_graph.row_ptr( p ) + n );
    }
};

} // namespace Cabana

#endif