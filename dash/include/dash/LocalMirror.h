#ifndef DASH__LOCAL_MIRROR_H__INCLUDED
#define DASH__LOCAL_MIRROR_H__INCLUDED

#include <cpp17/polymorphic_allocator.h>

#include <dash/memory/MemorySpace.h>

#include <dash/algorithm/Copy.h>

#include <dash/Exception.h>
#include <dash/Future.h>
#include <dash/Types.h>

namespace dash {

template <class GlobInputIt, class MemorySpace>
class LocalMirror {
  using pointer_traits =
      std::pointer_traits<typename MemorySpace::void_pointer>;

  using size_type     = dash::default_size_t;
  using index_type    = dash::default_index_t;
  using memory_traits = dash::memory_space_traits<MemorySpace>;
  using value_t       = typename std::remove_cv<
      typename dash::iterator_traits<GlobInputIt>::value_type>::type;

  using allocator_type = cpp17::pmr::polymorphic_allocator<value_t>;

public:
  /// public type-defs
  using value_type = value_t;
  using memory     = MemorySpace;

  using pointer = typename pointer_traits::template rebind<value_type>;
  using const_pointer =
      typename pointer_traits::template rebind<const value_type>;

  using iterator = GlobInputIt;

public:
  // Default constructor
  constexpr explicit LocalMirror(MemorySpace *r = nullptr);

  LocalMirror(const LocalMirror &) = delete;
  LocalMirror(LocalMirror &&)      = default;
  LocalMirror &operator=(LocalMirror const &) = default;
  LocalMirror &operator=(LocalMirror &&) = default;

  void replicate(GlobInputIt begin, GlobInputIt end);

  void reserve(size_type n);

  void wait()
  {
    std::for_each(
        std::begin(m_futs), std::end(m_futs), [](dash::Future<pointer> &fut) {
          fut.wait();
        });
  }
  void wait_local();

  pointer begin()
  {
    if (m_futs.empty()) {
      return nullptr;
    }

    std::for_each(
        std::begin(m_futs), std::end(m_futs), [](dash::Future<pointer> &fut) {
          fut.wait();
        });

    return m_data.data();
  }
  constexpr const_pointer begin() const
  {
    if (m_futs.empty()) {
      return nullptr;
    }

    std::for_each(
        std::begin(m_futs), std::end(m_futs), [](dash::Future<pointer> &fut) {
          fut.wait();
        });

    return m_data.data();
  }
  pointer end()
  {
    if (m_size == 0) {
      return nullptr;
    }
    return std::next(m_data, m_size);
  }
  constexpr const_pointer end() const
  {
    if (m_size == 0) {
      return nullptr;
    }
    return std::next(m_data, m_size);
  }
  /* lbegin works */
  pointer lbegin()
  {
    // if we have no local portion or we did not mirror anything before
    // --> early exit
    if (m_lend_gindex == std::numeric_limits<index_type>::min() ||
        m_futs.empty()) {
      return nullptr;
    }
    m_futs.back().wait();
    return std::next(m_data.data(), m_lbegin_gindex);
  }
  constexpr const_pointer lbegin() const
  {
    // if we have no local portion or we did not mirror anything before
    // --> early exit
    if (m_lend_gindex == std::numeric_limits<index_type>::min() ||
        m_futs.empty()) {
      return nullptr;
    }

    m_futs.back().wait();
    return std::next(m_data.data(), m_lbegin_gindex);
  }
  pointer lend()
  {
    if (m_lend_gindex == std::numeric_limits<index_type>::min() ||
        m_futs.empty()) {
      return nullptr;
    }
    return std::next(m_data.data(), m_lend_gindex);
  }
  constexpr const_pointer lend() const
  {
    if (m_lend_gindex == std::numeric_limits<index_type>::min() ||
        m_futs.empty()) {
      return nullptr;
    }
    return std::next(m_data.data(), m_lend_gindex);
  }

private:
  allocator_type                             m_allocator{};
  std::vector<value_type, allocator_type>    m_data;
  mutable std::vector<dash::Future<pointer>> m_futs;

  // local indexes of local portion
  index_type m_lbegin_lindex{std::numeric_limits<index_type>::max()};
  index_type m_lend_lindex{std::numeric_limits<index_type>::min()};

  // global indexes of local portion
  index_type m_lbegin_gindex{std::numeric_limits<index_type>::max()};
  index_type m_lend_gindex{std::numeric_limits<index_type>::min()};

