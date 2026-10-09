// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <libxle/types.hxx>
#include <libxle/utility.hxx>

#include <libxle/export.hxx>

namespace xle
{
  // The request target in the origin form (RFC 9112, Section 3.2.1), for
  // example:
  //
  // /users/xuid(2533274790395904)/people?view=All&maxItems=100
  //
  // The path is split into segments and the query into parameters, all of
  // them percent-decoded (RFC 3986, Section 2.1). In the query a plus
  // stands for a space (the form encoding) and a parameter without a value
  // has the empty value.
  //
  class LIBXLE_SYMEXPORT request_target
  {
  public:
    // Throw invalid_input if the target is not in the origin form or is
    // incorrectly percent-encoded.
    //
    explicit
    request_target (string_view);

    // The path segments, without the empty leading segment. A trailing
    // slash results in the empty trailing segment.
    //
    strings path;

    // The query parameters in the order of appearance.
    //
    vector<pair<string, string>> query;

    // Return the value of the first parameter with the specified name or
    // nullptr if there is none.
    //
    const string*
    parameter (string_view name) const;
  };

  // Return the user of the path segment, which is either xuid(<decimal>) or
  // me, which stands for the caller. Throw invalid_input if it is neither.
  //
  LIBXLE_SYMEXPORT xuid
  parse_user (string_view segment, xuid caller);

  // Return the unsigned decimal integer. Throw invalid_input describing the
  // value as what if it is not one or is out of range.
  //
  LIBXLE_SYMEXPORT uint64_t
  parse_unsigned (string_view, const char* what);
}
