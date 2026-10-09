// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libxle/pgsql.hxx>

#include <thread>      // this_thread::sleep_for()
#include <concepts>    // invocable
#include <type_traits> // invoke_result_t, is_void_v

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <odb/database.hxx>
#include <odb/exceptions.hxx>
#include <odb/transaction.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/pgsql/database.hxx>
#include <odb/pgsql/connection-factory.hxx>

#include <libxle/model.hxx>
#include <libxle/model-odb.hxx>

using namespace std;

namespace xle
{
  namespace asio = boost::asio;

  // The advisory lock key that serializes the schema migrations (the ASCII
  // codes of "xle").
  //
  static const int64_t migration_lock (0x786c65);

  // Return the description of the database error. Note that the libpq
  // messages end with a newline, which we strip.
  //
  static string
  describe (const odb::exception& e)
  {
    string r (e.what ());

    while (!r.empty () && (r.back () == '\n' || r.back () == ' '))
      r.pop_back ();

    return r;
  }

  // Execute the SQL statement formatted from the arguments on the connection
  // and return the number of affected rows.
  //
  // Note that the arguments are formatted into the statement verbatim (they
  // are not quoted or escaped) and so should never come from the external
  // input. Such values are passed with ODB queries instead.
  //
  template <typename... A>
    requires formattable_arguments<A...>
  static unsigned long long
  execute (odb::pgsql::connection& c, std::format_string<A...> f, A&&... a)
  {
    return c.execute (std::format (f, std::forward<A> (a)...));
  }

  // pgsql_database
  //
  struct pgsql_database::transaction_pool
  {
    odb::pgsql::database database;
    asio::thread_pool    pool;
    size_t               retry;

    explicit
    transaction_pool (const pgsql_settings& s)
      : database (s.user,
                  s.password,
                  s.name,
                  s.host,
                  s.port,
                  "options='-c default_transaction_isolation=serializable'",
                  make_unique<odb::pgsql::connection_pool_factory> (
                    s.max_connections)),
        pool (s.max_connections),
        retry (s.retry)
    {
    }

    // Perform the operation in a transaction on a pool thread and return
    // its result. Retry the operation on recoverable errors (so it should
    // only change the database) and translate the database errors to
    // store_error.
    //
    // Note that the operation is called on the pool thread and so should not
    // touch the caller's state other than by its (copied) captures.
    //
    template <typename F>
      requires std::invocable<const F&, odb::database&> &&
               (!std::is_void_v<std::invoke_result_t<const F&,
                                                     odb::database&>>)
    awaitable<std::invoke_result_t<const F&, odb::database&>>
    execute (F f)
    {
      using result = std::invoke_result_t<const F&, odb::database&>;

      auto run = [this, f = move (f)] () -> awaitable<result>
      {
        co_return perform (f);
      };

      // Note that the completion resumes us on our executor.
      //
      co_return co_await asio::co_spawn (pool, move (run), asio::use_awaitable);
    }

    template <typename F>
    std::invoke_result_t<const F&, odb::database&>
    perform (const F& f)
    {
      for (size_t i (0);; ++i)
      {
        try
        {
          odb::transaction t (database.begin ());
          auto r (f (database));
          t.commit ();
          return r;
        }
        catch (const odb::recoverable& e)
        {
          if (i == retry)
            throw store_error ("database error: {} (after {} retries)",
                               describe (e), retry);

          // Back off before retrying since the cause (a lost connection, a
          // concurrent update) may take a moment to go away.
          //
          this_thread::sleep_for (
            chrono::milliseconds (10 << min<size_t> (i, 6)));
        }
        catch (const odb::exception& e)
        {
          throw store_error ("database error: {}", describe (e));
        }
      }
    }
  };

  pgsql_database::
  pgsql_database (const pgsql_settings& s)
    : transactions_ (make_unique<transaction_pool> (s))
  {
    LIBOBE_PRE (s.max_connections != 0);
  }

  pgsql_database::
  ~pgsql_database ()
  {
    // Note that the pool must be joined before the database is destroyed
    // (which happens after this destructor's body).
    //
    join ();
  }

  void pgsql_database::
  join ()
  {
    // Note that the operations already handed over to the pool are
    // performed (rather than abandoned) and that joining the pool repeatedly
    // is a noop.
    //
    transactions_->pool.join ();
  }

  uint64_t pgsql_database::
  schema_version ()
  try
  {
    return transactions_->database.schema_version ();
  }
  catch (const odb::exception& e)
  {
    throw database_error ("database error: {}", describe (e));
  }

  uint64_t pgsql_database::
  current_schema_version () const
  {
    return odb::schema_catalog::current_version (
      transactions_->database);
  }

