// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <string>
#include <memory>    // unique_ptr
#include <print>
#include <chrono>
#include <sstream>
#include <utility>   // to_underlying()
#include <iostream>
#include <optional>
#include <exception> // exception_ptr, rethrow_exception()
#include <stdexcept> // invalid_argument

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/awaitable.hpp>

#include <libxle/social-store.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace xle;

namespace asio = boost::asio;

// Run the store operation to completion and return its result.
//
template <typename T>
static T
run (asio::awaitable<T> a)
{
  asio::io_context ctx;
  optional<T> r;
  exception_ptr e;

  asio::co_spawn (ctx, move (a), [&r, &e] (exception_ptr x, T v)
  {
    if (x)
      e = x;
    else
      r = move (v);
  });
  ctx.run ();

  if (e)
    rethrow_exception (e);

  return move (*r);
}

static timestamp
to_timestamp (uint64_t s)
{
  return timestamp (chrono::seconds (s));
}

static uint64_t
to_seconds (timestamp t)
{
  return static_cast<uint64_t> (
    chrono::duration_cast<chrono::seconds> (t.time_since_epoch ()).count ());
}

static void
print_person (const person& p)
{
  println ("{} favorite={} following={} added={}",
           to_underlying (p.user),
           p.favorite,
           p.following_caller,
           to_seconds (p.added));
}

// Usage: argv[0]
//
// Read commands from stdin, one per line, perform them on the memory store,
// and print the results to stdout:
//
// follow <owner> <user> <time> <limit>  Print the follow result.
// unfollow <owner> <user>               Print whether the user was followed.
// favorite <owner> <user> <bool>        Print whether the user is followed.
// find <owner> <user>                   Print the followed user or 'none'.
// people <owner> <view> <start> <count> Print the total and the people, one
//                                       per line.
//
// The users are XUIDs and the times are seconds since the epoch. A followed
// user is printed as:
//
// <user> favorite=<bool> following=<bool> added=<time>
//
int
main ()
try
{
  memory_social_store s;

  for (string l; getline (cin, l); )
  {
    istringstream is (l);
    string c;
    is >> c;

    auto next = [&is] ()
    {
      string v;
      if (!(is >> v))
        throw invalid_argument ("missing argument");
      return v;
    };

    auto number = [&next] ()
    {
      return stoull (next ());
    };

    auto user = [&number] ()
    {
      return xuid {number ()};
    };

    auto boolean = [&next] ()
    {
      const string v (next ());

      if (v != "true" && v != "false")
        throw invalid_argument ("invalid boolean '" + v + "'");

      return v == "true";
    };

    if (c == "follow")
    {
      const xuid o (user ()), u (user ());
      const timestamp t (to_timestamp (number ()));

      println ("{}", run (s.follow (o, u, t, number ())));
    }
    else if (c == "unfollow")
    {
      const xuid o (user ()), u (user ());

      println ("{}", run (s.unfollow (o, u)));
    }
    else if (c == "favorite")
    {
      const xuid o (user ()), u (user ());

      println ("{}", run (s.favorite (o, u, boolean ())));
    }
    else if (c == "find")
    {
      const xuid o (user ()), u (user ());

      if (optional<person> p = run (s.find (o, u)))
      {
        assert (p->user == u);
        print_person (*p);
      }
      else
        println ("none");
    }
    else if (c == "people")
    {
      const xuid o (user ());
      const relationship_view v (to_relationship_view (next ()));
      const size_t b (number ());

      const people_page p (run (s.people (o, v, b, number ())));

      println ("total {}", p.total);
      for (const person& x: p.people)
        print_person (x);
    }
    else
      throw invalid_argument ("unknown command '" + c + "'");
  }
}
catch (const invalid_argument& e)
{
  println (cerr, "error: {}", e.what ());
  return 1;
}
