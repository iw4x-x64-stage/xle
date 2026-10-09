// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <libxle/types.hxx>
#include <libxle/utility.hxx>

#include <libxle/social-service.hxx>

#include <libxle/export.hxx>

namespace xle
{
  // Verify the platform token (the Authorization header value) and return
  // the caller. Throw invalid_argument describing the problem if it is not
  // valid.
  //
  // In production this is obe::verify_xbl_token() with the audience of the
  // social service (see the xle executable).
  //
  using token_verifier = move_only_function<xuid (string_view token) const>;

  struct social_server_settings
  {
    // The largest request (headers and body) and the time the client has
    // to complete the TLS handshake and send it.
    //
    size_t   max_request_size = 8192;
    duration request_timeout = std::chrono::seconds (30);
  };

  // The social server.
  //
  // Serve the social service (see social_service) over HTTPS: verify the
  // platform token of each request and pass the request to the service. A
  // request without a valid token gets the 401 status, one that exceeds the
  // size limits 431 or 413, and one the store fails 503.
  //
  // The diagnostics (invalid tokens, refused requests, store failures) are
  // printed to stderr.
  //
  // The server, the TLS context, and the service should outlive the
  // connections, which means the io_context should be stopped (and its
  // handlers destroyed) before they are destroyed.
  //
  class LIBXLE_SYMEXPORT social_server
  {
  public:
    using tcp = boost::asio::ip::tcp;

    // Bind to the endpoint and start listening. Throw boost::system::
    // system_error on failure.
    //
    social_server (const boost::asio::any_io_executor&,
                   const tcp::endpoint&,
                   boost::asio::ssl::context&,
                   token_verifier,
                   social_service&,
                   social_server_settings = {});

    social_server (const social_server&) = delete;
    social_server& operator= (const social_server&) = delete;

    tcp::endpoint
    endpoint () const;

    // Accept the connections and serve them, each on its own coroutine,
    // until the acceptor is closed or the operation is cancelled.
    //
    boost::asio::awaitable<void>
    run ();

    // Stop accepting new connections.
    //
    void
    close ();

  private:
    boost::asio::awaitable<void>
    serve (tcp::socket);

    // Verify the token and handle the request with the service.
    //
    boost::asio::awaitable<service_reply>
    handle (const string& name,
            string_view token,
            string_view method,
            string_view target);

  private:
    tcp::acceptor                acceptor_;
    boost::asio::ssl::context&   tls_;
    const token_verifier         verify_;
    social_service&              service_;
    const social_server_settings settings_;
  };
}
