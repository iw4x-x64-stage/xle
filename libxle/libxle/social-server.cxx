// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libxle/social-server.hxx>

#include <print>
#include <cstdio> // stderr
#include <algorithm> // find_if()

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <libobe/endpoint.hxx>

using namespace std;

namespace xle
{
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;

  using asio::awaitable;
  using asio::use_awaitable;
  using asio::ip::tcp;

  // Return the method or nullopt if it is not one the services serve.
  //
  static optional<http_method>
  to_method (http::verb v)
  {
    static const pair<http::verb, http_method> methods[]
    {
      {http::verb::get,     http_method::get},
      {http::verb::post,    http_method::post},
      {http::verb::put,     http_method::put},
      {http::verb::delete_, http_method::delete_}
    };

    for (const auto& [x, m]: methods)
    {
      if (x == v)
        return m;
    }

    return nullopt;
  }

  // The delay before accepting again after a failure.
  //
  static const chrono::seconds accept_retry_delay (1);

  social_server::
  social_server (const asio::any_io_executor& ex,
                 const tcp::endpoint& ep,
                 asio::ssl::context& t,
                 token_verifier v,
                 vector<reference_wrapper<service>> s,
                 caller_observer o,
                 social_server_settings ss)
    : acceptor_ (ex, ep),
      tls_ (t),
      verify_ (move (v)),
      services_ (move (s)),
      observe_ (move (o)),
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
          http_method m,
          const request_target& t,
          string_view body)
  {
    // Find the service that serves the target.
    //
    auto i (ranges::find_if (services_, [&t] (const service& s)
    {
      return s.match (t);
    }));

    if (i == services_.end ())
    {
      println (stderr, "{}: warning: unknown target", n);
      co_return service_reply {404, string (), string ()};
    }

    service& s (*i);

    // Verify the token for the service's audience.
    //
    optional<caller> c;
    try
    {
      c = verify_ (token, s.audience ());
    }
    catch (const invalid_argument& e)
    {
      println (stderr, "{}: warning: invalid platform token: {}", n, e.what ());
      co_return service_reply {401, string (), string ()};
    }

    // Let the observer see the caller and handle the request. Note that we
    // cannot co_await in the handler.
    //
    optional<service_reply> r;
    try
    {
      if (observe_ != nullptr)
        co_await observe_ (*c);

      r = co_await s.handle (*c, m, t, body);
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
               n, to_underlying (c->user), r->error);

    co_return move (*r);
  }

  awaitable<void> social_server::
  serve (tcp::socket s)
  {
    string n;
    {
      boost::system::error_code ec;
      const tcp::endpoint e (s.remote_endpoint (ec));

      n = ec ? string ("<unknown>") : format ("{}", e);
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

      // Map the method and parse the target, and handle the request.
      //
      service_reply r {501, string (), string ()};

      if (const optional<http_method> m = to_method (rq.method ()))
      {
        optional<request_target> t;
        try
        {
          t.emplace (rq.target ());
        }
        catch (const invalid_argument& e)
        {
          println (stderr, "{}: warning: {}", n, e.what ());
          r.status = 400;
        }

        if (t)
          r = co_await handle (n,
                               rq[http::field::authorization],
                               *m,
                               *t,
                               rq.body ());
      }
      else
        println (stderr,
                 "{}: warning: method {} not implemented",
                 n, string (rq.method_string ()));

      // Prepare the response.
      //
      http::response<http::string_body> rs;
      rs.version (rq.version ());
      rs.keep_alive (rq.keep_alive ());
      rs.result (r.status);
      rs.set (http::field::server, "xle");

      if (r.status == 405)
      {
        string a;
        for (http_method x: r.allow)
        {
          if (!a.empty ())
            a += ", ";

          a += to_string (x);
        }

        rs.set (http::field::allow, a);
      }

      if (r.status == 200 && !r.body.empty ())
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
