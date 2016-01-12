/*
 * Sequential GUPS benchmark for various pattern types.
 *
 */
/* @DASH_HEADER@ */

#include "../bench.h"
#include <libdash.h>

#include <array>
#include <vector>
#include <deque>
#include <iostream>
#include <iomanip>
#include <unistd.h>

using std::cout;
using std::endl;

typedef dash::util::Timer<
          dash::util::TimeMeasure::Clock
        > Timer;

typedef double value_t;

template<typename MatrixType>
void init_values(
  MatrixType & matrix_a,
  MatrixType & matrix_b,
  MatrixType & matrix_c);

template<typename MatrixType>
double test_summa(
  MatrixType & matrix_a,
  MatrixType & matrix_b,
  MatrixType & matrix_c,
  unsigned     repeat);

void perform_test(long long n, unsigned repeat);

int main(int argc, char* argv[]) {
#ifndef DASH_ENABLE_MKL
  cout << "WARNING: MKL not available, "
       << "falling back to naive local matrix multiplication"
       << endl;
#endif
  dash::init(&argc, &argv);

  Timer::Calibrate(0);

  std::deque<std::pair<int, int>> tests;

  tests.push_back({0,      0}); // this prints the header
#ifdef DASH_ENABLE_MKL
  tests.push_back({1024, 100});
  tests.push_back({2048,  50});
  tests.push_back({4096,   5});
  tests.push_back({8192,   1});
  tests.push_back({16384,  1});
#else
  tests.push_back({64,   100});
  tests.push_back({256,   50});
  tests.push_back({1024,  10});
  tests.push_back({2048,   1});
#endif

  for (auto test: tests) {
    perform_test(test.first, test.second);
  }

  dash::finalize();

  return 0;
}

void perform_test(
  long long n,
  unsigned  repeat)
{
  auto num_units = dash::size();
  if (n == 0) {
    if (dash::myid() == 0) {
      cout << std::setw(10)
           << "units"
           << ", "
           << std::setw(10)
           << "n"
           << ", "
           << std::setw(10)
           << "size"
           << ", "
           << std::setw(10)
           << "gflop"
           << ", "
           << std::setw(10)
           << "gflop/s"
           << ", "
           << std::setw(10)
           << "repeats"
           << ", "
           << std::setw(11)
           << "time (s)"
           << endl;
    }
    return;
  }

  // Automatically deduce pattern type satisfying constraints defined by
  // SUMMA implementation:
  dash::SizeSpec<2> size_spec(n, n);
  dash::TeamSpec<2> team_spec;
  auto pattern = dash::make_pattern <
                 dash::summa_pattern_partitioning_constraints,
                 dash::summa_pattern_mapping_constraints,
                 dash::summa_pattern_layout_constraints >(
                   size_spec,
                   team_spec);
  dash::Matrix<value_t, 2> matrix_a(pattern);
  dash::Matrix<value_t, 2> matrix_b(pattern);
  dash::Matrix<value_t, 2> matrix_c(pattern);

  double t_summa = test_summa(matrix_a, matrix_b, matrix_c, repeat);

  dash::barrier();

  if (dash::myid() == 0) {
    double gflop   = static_cast<double>(n * n * n * 2) * 1.0e-9 * repeat;
    double seconds = 1.0e-6 * t_summa;
    double gflops  = gflop / seconds;
    cout << std::setw(10)
         << num_units
         << ", "
         << std::setw(10)
         << n
         << ", "
         << std::setw(10)
         << (n*n)
         << ", "
         << std::setw(10) << std::fixed << std::setprecision(4)
         << gflop
         << ", "
         << std::setw(10) << std::fixed << std::setprecision(4)
         << gflops
         << ", "
         << std::setw(10)
         << repeat
         << ", "
         << std::setw(11) << std::fixed << std::setprecision(4)
         << seconds
         << endl;
  }
}

template<typename MatrixType>
void init_values(
  MatrixType & matrix_a,
  MatrixType & matrix_b,
  MatrixType & matrix_c)
{
  // Initialize local sections of matrix C:
  auto unit_id          = dash::myid();
  auto pattern          = matrix_c.pattern();
  auto block_cols       = pattern.blocksize(1);
  auto block_rows       = pattern.blocksize(1);
  auto num_blocks_cols  = pattern.extent(0) / block_cols;
  auto num_blocks_rows  = pattern.extent(1) / block_rows;
  auto num_blocks       = num_blocks_rows * num_blocks_cols;
  auto num_local_blocks = num_blocks / dash::Team::All().size();
  for (auto l_block_idx = 0; l_block_idx < num_local_blocks; ++l_block_idx) {
    auto l_block = matrix_a.local.block(l_block_idx);
    for (auto elem : l_block) {
      elem = 100000 * (unit_id + 1) + l_block_idx;
    }
  }
  // Matrix B is identity matrix:
  for (auto diag_idx = 0; diag_idx < pattern.extent(0); ++diag_idx) {
    auto elem_ref = matrix_b[diag_idx][diag_idx];
    if (elem_ref.is_local()) {
      elem_ref = 1;
    }
  }
}

template<typename MatrixType>
double test_summa(
  MatrixType & matrix_a,
  MatrixType & matrix_b,
  MatrixType & matrix_c,
  unsigned     repeat)
{
  init_values(matrix_a, matrix_b, matrix_c);

  auto ts_start = Timer::Now();
  for (auto i = 0; i < repeat; ++i) {
    dash::summa(matrix_a, matrix_b, matrix_c);
  }
  return Timer::ElapsedSince(ts_start);
}


