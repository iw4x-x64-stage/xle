// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <print>
#include <string>
#include <cstdio>    // fflush(), stdout
#include <cstdint>
#include <csignal>   // SIGINT, SIGTERM
#include <sstream>
#include <iostream>
#include <exception>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>

#include <libobe/authenticator-xbl.hxx>

#include <libxle/version.hxx>
#include <libxle/pgsql.hxx>
#include <libxle/social-store.hxx>
#include <libxle/social-server.hxx>
#include <libxle/social-service.hxx>

#include <xle/version.hxx>
#include <xle/xle-options.hxx>

using namespace std;

namespace xle
{
  namespace asio = boost::asio;

  using asio::ip::tcp;

  namespace
  {
    // Thrown to terminate the process once the diagnostics has been issued.
    //
    class failed: public std::exception {};
  }

  // The io_context that can destroy its pending handlers (the suspended
  // coroutines) before it is destroyed itself.
  //
  // The handlers refer to the server and the service, which in turn have
  // the sockets and timers of the io_context. So we destroy the handlers
  // first, then the server and the service, and the io_context last (see
  // main()).
  //
  class context: public asio::io_context
  {
  public:
    using io_context::shutdown;
  };

  // The diagnostics verbosity (see --verbose).
  //
  static uint16_t verb (1);

