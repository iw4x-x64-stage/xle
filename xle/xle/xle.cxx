// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <print>
#include <string>
#include <cstdint>
#include <sstream>
#include <iostream>
#include <exception>

#include <libxle/version.hxx>
#include <libxle/pgsql.hxx>

#include <xle/version.hxx>
#include <xle/xle-options.hxx>

using namespace std;

namespace xle
{
  namespace
  {
    // Thrown to terminate the process once the diagnostics has been issued.
    //
    class failed: public std::exception {};
  }

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

    // Figure out what to do. For now the only operation is the database
    // schema migration since there are no services to serve yet.
    //
    if (!o.migrate ())
    {
      println (cerr,
               "error: nothing to do\n"
               "  info: specify --migrate to create or migrate the database "
               "schema");
      throw failed ();
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

    // Migrate the database schema.
    //
    const string& n (o.db_name ());
    const uint64_t cv (db.current_schema_version ());

    uint64_t v;
    try
    {
      v = db.migrate ();
    }
    catch (const database_error& e)
    {
      println (cerr, "error: unable to migrate database {}: {}", n, e.what ());
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
