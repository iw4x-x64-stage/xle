// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <boost/asio/awaitable.hpp>

#include <libxle/types.hxx>
#include <libxle/utility.hxx>

#include <libxle/social-store.hxx>

#include <libxle/export.hxx>

namespace xle
{
  struct social_settings
  {
    // The most people a reply lists (maxItems is capped at it).
    //
    size_t max_items = 1000;
  };

  // The reply to a service request: the HTTP status code and, for the 200
  // status, the JSON body. For the other statuses the error describes the
  // problem (for the diagnostics, it is not sent).
  //
  struct service_reply
  {
    uint16_t status;
    string   body;
    string   error;
  };

  // The social relationships service (social.xboxlive.com).
  //
  // Serve the caller's people list:
  //
  // GET /users/{user}/people[?view=<view>&startIndex=<n>&maxItems=<n>]
  //
  // Where {user} is xuid(<decimal>) and must be the caller (see
  // parse_user()), and the query parameters are:
  //
  // view        All (the default), Favorite, or LegacyXboxLiveFriends (see
  //             relationship_view).
  // startIndex  The index of the first person to list, 0 by default.
  // maxItems    The most people to list, max_items by default and at most.
  //
  // The reply lists the people in the order of the store (see
  // social_store::people()):
  //
  // {"totalCount": <n>,
  //  "people": [{"xuid": "<decimal>",
  //              "isFavorite": <bool>,
  //              "isFollowingCaller": <bool>,
  //              "socialNetworks": ["LegacyXboxLive"]}, ...]}
  //
  // Where totalCount is the number of people matching the view and
  // socialNetworks names LegacyXboxLive for the friends (the people who
  // follow the caller back) and is empty otherwise.
  //
  // A request for another target gets the 404 status, for another method
  // 405, with an invalid user or query 400, and for the list of another user
  // 403 (the title only ever asks for its own).
  //
  class LIBXLE_SYMEXPORT social_service
  {
  public:
    // The store should outlive the service.
    //
    explicit
    social_service (social_store&, social_settings = {});

    // Handle the request of the authenticated caller. The target is in the
    // origin form (see request_target). Throw store_error if the store
    // fails.
    //
    boost::asio::awaitable<service_reply>
    handle (xuid caller, string_view method, string_view target);

    social_service (const social_service&) = delete;
    social_service& operator= (const social_service&) = delete;

  private:
    social_store&         store_;
    const social_settings settings_;
  };
}