  static int
  main (int argc, char* argv[])
  try
  {
    // Parse the command line, including the options files.
    //
    options o;
    {
      cli::argv_file_scanner s (argc, argv, "--options-file");
      o.parse (s);
    }

    // Handle --help and --version.
    //
    if (o.help ())
    {
      print_xle_usage (cout);
      return 0;
    }

    if (o.version ())
    {
      println ("xle {}\n"
               "libxle {}\n"
               "Copyright (c) the IW4x authors.\n"
               "This is free software released under the GNU General Public "
               "License, version 3,\n"
               "with the IW4x Linking Exception, version 1.1.",
               XLE_VERSION_ID,
               LIBXLE_VERSION_ID);
      return 0;
    }

    // Set the verbosity.
    //
    // Note that the more specific options override --verbose.
    //
    if (o.verbose () > 3)
    {
      println (cerr,
               "error: invalid --verbose value {}\n"
               "  info: specify a level between 0 and 3",
               o.verbose ());
      throw failed ();
    }

    verb = o.quiet () ? 0 :
           o.V ()     ? 3 :
           o.v ()     ? 2 :
           o.verbose ();

    // Verify the server configuration, unless migrating, before going any
    // further.
    //
    if (!o.migrate () &&
        (!o.tls_certificate_specified () || !o.tls_key_specified ()))
    {
      println (cerr,
               "error: TLS certificate and key are required\n"
               "  info: specify them with --tls-certificate and --tls-key");
      throw failed ();
    }

    tcp::endpoint ep;
    if (!o.migrate ())
    {
      boost::system::error_code ec;
      const asio::ip::address a (asio::ip::make_address (o.address (), ec));

      if (ec)
      {
        println (cerr, "error: invalid address '{}'", o.address ());
        throw failed ();
      }

      ep = tcp::endpoint (a, o.port ());
    }

    // Load the TLS certificate and key.
    //
    asio::ssl::context tls (asio::ssl::context::tls_server);
    if (!o.migrate ())
    {
      try
      {
        tls.use_certificate_chain_file (o.tls_certificate ());
        tls.use_private_key_file (o.tls_key (), asio::ssl::context::pem);
      }
      catch (const boost::system::system_error& e)
      {
        println (cerr, "error: unable to load TLS certificate and key: {}",
                 e.what ());
        throw failed ();
      }
    }

    // Open the database.
    //
    // Note that the connection is only established on the first database
    // access.
    //
    if (!o.db_name_specified ())
    {
      println (cerr,
               "error: no database specified\n"
               "  info: specify it with --db-name");
      throw failed ();
    }

    if (o.db_max_connections () == 0)
    {
      println (cerr, "error: invalid --db-max-connections value 0");
      throw failed ();
    }

    pgsql_settings s;
    s.user            = o.db_user ();
    s.password        = o.db_password ();
    s.name            = o.db_name ();
    s.host            = o.db_host ();
    s.port            = o.db_port ();
    s.max_connections = o.db_max_connections ();
    s.retry           = o.db_retry ();

    pgsql_database db (s);

    // If requested, migrate the database schema and exit. Otherwise, make
    // sure the schema is current: the server of one version should not touch
    // the schema of another.
    //
    const string& n (o.db_name ());
    const uint64_t cv (db.current_schema_version ());

    if (o.migrate ())
    {
      uint64_t v;
      try
      {
        v = db.migrate ();
      }
      catch (const database_error& e)
      {
        println (cerr, "error: unable to migrate database {}: {}",
                 n, e.what ());
        throw failed ();
      }

      if (verb >= 2)
      {
        if (v == 0)
          println (cerr, "created database {} schema version {}", n, cv);
        else if (v != cv)
          println (cerr,
                   "migrated database {} schema from version {} to {}",
                   n, v, cv);
        else
          println (cerr, "database {} schema version {} is current", n, cv);
      }

      return 0;
    }

    {
      uint64_t v;
      try
      {
        v = db.schema_version ();
      }
      catch (const database_error& e)
      {
        println (cerr, "error: unable to access database {}: {}",
                 n, e.what ());
        throw failed ();
      }

      if (v != cv)
      {
        if (v == 0)
          println (cerr, "error: database {} has no schema", n);
        else
          println (cerr,
                   "error: database {} schema version {} instead of {}",
                   n, v, cv);

        println (cerr, "  info: run 'xle --migrate' to create or migrate it");
        throw failed ();
      }
    }

    // Verify the Xbox Live style tokens that IW4x issues for the services
    // (see obe::verify_xbl_token() for their form). Note that the gamertag
    // is the user name of the identity.
    //
    auto verify = [] (string_view t, string_view a)
    {
      obe::xbl_settings xs;
      xs.audience = a;

      obe::auth_identity id (
        obe::verify_xbl_token (t, system_clock::now (), xs));

      return caller {xuid {to_underlying (id.user)}, move (id.user_name)};
    };

    // Create the io_context. Note that it must outlive the server and the
    // service (see context for details).
    //
    context ctx;

    pgsql_social_store store (db);
    social_service social (store);

    // Start the server.
    //
    optional<social_server> server;
    try
    {
      server.emplace (ctx.get_executor (),
                      ep,
                      tls,
                      move (verify),
                      vector<reference_wrapper<service>> {social});
    }
    catch (const boost::system::system_error& e)
    {
      println (cerr, "error: unable to listen: {}", e.what ());
      throw failed ();
    }

    if (verb >= 2)
    {
      ostringstream os;
      os << server->endpoint ();
      println (cerr, "listening on {}", os.str ());
    }

    // Flush so that whoever reads the endpoint gets it right away.
    //
    if (o.print_endpoint ())
    {
      ostringstream os;
      os << server->endpoint ();
      println ("{}", os.str ());
      fflush (stdout);
    }

    // Serve until the server fails or a termination signal arrives.
    //
    int r (0);
    {
      asio::co_spawn (ctx, server->run (), [&ctx, &r] (exception_ptr e)
      {
        if (e == nullptr)
          return;

        try
        {
          rethrow_exception (e);
        }
        catch (const std::exception& x)
        {
          println (cerr, "error: server failed: {}", x.what ());
        }

        r = 1;
        ctx.stop ();
      });

      asio::signal_set ss (ctx, SIGINT, SIGTERM);
      ss.async_wait ([&ctx] (const boost::system::error_code& ec, int)
      {
        if (ec)
          return;

        if (verb >= 2)
          println (cerr, "shutting down");

        ctx.stop ();
      });

      ctx.run ();
    }

    // Shut down.
    //
    // First wait for the database operations in progress since their
    // completions are posted to the io_context. Then destroy the pending
    // handlers and with them the coroutines of the connections that are
    // still open, which refer to the server and the service.
    //
    db.join ();
    ctx.shutdown ();

    return r;
  }
  catch (const failed&)
  {
    return 1; // Diagnostics already issued.
  }
  catch (const cli::exception& e)
  {
    ostringstream os;
    os << e;

    println (cerr,
             "error: {}\n"
             "  info: run 'xle --help' for more information",
             os.str ());
    return 1;
  }
}

int
main (int argc, char* argv[])
{
  return xle::main (argc, argv);
}
