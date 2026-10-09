// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libxle/social-server.hxx>

#include <print>
#include <cstdio> // stderr
#include <sstream>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

using namespace std;

namespace xle
{
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;

  using asio::awaitable;
  using asio::use_awaitable;
  using asio::ip::tcp;

  // The delay before accepting again after a failure.
  //
  static const chrono::seconds accept_retry_delay (1);

  social_server::
  social_server (const asio::any_io_executor& ex,
                 const tcp::endpoint& ep,
                 asio::ssl::context& t,
                 token_verifier v,
                 social_service& s,
                 social_server_settings ss)
    : acceptor_ (ex, ep),
      tls_ (t),
      verify_ (move (v)),
      service_ (s),
      settings_ (move (ss))
  {
  }

  tcp::endpoint social_server::
  endpoint () const
  {
    return acceptor_.local_endpoint ();
  }

  void social_server::
  close ()
  {
    boost::system::error_code ec;
    acceptor_.close (ec);
  }

  awaitable<service_reply> social_server::
  handle (const string& n,
          string_view token,
          string_view method,
          string_view target)
  {
    // Verify the token.
    //
    optional<xuid> caller;
    try
    {
      caller = verify_ (token);
    }
    catch (const invalid_argument& e)
    {
      println (stderr, "{}: warning: invalid platform token: {}", n, e.what ());
      co_return service_reply {401, string (), string ()};
    }

    // Handle the request. Note that we cannot co_await in the handler.
    //
    optional<service_reply> r;
    try
    {
      r = co_await service_.handle (*caller, method, target);
    }
    catch (const store_error& e)
    {
      println (stderr, "{}: error: {}", n, e.what ());
    }

    if (!r)
      co_return service_reply {503, string (), string ()};

    if (r->status != 200)
      println (stderr,
               "{}: warning: user {}: {}",
               n, to_underlying (*caller), r->error);

    co_return move (*r);
  }

  awaitable<void> social_server::
  serve (tcp::socket s)
  {
    string n;
    {
      boost::system::error_code ec;
      const tcp::endpoint e (s.remote_endpoint (ec));

      ostringstream os;
      if (ec)
        os << "<unknown>";
      else
        os << e;
      n = os.str ();
    }

    beast::ssl_stream<beast::tcp_stream> ts (move (s), tls_);

    // Each operation (the TLS handshake, the request read, the response
    // write) has the same time limit.
    //
    beast::get_lowest_layer (ts).expires_after (settings_.request_timeout);
    co_await ts.async_handshake (asio::ssl::stream_base::server,
                                 use_awaitable);

    beast::flat_buffer b;
    for (;;)
    {
      // Read the request.
      //
      http::request_parser<http::string_body> p;
      p.header_limit (static_cast<uint32_t> (settings_.max_request_size));
      p.body_limit (settings_.max_request_size);

      beast::get_lowest_layer (ts).expires_after (settings_.request_timeout);

      auto [ec, _] (
        co_await http::async_read (ts, b, p, asio::as_tuple (use_awaitable)));

      if (ec == http::error::end_of_stream)
        break;

      // Reply to a request that exceeds the size limits and close the
      // connection since we cannot resynchronize with the rest of the
      // request.
      //
      if (ec == http::error::header_limit || ec == http::error::body_limit)
      {
        println (stderr, "{}: warning: request too large", n);

        http::response<http::string_body> rs (
          ec == http::error::header_limit
          ? http::status::request_header_fields_too_large
          : http::status::payload_too_large,
          11 /* HTTP/1.1 */);

        rs.set (http::field::server, "xle");
        rs.keep_alive (false);
        rs.prepare_payload ();

        beast::get_lowest_layer (ts).expires_after (settings_.request_timeout);
        co_await http::async_write (ts, rs, use_awaitable);
        break;
      }

      if (ec)
        throw boost::system::system_error (ec);

      const http::request<http::string_body>& rq (p.get ());

      // Handle the request and prepare the response.
      //
      const service_reply r (
        co_await handle (n,
                         rq[http::field::authorization],
                         rq.method_string (),
                         rq.target ()));

      http::response<http::string_body> rs;
      rs.version (rq.version ());
      rs.keep_alive (rq.keep_alive ());
      rs.result (r.status);
      rs.set (http::field::server, "xle");

      // The service only serves GET (see social_service).
      //
      if (r.status == 405)
        rs.set (http::field::allow, "GET");

      if (r.status == 200)
      {
        rs.set (http::field::content_type, "application/json");
        rs.body () = r.body;
      }

      rs.prepare_payload ();

      beast::get_lowest_layer (ts).expires_after (settings_.request_timeout);
      co_await http::async_write (ts, rs, use_awaitable);

      if (!rs.keep_alive ())
        break;
    }

    // Shut down TLS gracefully. The client may just drop the connection, so
    // ignore any errors.
    //
    beast::get_lowest_layer (ts).expires_after (settings_.request_timeout);
    co_await ts.async_shutdown (asio::as_tuple (use_awaitable));
  }

  awaitable<void> social_server::
  run ()
  {
    for (;;)
    {
      auto [ec, s] (
        co_await acceptor_.async_accept (asio::as_tuple (use_awaitable)));

      if (ec == asio::error::operation_aborted)
        co_return;

      // Accept can fail for transient reasons (for example, out of file
      // descriptors), so we diagnose and carry on. But not right away: such
      // a failure is normally immediate and would repeat until the condition
      // clears, so retrying straight away would spin and flood stderr.
      //
      if (ec)
      {
        println (stderr,
                 "error: unable to accept connection: {}",
                 ec.message ());

        asio::steady_timer t (acceptor_.get_executor (), accept_retry_delay);
        co_await t.async_wait (asio::as_tuple (use_awaitable));

        // Bail out if we were closed while waiting.
        //
        if (!acceptor_.is_open ())
          co_return;

        continue;
      }

      auto report = [] (exception_ptr e)
      {
        if (!e)
          return;

        // Network and TLS errors are routine (the client went away, etc),
        // so we keep quiet about them.
        //
        try
        {
          rethrow_exception (e);
        }
        catch (const boost::system::system_error&)
        {
        }
        catch (const std::exception& x)
        {
          println (stderr, "error: connection: {}", x.what ());
        }
      };

      asio::co_spawn (acceptor_.get_executor (), serve (move (s)), report);
    }
  }
}
