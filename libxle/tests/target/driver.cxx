// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <string>
#include <print>
#include <utility>   // to_underlying()
#include <iostream>
#include <optional>
#include <stdexcept> // invalid_argument

#include <libxle/target.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace xle;

// Usage: argv[0]
//
// Read commands from stdin, one per line, and print the results to stdout:
//
// target <target>  Parse the request target and print its path segments
//                  on one line and query parameters on the next, each
//                  enclosed in [].
// user <segment>   Parse the user path segment and print the XUID.
//
// On invalid input print 'invalid: <description>' and carry on.
//
int
main ()
{
  for (string l; getline (cin, l); )
  {
    size_t n (l.find (' '));
    string c (l.substr (0, n));
    string a (n != string::npos ? l.substr (n + 1) : string ());

    try
    {
      if (c == "target")
      {
        request_target t (a);

        print ("path");
        for (const string& s: t.path)
          print (" [{}]", s);
        println ();

        print ("query");
        for (const auto& [k, v]: t.query)
          print (" [{}]=[{}]", k, v);
        println ();

        if (!t.query.empty ())
        {
          optional<const string&> v (t.parameter (t.query.front ().first));
          assert (v && *v == t.query.front ().second);
        }

        assert (!t.parameter ("absent"));
      }
      else if (c == "user")
      {
        println ("{}", to_underlying (parse_user (a)));
      }
      else
      {
        println (cerr, "unknown command '{}'", c);
        return 1;
      }
    }
    catch (const invalid_argument& e)
    {
      println ("invalid: {}", e.what ());
    }
  }
}
