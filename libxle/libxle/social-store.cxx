// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libxle/social-store.hxx>

using namespace std;

namespace xle
{
  const char*
  to_string (relationship_view v) noexcept
  {
    switch (v)
    {
      case relationship_view::all:            return "All";
      case relationship_view::favorite:       return "Favorite";
      case relationship_view::legacy_friends: return "LegacyXboxLiveFriends";
    }

    LIBOBE_UNREACHABLE ();
  }

  // Return true if the ASCII strings are equal ignoring case.
  //
  static bool
  icase_equal (string_view x, string_view y)
  {
    auto lower = [] (char c)
    {
      return c >= 'A' && c <= 'Z' ? static_cast<char> (c - 'A' + 'a') : c;
    };

    return ranges::equal (x, y, [&lower] (char a, char b)
    {
      return lower (a) == lower (b);
    });
  }

  relationship_view
  to_relationship_view (string_view s)
  {
    // Note that Xbox Live matches the names ignoring case, so we do too.
    //
    static const pair<string_view, relationship_view> views[]
    {
      {"All",                   relationship_view::all},
      {"Favorite",              relationship_view::favorite},
      {"LegacyXboxLiveFriends", relationship_view::legacy_friends}
    };

    for (const auto& [n, v]: views)
    {
      if (icase_equal (s, n))
        return v;
    }

    throw invalid_input ("invalid relationship view '{}'", s);
  }

  const char*
  to_string (follow_result r) noexcept
  {
    switch (r)
    {
      case follow_result::added:    return "added";
      case follow_result::existing: return "existing";
      case follow_result::full:     return "full";
    }

    LIBOBE_UNREACHABLE ();
  }

  // social_store
  //
  social_store::
  ~social_store () = default;

  // memory_social_store
  //
  ranges::subrange<memory_social_store::relationship_map::const_iterator>
  memory_social_store::
  relationships (xuid o) const
  {
    auto [b, e] (relationships_.equal_range (o));
    return ranges::subrange (b, e);
  }

  awaitable<people_page> memory_social_store::
  people (xuid o, relationship_view v, size_t start, size_t count)
  {
    vector<person> ps;

    for (const auto& [k, e]: relationships (o))
    {
      person p {k.second,
                e.favorite,
                relationships_.contains (key (k.second, o)),
                e.added};

      if ((v == relationship_view::favorite && !p.favorite) ||
          (v == relationship_view::legacy_friends && !p.following_caller))
        continue;

      ps.push_back (p);
    }

    ranges::sort (ps, [] (const person& x, const person& y)
    {
      return x.added != y.added ? x.added < y.added : x.user < y.user;
    });

    people_page r;
    r.total = ps.size ();

    if (start < ps.size ())
    {
      auto b (ps.begin () + start);
      auto e (b + min (count, ps.size () - start));
      r.people.assign (b, e);
    }

    co_return r;
  }

  awaitable<optional<person>> memory_social_store::
  find (xuid o, xuid u)
  {
    auto i (relationships_.find (key (o, u)));

    if (i == relationships_.end ())
      co_return nullopt;

    co_return person {u,
                      i->second.favorite,
                      relationships_.contains (key (u, o)),
                      i->second.added};
  }

  awaitable<follow_result> memory_social_store::
  follow (xuid o, xuid u, timestamp now, size_t limit)
  {
    if (relationships_.contains (key (o, u)))
      co_return follow_result::existing;

    if (static_cast<size_t> (ranges::distance (relationships (o))) >= limit)
      co_return follow_result::full;

    relationships_.emplace (key (o, u), relationship {false, now});
    co_return follow_result::added;
  }

  awaitable<bool> memory_social_store::
  unfollow (xuid o, xuid u)
  {
    co_return relationships_.erase (key (o, u)) != 0;
  }

  awaitable<bool> memory_social_store::
  favorite (xuid o, xuid u, bool f)
  {
    auto i (relationships_.find (key (o, u)));

    if (i == relationships_.end ())
      co_return false;

    i->second.favorite = f;
    co_return true;
  }
}