  uint64_t pgsql_database::
  migrate ()
  try
  {
    odb::pgsql::database& db (transactions_->database);

    // Serialize the concurrent migrations.
    //
    // We hold the session lock on a dedicated connection and start the
    // transaction only once we have it. Note that taking the lock inside
    // the (serializable) transaction would not do: its snapshot would
    // predate the migration we waited for.
    //
    odb::pgsql::connection_ptr c (db.connection ());
    execute (*c, "SELECT pg_advisory_lock ({})", migration_lock);

    struct unlock
    {
      odb::pgsql::connection& c;

      ~unlock ()
      {
        // If this fails, then the connection is likely broken, in which
        // case the server releases the lock when the session ends.
        //
        try
        {
          execute (c, "SELECT pg_advisory_unlock ({})", migration_lock);
        }
        catch (const odb::exception&)
        {
        }
      }
    } u {*c};

    // Note that reading the schema version inside the transaction is safe
    // (the ODB runtime checks that the version table exists first).
    //
    odb::transaction t (c->begin ());

    const uint64_t v (db.schema_version ());
    const uint64_t cv (odb::schema_catalog::current_version (db));

    if (v != 0)
    {
      if (v < odb::schema_catalog::base_version (db))
        throw database_error (
          "database schema version {} is too old to migrate", v);

      if (v > cv)
        throw database_error (
          "database schema version {} is newer than {}", v, cv);
    }

    if (v == 0)
      odb::schema_catalog::create_schema (db, "", false /* drop */);
    else if (v != cv)
      odb::schema_catalog::migrate (db);

    t.commit ();
    return v;
  }
  catch (const odb::exception& e)
  {
    throw database_error ("database error: {}", describe (e));
  }

  // Return the timestamp as nanoseconds since the epoch and back.
  //
  static int64_t
  to_nanoseconds (timestamp t)
  {
    return chrono::duration_cast<chrono::nanoseconds> (
      t.time_since_epoch ()).count ();
  }

  static timestamp
  to_timestamp (int64_t ns)
  {
    return timestamp (
      chrono::duration_cast<duration> (chrono::nanoseconds (ns)));
  }

  // Return the followed user of the relationship entry.
  //
  static person
  to_person (const relationship_entry& e)
  {
    return person {e.target, e.favorite, e.following, to_timestamp (e.added)};
  }

  // pgsql_social_store
  //
  pgsql_social_store::
  pgsql_social_store (pgsql_database& d)
    : database_ (d)
  {
  }

  awaitable<people_page> pgsql_social_store::
  people (xuid o, relationship_view v, size_t start, size_t count)
  {
    co_return co_await database_.transactions_->execute (
      [o, v, start, count] (odb::database& db)
    {
      using query = odb::query<relationship_entry>;

      query q (query::forward::id.owner == o);

      switch (v)
      {
        case relationship_view::all:
          break;
        case relationship_view::favorite:
          q = q && query::forward::favorite;
          break;
        case relationship_view::legacy_friends:
          q = q && query::reverse::id.owner.is_not_null ();
          break;
      }

      // Select the whole list (ordered by the view) and take the range
      // while counting.
      //
      // Note that ODB has no notion of the result range (SQL LIMIT and
      // OFFSET) and we would rather not spell it in SQL. Fetching the whole
      // list costs little: it is bounded by the follow limit and PostgreSQL
      // results are always fetched completely anyway (see the ODB manual,
      // PostgreSQL Limitations).
      //
      people_page r;
      for (const relationship_entry& e: db.query<relationship_entry> (q))
      {
        if (r.total >= start && r.people.size () < count)
          r.people.push_back (to_person (e));

        ++r.total;
      }

      return r;
    });
  }

  awaitable<optional<person>> pgsql_social_store::
  find (xuid o, xuid u)
  {
    co_return co_await database_.transactions_->execute (
      [o, u] (odb::database& db) -> optional<person>
    {
      using query = odb::query<relationship_entry>;

      unique_ptr<relationship_entry> e (
        db.query_one<relationship_entry> (
          query::forward::id.owner == o &&
          query::forward::id.target == u));

      if (e == nullptr)
        return nullopt;

      return to_person (*e);
    });
  }

  awaitable<follow_result> pgsql_social_store::
  follow (xuid o, xuid u, timestamp now, size_t limit)
  {
    co_return co_await database_.transactions_->execute (
      [o, u, now, limit] (odb::database& db)
    {
      using count_query = odb::query<relationship_count>;

      const relationship_key k {o, u};

      if (db.find<relationship_record> (k) != nullptr)
        return follow_result::existing;

      const relationship_count c (
        db.query_value<relationship_count> (
          count_query::id.owner == o));

      if (c.result >= limit)
        return follow_result::full;

      db.persist (relationship_record {k, false, to_nanoseconds (now)});
      return follow_result::added;
    });
  }

  awaitable<bool> pgsql_social_store::
  unfollow (xuid o, xuid u)
  {
    co_return co_await database_.transactions_->execute (
      [o, u] (odb::database& db)
    {
      using query = odb::query<relationship_record>;

      return db.erase_query<relationship_record> (
        query::id.owner == o &&
        query::id.target == u) != 0;
    });
  }

  awaitable<bool> pgsql_social_store::
  favorite (xuid o, xuid u, bool f)
  {
    co_return co_await database_.transactions_->execute (
      [o, u, f] (odb::database& db)
    {
      unique_ptr<relationship_record> r (
        db.find<relationship_record> (
          relationship_key {o, u}));

      if (r == nullptr)
        return false;

      r->favorite = f;
      db.update (*r);
      return true;
    });
  }
}
