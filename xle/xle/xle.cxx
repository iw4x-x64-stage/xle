// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <print>
#include <format>
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

#include <libbde/endpoint.hxx>
#include <libbde/authenticator-xbl.hxx>

#include <libxle/version.hxx>
#include <libxle/pgsql.hxx>
#include <libxle/diagnostics.hxx>
#include <libxle/social-store.hxx>
#include <libxle/social-server.hxx>
#include <libxle/service-social.hxx>

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
      LOG_ERROR (logger (),
                 "error: invalid --verbose value {}\n"
                 "  info: specify a level between 0 and 3",
                 o.verbose ());
      throw failed ();
    }

    set_verbosity (o.quiet () ? 0 :
                   o.V ()     ? 3 :
                   o.v ()     ? 2 :
                   o.verbose ());

    // Verify the server configuration, unless migrating, before going any
    // further.
    //
    if (!o.migrate () &&
        (!o.tls_certificate_specified () || !o.tls_key_specified ()))
    {
      LOG_ERROR (logger (),
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
        LOG_ERROR (logger (), "error: invalid address '{}'", o.address ());
        throw failed ();
      }

      ep = tcp::endpoint (a, o.port ());
    }

    // Load the TLS certificate and key.
    //
    // Note that here and below we report the error code's message rather
    // than what(), which also carries the failed function and the source
    // location.
    //
    asio::ssl::context tls (asio::ssl::context::tls_server);
    if (!o.migrate ())
    {
      boost::system::error_code ec;

      tls.use_certificate_chain_file (o.tls_certificate (), ec);
      if (ec)
      {
        LOG_ERROR (logger (), "error: unable to load TLS certificate {}: {}",
                   o.tls_certificate (), ec.message ());
        throw failed ();
      }

      tls.use_private_key_file (o.tls_key (), asio::ssl::context::pem, ec);
      if (ec)
      {
        LOG_ERROR (logger (), "error: unable to load TLS key {}: {}",
                   o.tls_key (), ec.message ());
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
      LOG_ERROR (logger (),
                 "error: no database specified\n"
                 "  info: specify it with --db-name");
      throw failed ();
    }

    if (o.db_max_connections () == 0)
    {
      LOG_ERROR (logger (), "error: invalid --db-max-connections value 0");
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
        LOG_ERROR (logger (), "error: unable to migrate database {}: {}",
                   n, e.what ());
        throw failed ();
      }

      if (v == 0)
        LOG_INFO (logger (), "created database {} schema version {}", n, cv);
      else if (v != cv)
        LOG_INFO (logger (),
                  "migrated database {} schema from version {} to {}",
                  n, v, cv);
      else
        LOG_INFO (logger (),
                  "database {} schema version {} is current",
                  n, cv);

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
        LOG_ERROR (logger (), "error: unable to access database {}: {}",
                   n, e.what ());
        throw failed ();
      }

      if (v != cv)
      {
        if (v == 0)
          LOG_ERROR (logger (),
                     "error: database {} has no schema\n"
                     "  info: run 'xle --migrate' to create or migrate it",
                     n);
        else
          LOG_ERROR (logger (),
                     "error: database {} schema version {} instead of {}\n"
                     "  info: run 'xle --migrate' to create or migrate it",
                     n, v, cv);

        throw failed ();
      }
    }

    // Verify the Xbox Live style tokens that IW4x issues for the services
    // (see bde::verify_xbl_token() for their form). The verification lives
    // in libbde because bde's Demonware authentication server uses it first
    // and xle depends on libbde, which also tests it. Note that the gamertag
    // is the user name of the identity.
    //
    auto verify = [] (string_view t, string_view a)
    {
      bde::xbl_settings xs;
      xs.audience = a;

      bde::auth_identity id (
        bde::verify_xbl_token (t, system_clock::now (), xs));

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
      LOG_ERROR (logger (),
                 "error: unable to listen on {}: {}",
                 format ("{}", ep), e.code ().message ());
      throw failed ();
    }

    LOG_INFO (logger (), "listening on {}", format ("{}", server->endpoint ()));

    // Flush so that whoever reads the endpoint gets it right away.
    //
    if (o.print_endpoint ())
    {
      println ("{}", server->endpoint ());
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
          LOG_ERROR (logger (), "error: server failed: {}", x.what ());
        }

        r = 1;
        ctx.stop ();
      });

      asio::signal_set ss (ctx, SIGINT, SIGTERM);
      ss.async_wait ([&ctx] (const boost::system::error_code& ec, int)
      {
        if (ec)
          return;

        LOG_INFO (logger (), "shutting down");

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

    LOG_ERROR (logger (),
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
