///
/// Copyright (c) 2013, Intel Corporation
/// Copyright (c) 2021, NVIDIA
///
/// Redistribution and use in source and binary forms, with or without
/// modification, are permitted provided that the following conditions
/// are met:
///
/// * Redistributions of source code must retain the above copyright
///       notice, this list of conditions and the following disclaimer.
/// * Redistributions in binary form must reproduce the above
///       copyright notice, this list of conditions and the following
///       disclaimer in the documentation and/or other materials provided
///       with the distribution.
/// * Neither the name of Intel Corporation nor the names of its
///       contributors may be used to endorse or promote products
///       derived from this software without specific prior written
///       permission.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
/// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
/// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
/// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
/// COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
/// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
/// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
/// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
/// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
/// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
/// POSSIBILITY OF SUCH DAMAGE.

//////////////////////////////////////////////////////////////////////
///
/// NAME:    transpose
///
/// PURPOSE: This program measures the time for the transpose of a
///          column-major stored matrix into a row-major stored matrix.
///
/// USAGE:   Program input is the matrix order and the number of times to
///          repeat the operation:
///
///          transpose <matrix_size> <# iterations> [tile size]
///
///          An optional parameter specifies the tile size used to divide the
///          individual matrix blocks for improved cache and TLB performance.
///
///          The output consists of diagnostics to make sure the
///          transpose worked and timing statistics.
///
/// HISTORY: Written by  Rob Van der Wijngaart, February 2009.
///          Converted to C++11 by Jeff Hammond, February 2016 and May 2017.
///
//////////////////////////////////////////////////////////////////////

#include "prk_util.h"

#include "range/v3/view/cartesian_product.hpp"
#include "range/v3/view/stride.hpp"

int main(int argc, char * argv[])
{
  std::cout << "Parallel Research Kernels version " << PRKVERSION << std::endl;
  std::cout << "C++11/ranges Matrix transpose: B = A^T" << std::endl;

  //////////////////////////////////////////////////////////////////////
  // Read and test input parameters
  //////////////////////////////////////////////////////////////////////

  int iterations;
  int order;
  int tile_size;
  try {
      if (argc < 3) {
        throw "Usage: <# iterations> <matrix order> [tile size]";
      }

      iterations  = std::atoi(argv[1]);
      if (iterations < 1) {
        throw "ERROR: iterations must be >= 1";
      }

      order = std::atoi(argv[2]);
      if (order <= 0) {
        throw "ERROR: Matrix Order must be greater than 0";
      } else if (order > prk::get_max_matrix_size()) {
        throw "ERROR: matrix dimension too large - overflow risk";
      }

      // default tile size for tiling of local transpose
      tile_size = (argc>3) ? std::atoi(argv[3]) : order;
      // a negative tile size means no tiling of the local transpose
      if (tile_size <= 0) tile_size = order;
      //
      if ((tile_size < order) && (order % tile_size)) {
        throw "ERROR: tile size must evenly divide order";
      }
  }
  catch (const char * e) {
    std::cout << e << std::endl;
    return 1;
  }

  std::cout << "Number of iterations = " << iterations << std::endl;
  std::cout << "Matrix order         = " << order << std::endl;
  std::cout << "Tile size            = " << tile_size << std::endl;

  //////////////////////////////////////////////////////////////////////
  // Allocate space and perform the computation
  //////////////////////////////////////////////////////////////////////

  double trans_time{0};

  prk::vector<double> A(order*order);
  prk::vector<double> B(order*order,0.0);

  // fill A with the sequence 0 to order^2-1 as doubles
  std::iota(A.begin(), A.end(), 0.0);

  // untiled
  auto v = ranges::views::cartesian_product(
               ranges::views::iota(0, order),
               ranges::views::iota(0, order)
           );

  // tiled: s is the strided (outer) view and t is the tile (inner) view
  auto s = ranges::views::cartesian_product(
               ranges::stride_view(ranges::views::iota(0, order), tile_size),
               ranges::stride_view(ranges::views::iota(0, order), tile_size)
           );
  auto t = ranges::views::cartesian_product(
               ranges::views::iota(0, tile_size),
               ranges::views::iota(0, tile_size)
           );

  for (int iter = 0; iter<=iterations; iter++) {

    if (iter==1) trans_time = prk::wtime();

    if (tile_size < order) {
#if USE_FOR_EACH_RANGES
      std::for_each(std::begin(s), std::end(s), [=,&A,&B] (auto itjt) {
          std::for_each(std::begin(t), std::end(t), [=,&A,&B] (auto ij) {
              auto [it, jt] = itjt;
              auto [i, j] = ij;
              B[(it+i)*order+(jt+j)] += A[(jt+j)*order+(it+i)];
              A[(jt+j)*order+(it+i)] += 1.0;
          });
      });
#else
      for (auto itjt : s) {
          auto [it, jt] = itjt;
          for (auto ij : t) {
              auto [i, j] = ij;
              B[(it+i)*order+(jt+j)] += A[(jt+j)*order+(it+i)];
              A[(jt+j)*order+(it+i)] += 1.0;
          }
      }
#endif
    } else {
#if USE_FOR_EACH_RANGES
      std::for_each(std::begin(v), std::end(v), [=,&A,&B] (auto ij) {
          auto [i, j] = ij;
          B[i*order+j] += A[j*order+i];
          A[j*order+i] += 1.0;
      });
#else
      for (auto ij : v) {
          auto [i, j] = ij;
          B[i*order+j] += A[j*order+i];
          A[j*order+i] += 1.0;
      }
#endif
    }
  }
  trans_time = prk::wtime() - trans_time;

  //////////////////////////////////////////////////////////////////////
  /// Analyze and output results
  //////////////////////////////////////////////////////////////////////

  // TODO: replace with std::generate, std::accumulate, or similar
  auto const addit = (iterations+1.) * (iterations/2.);
  double abserr(0);
  auto irange = prk::range(0,order);
  auto jrange = prk::range(0,order);
  for (auto i : irange) {
    for (auto j : jrange) {
      const int ij = i*order+j;
      const int ji = j*order+i;
      const double reference = static_cast<double>(ij)*(1.+iterations)+addit;
      abserr += prk::abs(B[ji] - reference);
    }
  }

#ifdef VERBOSE
  std::cout << "Sum of absolute differences: " << abserr << std::endl;
#endif

  const auto epsilon = 1.0e-8;
  if (abserr < epsilon) {
    std::cout << "Solution validates" << std::endl;
    auto avgtime = trans_time/iterations;
    auto bytes = (size_t)order * (size_t)order * sizeof(double);
    std::cout << "Rate (MB/s): " << 1.0e-6 * (2L*bytes)/avgtime
              << " Avg time (s): " << avgtime << std::endl;
  } else {
    std::cout << "ERROR: Aggregate squared error " << abserr
              << " exceeds threshold " << epsilon << std::endl;
    return 1;
  }

  return 0;
}


