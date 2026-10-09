// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <format>
#include <memory>    // make_shared(), make_unique()
#include <string>    // to_string()
#include <utility>   // move(), forward(), declval(), make_pair(),
                     // to_underlying()
#include <iterator>  // make_move_iterator()
#include <algorithm> // *

#include <libobe/utility.hxx>  // invalid_input, formattable_arguments
#include <libobe/contract.hxx> // LIBOBE_PRE(), LIBOBE_ASSERT(), etc.

#include <libxle/types.hxx>

namespace xle
{
  using std::move;
  using std::forward;
  using std::declval;

  using std::make_pair;
  using std::make_shared;
  using std::make_unique;
  using std::make_move_iterator;
  using std::to_string;
  using std::to_underlying;

  // Invalid external input (a request, a token, etc). The description is
  // formatted from the arguments, for example:
  //
  // throw invalid_input ("invalid {}: object expected", what);
  //
  using obe::invalid_input;
  using obe::formattable_arguments;

  // The store failure (the database is unavailable, etc), which the
  // services report as a transient server error.
  //
  class store_error: public runtime_error
  {
  public:
    template <typename... A>
      requires formattable_arguments<A...>
    explicit
    store_error (std::format_string<A...> f, A&&... a)
      : runtime_error (std::format (f, std::forward<A> (a)...)) {}
  };
}

#include <libxle/version.hxx>