  size_type m_size{};
};

template <class GlobInputIt, class MemorySpace>
inline constexpr LocalMirror<GlobInputIt, MemorySpace>::LocalMirror(
    MemorySpace *r)
  : m_allocator(
        r ? r
          : get_default_memory_space<
                typename memory_traits::memory_space_domain_category,
                typename memory_traits::memory_space_type_category>())
  , m_data(m_allocator)
{
  //DASH_LOG_DEBUG("< LocalMirror.LocalMirror(resource)");

  //DASH_LOG_DEBUG("LocalMirror.LocalMirror(resource) >");
}

template <class GlobInputIt, class MemorySpace>
inline void LocalMirror<GlobInputIt, MemorySpace>::replicate(
    GlobInputIt first, GlobInputIt last)
{
  DASH_ASSERT_MSG(first.pattern() == last.pattern(), "invalid patterns");

  using pattern_t     = typename GlobInputIt::pattern_type;
  auto const &pattern = first.pattern();

  m_size = dash::distance(first, last);

  DASH_LOG_DEBUG_VAR("LocalMirror.mirror", m_size);

  m_data.resize(m_size);

  auto const first_gindex = first.pos();
  auto const last_gindex  = last.pos();

  if (pattern.local_size() == 0) {
    // TODO rko: this check does not work for all patterns...fix it
    DASH_THROW(
        dash::exception::NotImplemented,
        "corner case (empty local range) not implemented yet");
  }

  // Global index of first element in pattern
  auto const l_first_gindex = pattern.lbegin();
  // Global index of last element in pattern
  auto const l_last_gindex = pattern.lend();

  DASH_LOG_TRACE(
      "LocalMirror.mirror(first, last) -> indexes",
      l_first_gindex,
      l_last_gindex,
      first_gindex,
      last_gindex);

  if (l_first_gindex < first_gindex ||  // local end before global begin
      l_last_gindex > last_gindex) {    // local begin after global end
    DASH_THROW(
        dash::exception::NotImplemented,
        "corner case (local range does not intercept global range) which is "
        "not implemented yet");
  }
  // Intersect local range and global range, in global index domain:
  m_lbegin_gindex = std::max(l_first_gindex, first_gindex);
  m_lend_gindex   = std::min(l_last_gindex, last_gindex);
  DASH_LOG_TRACE_VAR("LocalMirror.mirror(first, last)", m_lbegin_gindex);
  DASH_LOG_TRACE_VAR("LocalMirror.mirror(first, last)", m_lend_gindex);
  // Global positions of local range to global coordinates, O(d):
  auto const lbegin_gcoords = pattern.coords(m_lbegin_gindex);
  // Subtract 1 from global end offset as it points one coordinate
  // past the last index which is out of the valid coordinates range:
  auto const lend_gcoords = pattern.coords(m_lend_gindex - 1);
  // Global coordinates of local range to local indices, O(d):
  m_lbegin_lindex = pattern.at(lbegin_gcoords);
  // Add 1 to local end index to it points one coordinate past the
  // last index:
  m_lend_lindex = pattern.at(lend_gcoords);

  if (m_lend_lindex ==
      std::numeric_limits<typename pattern_t::index_type>::max()) {
    DASH_THROW(
        dash::exception::RuntimeError,
        "index type too small for for local index range");
  }

  m_lend_lindex += 1;
  // Return local index range
  DASH_LOG_TRACE(
      "LocalMirror.mirror(first, last) -> before local range",
      first.pos(),
      (first + m_lend_gindex - 1).pos());

  // copy everthing before local part
  m_futs.emplace_back(
      dash::copy_async(first, first + m_lend_gindex - 1, m_data.data()));

  DASH_LOG_TRACE(
      "LocalMirror.mirror(first, last) -> after local range",
      (first + m_lend_gindex).pos(),
      last.pos());
  // copy everything after local part...
  m_futs.emplace_back(dash::copy_async(
      first + m_lend_gindex, last, std::next(m_data.data(), m_lend_gindex)));

  auto *     data          = m_data.data();
  auto const lbegin_index  = m_lbegin_lindex;
  auto const lend_index    = m_lend_lindex;
  auto const lbegin_gindex = m_lbegin_gindex;

  DASH_LOG_TRACE(
      "LocalMirror.mirror(first, last) -> local range",
      m_lbegin_lindex,
      m_lend_lindex);

  m_futs.emplace_back(dash::Future<pointer>{
      [first, lbegin_index, lend_index, data, lbegin_gindex]() {
        return std::copy(
            std::next(
                static_cast<const_pointer>(first.globmem().lbegin()),
                lbegin_index),
            std::next(
                static_cast<const_pointer>(first.globmem().lbegin()),
                lend_index),
            std::next(data, lbegin_gindex));
      }});
}

}  // namespace dash
#endif
