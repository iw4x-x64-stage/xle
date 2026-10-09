// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <string>
#include <memory>    // unique_ptr
#include <print>
#include <format>
#include <random>
#include <chrono>
#include <sstream>
#include <concepts>  // movable
#include <utility>   // to_underlying()
#include <iostream>
#include <optional>
#include <exception> // exception_ptr, rethrow_exception()
#include <stdexcept> // invalid_argument

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/awaitable.hpp>

#include <odb/pgsql/database.hxx>
#include <odb/pgsql/connection.hxx>

#include <libxle/pgsql.hxx>
#include <libxle/social-store.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace xle;

namespace asio = boost::asio;

// The test database, created in the PostgreSQL server of the maintenance
// database on construction and dropped on destruction.
//
// Note that we use the libpq defaults for everything else (the user, the
// host, etc) and that the database is dropped even if it is still in use
// (by the pool connections of a failed test).
//
class test_database
{
public:
  explicit
  test_database (string maintenance)
    : admin_ ("", "", move (maintenance))
  {
    random_device rd;
    name_ = format ("xle_test_{:08x}{:08x}", rd (), rd ());

    admin_.connection ()->execute ("CREATE DATABASE " + name_);
  }

  ~test_database ()
  {
    try
    {
      admin_.connection ()->execute (
        "DROP DATABASE " + name_ + " WITH (FORCE)");
    }
    catch (const std::exception& e)
    {
      println (cerr,
               "warning: unable to drop database {}: {}",
               name_, e.what ());
    }
  }

  const string&
  name () const noexcept {return name_;}

  test_database (const test_database&) = delete;
  test_database& operator= (const test_database&) = delete;

private:
  odb::pgsql::database admin_;
  string               name_;
};

// Run the store operation to completion and return its result.
//
template <movable T>
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

// Usage: argv[0] [--pgsql <database>]
//
// Read commands from stdin, one per line, perform them on the store, and
// print the results to stdout:
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
// The store is kept in memory or, with --pgsql, in a new PostgreSQL
// database created in the server of the specified maintenance database
// (and dropped on exit).
//
int
main (int argc, char* argv[])
try
{
  optional<string> pgsql;

  if (argc == 3 && string (argv[1]) == "--pgsql")
    pgsql = argv[2];
  else if (argc != 1)
  {
    println (cerr, "usage: {} [--pgsql <database>]", argv[0]);
    return 1;
  }

  // Create the store.
  //
  // Note that the order of the declarations matters: the database must
  // outlive the store and the test database the database.
  //
  optional<test_database> tdb;
  unique_ptr<pgsql_database> db;
  unique_ptr<social_store> store;

  try
  {
    if (pgsql)
    {
      tdb.emplace (*pgsql);

      pgsql_settings ps;
      ps.name = tdb->name ();
      ps.max_connections = 2;

      db = make_unique<pgsql_database> (ps);
      db->migrate ();

      store = make_unique<pgsql_social_store> (*db);
    }
    else
      store = make_unique<memory_social_store> ();
  }
  catch (const std::exception& e)
  {
    println (cerr, "error: unable to create store: {}", e.what ());
    return 1;
  }

  social_store& s (*store);

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
