// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libxle/service.hxx>

namespace xle
{
  const char*
  to_string (http_method m) noexcept
  {
    switch (m)
    {
      case http_method::get:     return "GET";
      case http_method::post:    return "POST";
      case http_method::put:     return "PUT";
      case http_method::delete_: return "DELETE";
    }

    LIBOBE_UNREACHABLE ();
  }

  // service (vtable)
  //
  service::
  ~service () = default;
}
