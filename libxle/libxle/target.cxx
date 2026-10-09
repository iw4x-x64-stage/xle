// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libxle/target.hxx>

#include <charconv> // from_chars()

using namespace std;

namespace xle
{
  // Return the value of the hexadecimal digit or -1 if it is not one.
  //
  static int
  hex_value (char c)
  {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  // Percent-decode the component, treating plus as space if requested.
  //
  static string
  decode (string_view s, bool plus)
  {
    string r;
    r.reserve (s.size ());

    for (size_t i (0); i != s.size (); ++i)
    {
      char c (s[i]);

      if (c == '%')
      {
        int h (i + 2 < s.size () ? hex_value (s[i + 1]) : -1);
        int l (h != -1 ? hex_value (s[i + 2]) : -1);

        if (l == -1)
          throw invalid_input ("invalid percent-encoding in request target");

        r += static_cast<char> (h * 16 + l);
        i += 2;
      }
      else if (c == '+' && plus)
        r += ' ';
      else
        r += c;
    }

    return r;
  }

  request_target::
  request_target (string_view t)
  {
    if (t.empty () || t[0] != '/')
      throw invalid_input ("request target must start with '/'");

    // Note that the fragment is never sent so a '#' is just invalid but we
    // let the path lookup reject such a target.
    //
    string_view p (t), q;
    if (size_t n = t.find ('?'); n != string_view::npos)
    {
      p = t.substr (0, n);
      q = t.substr (n + 1);
    }

    // Split the path skipping the leading slash.
    //
    for (size_t b (1);;)
    {
      size_t e (p.find ('/', b));
      path.push_back (decode (p.substr (b, e - b), false /* plus */));

      if (e == string_view::npos)
        break;

      b = e + 1;
    }

    // Split the query into the parameters, skipping the empty ones (for
    // example, in a&&b).
    //
    for (size_t b (0); b < q.size ();)
    {
      size_t e (q.find ('&', b));
      if (e == string_view::npos)
        e = q.size ();

      string_view a (q.substr (b, e - b));
      if (!a.empty ())
      {
        size_t n (a.find ('='));
        query.emplace_back (
          decode (a.substr (0, n), true /* plus */),
          n != string_view::npos ? decode (a.substr (n + 1), true) : string ());
      }

      b = e + 1;
    }
  }

  const string* request_target::
  parameter (string_view n) const
  {
    for (const pair<string, string>& p: query)
    {
      if (p.first == n)
        return &p.second;
    }

    return nullptr;
  }

  uint64_t
  parse_unsigned (string_view s, const char* what)
  {
    uint64_t r;
    const char* b (s.data ());
    const char* e (b + s.size ());

    // Note that from_chars() accepts neither the sign nor the spaces.
    //
    auto [p, ec] (from_chars (b, e, r));

    if (s.empty () || ec != errc () || p != e)
      throw invalid_input ("invalid {} '{}'", what, s);

    return r;
  }

  xuid
  parse_user (string_view s, xuid caller)
  {
    if (s == "me")
      return caller;

    static const string_view prefix ("xuid(");

    if (!s.starts_with (prefix) || !s.ends_with (')'))
      throw invalid_input ("invalid user '{}': expected xuid(<xuid>)", s);

    s.remove_prefix (prefix.size ());
    s.remove_suffix (1);

    return xuid {parse_unsigned (s, "xuid")};
  }
}
