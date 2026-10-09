// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libxle/social-service.hxx>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <libxle/target.hxx>

using namespace std;

namespace xle
{
  namespace json = boost::json;

  using boost::asio::awaitable;

  // The social network of the friends (see social_service).
  //
  static const char legacy_network[] = "LegacyXboxLive";

  social_service::
  social_service (social_store& s, social_settings ss)
    : store_ (s),
      settings_ (move (ss))
  {
  }

  awaitable<service_reply> social_service::
  handle (xuid caller, string_view method, string_view target)
  {
    // Return the error reply with the status and the formatted error.
    //
    auto error = [] <typename... A>
      requires formattable_arguments<A...>
      (uint16_t s, std::format_string<A...> f, A&&... a)
    {
      return service_reply {s,
                            string (),
                            std::format (f, std::forward<A> (a)...)};
    };

    // Parse the target and see if it is the people list.
    //
    optional<request_target> t;
    try
    {
      t.emplace (target);
    }
    catch (const invalid_argument& e)
    {
      co_return error (400, "{}", e.what ());
    }

    const strings& p (t->path);

    if (p.size () != 3 || p[0] != "users" || p[2] != "people")
      co_return error (404, "unknown target '{}'", target);

    if (method != "GET")
      co_return error (405, "method {} not allowed", method);

    // Parse the user and the query parameters.
    //
    relationship_view v (relationship_view::all);
    size_t start (0);
    size_t count (settings_.max_items);
    try
    {
      if (const xuid u (parse_user (p[1])); u != caller)
        co_return error (403,
                         "people of user {} not accessible",
                         to_underlying (u));

      if (const string* s = t->parameter ("view"))
        v = to_relationship_view (*s);

      if (const string* s = t->parameter ("startIndex"))
        start = parse_unsigned (*s, "start index");

      if (const string* s = t->parameter ("maxItems"))
        count = min<uint64_t> (parse_unsigned (*s, "maximum items"), count);
    }
    catch (const invalid_argument& e)
    {
      co_return error (400, "{}", e.what ());
    }

    // List the people.
    //
    const people_page pp (co_await store_.people (caller, v, start, count));

    json::array ps;
    for (const person& x: pp.people)
    {
      json::array ns;
      if (x.following_caller)
        ns.emplace_back (legacy_network);

      ps.emplace_back (json::object {
        {"xuid",              to_string (to_underlying (x.user))},
        {"isFavorite",        x.favorite},
        {"isFollowingCaller", x.following_caller},
        {"socialNetworks",    move (ns)}});
    }

    json::object r {
      {"totalCount", pp.total},
      {"people",     move (ps)}};

    co_return service_reply {200, json::serialize (r), string ()};
  }
}
