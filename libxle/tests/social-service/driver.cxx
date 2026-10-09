// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <string>
#include <print>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <iostream>
#include <algorithm> // find_if()
#include <exception> // exception_ptr, rethrow_exception()
#include <stdexcept> // invalid_argument

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/awaitable.hpp>

#include <libxle/target.hxx>
#include <libxle/social-store.hxx>
#include <libxle/social-service.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace xle;

namespace asio = boost::asio;

// Usage: argv[0]
//
// Create the social service with the store in memory and at most 2 people
// per reply, read the lines from stdin, and print the results to stdout.
// The lines are:
//
// follow <owner> <user>             (the owner follows the user)
// favorite <owner> <user>           (the owner marks the user as a favorite)
// <method> <caller> <target>        (the caller's request)
//
// The users follow each other in the order of the lines, one second apart.
// The reply to a request is printed as the status code followed, for the 200
// status, by the body and, for the other statuses, by the error (and, for
// the 405 status, by the allowed methods in parenthesis). A target the
// service doesn't match is printed as 'unmatched'.
//
int
main ()
{
  memory_social_store store;

  social_settings settings;
  settings.max_items = 2;

  social_service service (store, settings);

  // Run the coroutine to completion, rethrowing its exception, if any.
  //
  asio::io_context ctx;

  auto await = [&ctx] (asio::awaitable<void> a)
  {
    exception_ptr r;
    asio::co_spawn (ctx, move (a), [&r] (exception_ptr e) {r = move (e);});

    ctx.run ();
    ctx.restart ();

    if (r)
      rethrow_exception (r);
  };

  try
  {
    timestamp now (chrono::seconds (1000));

    for (string l; getline (cin, l); )
    {
      istringstream is (l);
      string k, t;
      uint64_t x;
      is >> k >> x >> t;

      if (is.fail ())
        throw invalid_argument ("invalid line '" + l + "'");

      if (k == "follow" || k == "favorite")
      {
        const xuid o {x};
        const xuid u {stoull (t)};

        if (k == "follow")
        {
          now += chrono::seconds (1);

          await ([&store, o, u, now] () -> asio::awaitable<void>
          {
            const follow_result r (co_await store.follow (o, u, now, 100));
            assert (r == follow_result::added);
          } ());
        }
        else
        {
          await ([&store, o, u] () -> asio::awaitable<void>
          {
            const bool r (co_await store.favorite (o, u, true));
            assert (r);
          } ());
        }

        continue;
      }

      // Map the method.
      //
      static const pair<string_view, http_method> methods[]
      {
        {"GET",    http_method::get},
        {"POST",   http_method::post},
        {"PUT",    http_method::put},
        {"DELETE", http_method::delete_}
      };

      auto mi (ranges::find_if (methods, [&k] (const auto& m)
      {
        return m.first == k;
      }));

      if (mi == ranges::end (methods))
        throw invalid_argument ("unknown method '" + k + "'");

      const request_target rt (t);

      if (!service.match (rt))
      {
        println ("unmatched");
        continue;
      }

      service_reply r;
      await ([&service, &r, &rt, m = mi->second, c = caller {xuid {x}, "user"}]
             () -> asio::awaitable<void>
      {
        r = co_await service.handle (c, m, rt, "");
      } ());

      if (r.status == 200)
        println ("{} {}", r.status, r.body);
      else if (r.status == 405)
      {
        string a;
        for (http_method m: r.allow)
          a += (a.empty () ? "" : ", ") + string (to_string (m));

        println ("{} {} ({})", r.status, r.error, a);
      }
      else
        println ("{} {}", r.status, r.error);
    }
  }
  catch (const invalid_argument& e)
  {
    println (cerr, "error: {}", e.what ());
    return 1;
  }
}
