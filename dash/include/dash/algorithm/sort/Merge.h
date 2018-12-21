#ifndef DASH__ALGORITHM__SORT__MERGE_H
#define DASH__ALGORITHM__SORT__MERGE_H

#include <dash/algorithm/Copy.h>
#include <dash/algorithm/sort/ThreadPool.h>
#include <dash/algorithm/sort/Types.h>

#include <dash/dart/if/dart_communication.h>

#include <dash/internal/Logging.h>

#include <map>

namespace dash {
namespace impl {

template <typename GlobIterT, typename SendInfoT, typename LocalIt>
inline auto psort__exchange_data(
    GlobIterT                                 gbegin,
    const LocalIt                             lbuffer,
    std::vector<dash::default_index_t> const& remote_partitions,
    SendInfoT&&                               get_send_info)
{
  using iter_type = GlobIterT;

  auto&      pattern       = gbegin.pattern();
  auto&      team          = gbegin.team();
  auto const unit_at_begin = pattern.unit_at(gbegin.pos());

  auto                       nchunks = team.size();
  std::vector<dart_handle_t> handles(nchunks, DART_HANDLE_NULL);

  if (nullptr == lbuffer) {
    return handles;
  }

  std::size_t target_count, src_disp, target_disp;

  for (auto const& unit : remote_partitions) {
    std::tie(target_count, src_disp, target_disp) = get_send_info(unit);

    if (0 == target_count) {
      continue;
    }

    DASH_LOG_TRACE(
        "async copy",
        "source unit",
        unit,
        "target_count",
        target_count,
        "src_disp",
        src_disp,
        "target_disp",
        target_disp);

    // Get a global iterator to the first local element of a unit within the
    // range to be sorted [begin, end)
    //
    iter_type it_src =
        (unit == unit_at_begin)
            ?
            /* If we are the unit at the beginning of the global range simply
               return begin */
            gbegin
            :
            /* Otherwise construct an global iterator pointing the first local
               element from the correspoding unit */
            iter_type{std::addressof(gbegin.globmem()),
                      pattern,
                      pattern.global_index(
                          static_cast<dash::team_unit_t>(unit), {})};

    dash::internal::get_handle(
        (it_src + src_disp).dart_gptr(),
        std::addressof(*(lbuffer + target_disp)),
        target_count,
        std::addressof(handles[unit]));
  }

  return handles;
}

template <class LocalIt, class ThreadPoolT, class SendInfoT>
inline auto psort__schedule_copy_tasks(
    const LocalIt                             lbuffer_from,
    LocalIt                                   lbuffer_to,
    dash::team_unit_t                         whoami,
    std::vector<dash::default_index_t> const& remote_partitions,
    std::vector<dart_handle_t>&&              copy_handles,
    ThreadPoolT&                              thread_pool,
    SendInfoT&&                               get_send_info)
{
  // Futures for the merges - only used to signal readiness.
  // Use a std::map because emplace will not invalidate any
  // references or iterators.
  impl::ChunkDependencies chunk_dependencies;

  std::transform(
      std::begin(remote_partitions),
      std::end(remote_partitions),
      std::inserter(chunk_dependencies, chunk_dependencies.begin()),
      [&thread_pool, &copy_handles](auto partition) {
        // our copy handle
        dart_handle_t& handle = copy_handles[partition];
        return std::make_pair(
            // the partition range
            std::make_pair(partition, partition + 1),
            // the future of our asynchronous communication task
            thread_pool.submit([&handle]() {
              if (handle != DART_HANDLE_NULL) {
                dart_wait(&handle);
              }
            }));
      });

  std::size_t target_count, src_disp, target_disp;
  std::tie(target_count, src_disp, target_disp) = get_send_info(whoami);
  // Create an entry for the local part
  impl::ChunkRange local_range = std::make_pair(whoami, whoami + 1);
  chunk_dependencies.emplace(
      local_range,
      thread_pool.submit([target_count,
                          local_range,
                          src_disp,
                          target_disp,
                          lbuffer_from,
                          lbuffer_to] {
        if (target_count) {
          std::copy(
              std::next(lbuffer_from, src_disp),
              std::next(lbuffer_from, src_disp + target_count),
              std::next(lbuffer_to, target_disp));
        }
      }));
  DASH_ASSERT_EQ(
      remote_partitions.size() + 1,
      chunk_dependencies.size(),
      "invalid chunk dependencies");

  return std::move(chunk_dependencies);
}

template <
    typename LocalIt,
    typename MergeDeps,
    typename SortCompT,
    typename ThreadPoolT>
inline void psort__merge_local(
    LocalIt                         lbuffer_from,
    LocalIt                         lbuffer_to,
    const std::vector<std::size_t>& target_displs,
    MergeDeps&                      chunk_dependencies,
    SortCompT                       sort_comp,
    dash::Team const&               team,
    ThreadPoolT&                    thread_pool,
    bool                            in_place)
{
  auto const nunits  = team.size();
  auto       nchunks = nunits;
  // number of merge steps in the tree
  auto const depth = static_cast<size_t>(std::ceil(std::log2(nchunks)));

  // calculate the prefix sum among all receive counts to find the offsets for
  // merging

  for (std::size_t d = 0; d < depth; ++d) {
    // distance between first and mid iterator while merging
    auto const step = std::size_t(0x1) << d;
    // distance between first and last iterator while merging
    auto const dist = step << 1;
    // number of merges
    auto const nmerges = nchunks >> 1;

    // Start threaded merges. When d == 0 they depend on dash::copy to finish,
    // later on other merges.
    for (std::size_t m = 0; m < nmerges; ++m) {
      auto f  = m * dist;
      auto mi = m * dist + step;
      // sometimes we have a lonely merge in the end, so we have to guarantee
      // that we do not access out of bounds
      auto l = std::min(m * dist + dist, nunits);

      // tuple of chunk displacements. Be cautious with the indexes and the
      // order in make_tuple
      static constexpr int left   = 0;
      static constexpr int right  = 1;
      static constexpr int middle = 2;

      auto chunk_displs = std::make_tuple(
          // left
          target_displs[f],
          // right
          target_displs[l],
          // middle
          target_displs[mi]);

      // pair of merge dependencies
      auto merge_deps =
          std::make_pair(impl::ChunkRange{f, mi}, impl::ChunkRange{mi, l});

      // Start a thread that blocks until the two previous merges are ready.
      auto&&     fut = thread_pool.submit([nunits,
                                       lbuffer_to,
                                       lbuffer_from,
                                       displs = std::move(chunk_displs),
                                       deps   = std::move(merge_deps),
                                       sort_comp,
                                       in_place,
                                       &team,
                                       &chunk_dependencies]() {
        // indexes for displacements

        auto first = std::next(lbuffer_from, std::get<left>(displs));
        auto mid   = std::next(lbuffer_from, std::get<middle>(displs));
        auto last  = std::next(lbuffer_from, std::get<right>(displs));
        // Wait for the left and right chunks to be copied/merged
        // This guarantees that for
        //
        // [____________________________]
        // ^f           ^mi             ^l
        //
        // [f, mi) and [mi, f) are both merged sequences when the task
        // continues.

        auto dep_l = std::get<left>(deps);
        auto dep_r = std::get<right>(deps);

        if (chunk_dependencies[dep_l].valid()) {
          chunk_dependencies[dep_l].wait();
        }
        if (chunk_dependencies[dep_r].valid()) {
          chunk_dependencies[dep_r].wait();
        }

        if (in_place) {
          // The final merge can be done non-inplace, because we need to
          // copy the result to the final buffer anyways.
          if (dep_l.first == 0 && dep_r.second == nunits) {
            // Make sure everyone merged their parts (necessary for the copy
            // into the final buffer)
            team.barrier();
            std::merge(first, mid, mid, last, lbuffer_to, sort_comp);
          }
          else {
            std::inplace_merge(first, mid, last, sort_comp);
          }
        }
        else {
          DASH_THROW(
              dash::exception::NotImplemented,
              "non-inplace merge not supported yet");
          // std::merge(first, mid, mid, last, std::next(lbuffer_to, first),
          // sort_comp);
        }
        DASH_LOG_TRACE("merged chunks", dep_l.first, dep_r.second);
      });
      ChunkRange to_merge(f, l);
      chunk_dependencies.emplace(to_merge, std::move(fut));
    }

    nchunks -= nmerges;
  }
}

}  // namespace impl
}  // namespace dash

#endif
