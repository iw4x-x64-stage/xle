// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <string>
#include <vector>
#include <print>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <iostream>
#include <exception>
#include <stdexcept> // invalid_argument

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/ssl/context.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <libxle/social-store.hxx>
#include <libxle/social-server.hxx>
#include <libxle/social-service.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace xle;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using asio::awaitable;
using asio::use_awaitable;
using asio::ip::tcp;

// The memory store that fails for user 666.
//
class failing_store: public memory_social_store
{
public:
  virtual awaitable<people_page>
  people (xuid o, relationship_view v, size_t start, size_t count) override
  {
    if (o == xuid {666})
      throw store_error ("database unavailable");

    co_return co_await memory_social_store::people (o, v, start, count);
  }
};

// Usage: argv[0] <certificate> <key>
//
// Run the social server on the loopback interface with the certificate and
// key, user 1 following user 2, read requests from stdin, one per line,
// send each over a new connection, and print the response:
//
// <method> <target> <token>
//
// The server accepts the tokens of the form 'user:<xuid>' and rejects any
// other, and its store fails for user 666. A token of the form '*<n>'
// stands for n 'x' characters (to test the size limit).
//
// The response is printed as the HTTP status code followed, for the 200
// status, by the content type and the body and, for the 405 status, by the
// allowed methods. The server's diagnostics go to stderr.
//
int
main (int argc, char* argv[])
{
  if (argc != 3)
  {
    println (cerr, "usage: {} <certificate> <key>", argv[0]);
    return 1;
  }

  asio::ssl::context stls (asio::ssl::context::tls_server);
  stls.use_certificate_chain_file (argv[1]);
  stls.use_private_key_file (argv[2], asio::ssl::context::pem);

  asio::ssl::context ctls (asio::ssl::context::tls_client);
  ctls.set_verify_mode (asio::ssl::verify_none);

  auto verify = [] (string_view t)
  {
    if (!t.starts_with ("user:"))
      throw invalid_argument ("not a test token");

    return xuid {stoull (string (t.substr (5)))};
  };

  failing_store store;
  social_service service (store);

  asio::io_context ctx;

  // Set up the relationships.
  //
  asio::co_spawn (
    ctx,
    [&store] () -> awaitable<void>
    {
      const follow_result r (
        co_await store.follow (xuid {1},
                               xuid {2},
                               timestamp (chrono::seconds (1)),
                               100));
      assert (r == follow_result::added);
    },
    asio::detached);

  ctx.run ();
  ctx.restart ();

  social_server server (ctx.get_executor (),
                        tcp::endpoint (asio::ip::address_v4::loopback (), 0),
                        stls,
                        verify,
                        service);

  asio::co_spawn (ctx, server.run (), asio::detached);

  vector<string> requests;
  for (string l; getline (cin, l); )
    requests.push_back (move (l));

  // Send the requests.
  //
  auto play = [&server, &ctls, &requests] () -> awaitable<void>
  {
    for (const string& l: requests)
    {
      istringstream is (l);
      string method, target, token;
      is >> method >> target >> token;

      if (token.size () > 1 && token[0] == '*')
        token = string (stoul (token.substr (1)), 'x');

      beast::ssl_stream<beast::tcp_stream> s (
        co_await asio::this_coro::executor, ctls);

      co_await beast::get_lowest_layer (s).async_connect (server.endpoint (),
                                                          use_awaitable);
      co_await s.async_handshake (asio::ssl::stream_base::client,
                                  use_awaitable);

      http::request<http::string_body> rq (http::string_to_verb (method),
                                           target,
                                           11);
      rq.set (http::field::host, "social.xboxlive.com");
      rq.set (http::field::authorization, token);
      rq.keep_alive (false);
      rq.prepare_payload ();

      co_await http::async_write (s, rq, use_awaitable);

      beast::flat_buffer b;
      http::response<http::string_body> rs;
      co_await http::async_read (s, b, rs, use_awaitable);

      print ("{}", rs.result_int ());

      if (rs.result () == http::status::ok)
        print (" {} {}", string (rs[http::field::content_type]), rs.body ());
      else if (rs.result () == http::status::method_not_allowed)
        print (" {}", string (rs[http::field::allow]));

      println ();

      co_await s.async_shutdown (asio::as_tuple (use_awaitable));
    }
  };

  int r (0);
  asio::co_spawn (ctx, play (), [&server, &r] (exception_ptr e)
  {
    if (e)
    {
      try
      {
        rethrow_exception (e);
      }
      catch (const std::exception& x)
      {
        println (cerr, "error: {}", x.what ());
      }
      r = 1;
    }

    server.close ();
  });

  ctx.run ();
  return r;
}
