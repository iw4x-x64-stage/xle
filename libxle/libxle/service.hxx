// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <boost/asio/awaitable.hpp>

#include <libxle/types.hxx>
#include <libxle/utility.hxx>

#include <libxle/target.hxx>

#include <libxle/export.hxx>

namespace xle
{
  // The request methods that the services serve.
  //
  enum class http_method
  {
    get,
    post,
    put,
    delete_
  };

  LIBXLE_SYMEXPORT const char*
  to_string (http_method) noexcept;

  // The authenticated caller (see the platform tokens in
  // libobe/authenticator-xbl.hxx).
  //
  struct caller
  {
    xuid   user;
    string gamertag;
  };

  // The reply to a service request: the HTTP status code and, for the 200
  // status, the JSON body (empty if the reply has none). For the other
  // statuses the error describes the problem (for the diagnostics, it is
  // not sent) and, for the 405 status, allow lists the allowed methods.
  //
  struct service_reply
  {
    uint16_t            status;
    string              body;
    string              error;
    vector<http_method> allow = {};
  };

  // A service that the server routes the requests to (see social_server).
  //
  // The clients reach every service at one address, so the server routes a
  // request by its path, which the services keep disjoint, rather than by
  // its host. A request is authenticated with a platform token for the
  // service's audience: the origin of the Xbox Live service the client
  // requests the token for.
  //
  // The services are shared by all the connections and run on the
  // io_context thread (see social_server).
  //
  class LIBXLE_SYMEXPORT service
  {
  public:
    // Return the audience of the tokens the service accepts, for example,
    // https://social.xboxlive.com.
    //
    virtual string_view
    audience () const noexcept = 0;

    // Return true if the target's path is one the service serves.
    //
    virtual bool
    match (const request_target&) const = 0;

    // Handle the request of the authenticated caller for a target that the
    // service matched. Throw store_error if the store fails.
    //
    virtual boost::asio::awaitable<service_reply>
    handle (const caller&,
            http_method,
            const request_target&,
            string_view body) = 0;

    service () = default;

    virtual
    ~service ();

    service (const service&) = delete;
    service& operator= (const service&) = delete;
  };

  // Return the error reply with the status and the error formatted from the
  // arguments.
  //
  template <formattable_argument... A>
  inline service_reply
  service_error (uint16_t status, std::format_string<A...> f, A&&... a)
  {
    return service_reply {status,
                          string (),
                          std::format (f, std::forward<A> (a)...)};
  }

  // Return the reply to a request with a method the target doesn't allow.
  //
  inline service_reply
  service_method_error (http_method m, vector<http_method> allow)
  {
    return service_reply {405,
                          string (),
                          std::format ("method {} not allowed", to_string (m)),
                          move (allow)};
  }
}
