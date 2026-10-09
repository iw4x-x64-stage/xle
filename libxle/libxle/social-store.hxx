// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <map>
#include <ranges> // subrange

#include <boost/asio/awaitable.hpp>

#include <libxle/types.hxx>
#include <libxle/utility.hxx>

#include <libxle/export.hxx>

namespace xle
{
  using boost::asio::awaitable;

  // The social relationship filter (the view query parameter, see
  // XblSocialRelationshipFilter).
  //
  enum class relationship_view
  {
    all,           // All
    favorite,      // Favorite
    legacy_friends // LegacyXboxLiveFriends (the friends)
  };

  LIBXLE_SYMEXPORT const char*
  to_string (relationship_view) noexcept;

  // Throw invalid_input if the value is not a filter name.
  //
  LIBXLE_SYMEXPORT relationship_view
  to_relationship_view (string_view);

  // The followed user (an entry of the people list).
  //
  struct person
  {
    xuid      user {};
    bool      favorite = false;
    bool      following_caller = false; // The user follows the list owner.
    timestamp added;
  };

  // The range of the people list.
  //
  struct people_page
  {
    vector<person> people;
    size_t         total = 0; // The number of people matching the view.
  };

  // The outcome of following a user.
  //
  enum class follow_result
  {
    added,    // The user was added.
    existing, // The user was already on the list.
    full      // The list is at the limit and so the user was not added.
  };

  LIBXLE_SYMEXPORT const char*
  to_string (follow_result) noexcept;

  // The social relationships storage.
  //
  // On Xbox Live a relationship is one-directional: the user follows (adds
  // to their people list) another user without the other's consent. The
  // two users are friends if they follow each other. A followed user can
  // also be marked as a favorite.
  //
  // The operations are atomic coroutines that run on the caller's executor.
  // They throw store_error on failure.
  //
  class LIBXLE_SYMEXPORT social_store
  {
  public:
    // Return the range of the people the owner follows that match the
    // view, ordered by the time they were added and then by XUID.
    //
    virtual awaitable<people_page>
    people (xuid owner, relationship_view, size_t start, size_t count) = 0;

    // Return the followed user or nullopt if the owner doesn't follow them.
    //
    virtual awaitable<optional<person>>
    find (xuid owner, xuid user) = 0;

    // Add the user to the owner's people list unless it already has the
    // limit number of people.
    //
    virtual awaitable<follow_result>
    follow (xuid owner, xuid user, timestamp, size_t limit) = 0;

    // Remove the user from the owner's people list. Return false if the
    // owner doesn't follow them.
    //
    virtual awaitable<bool>
    unfollow (xuid owner, xuid user) = 0;

    // Mark or unmark the followed user as a favorite. Return false if the
    // owner doesn't follow them.
    //
    virtual awaitable<bool>
    favorite (xuid owner, xuid user, bool) = 0;

    social_store () = default;

    virtual
    ~social_store ();

    social_store (const social_store&) = delete;
    social_store& operator= (const social_store&) = delete;
  };

  // The social store that keeps the relationships in memory, for
  // development and testing.
  //
  class LIBXLE_SYMEXPORT memory_social_store: public social_store
  {
  public:
    virtual awaitable<people_page>
    people (xuid, relationship_view, size_t, size_t) override;

    virtual awaitable<optional<person>>
    find (xuid, xuid) override;

    virtual awaitable<follow_result>
    follow (xuid, xuid, timestamp, size_t) override;

    virtual awaitable<bool>
    unfollow (xuid, xuid) override;

    virtual awaitable<bool>
    favorite (xuid, xuid, bool) override;

  private:
    struct relationship
    {
      bool      favorite;
      timestamp added;
    };

    using key = pair<xuid, xuid>; // Owner, user.

    // Order the keys by the owner and then by the user. The comparison is
    // transparent and also orders a key against an owner alone, so that
    // equal_range (owner) returns the owner's relationships.
    //
    struct key_compare
    {
      using is_transparent = void;

      bool
      operator() (const key& x, const key& y) const noexcept {return x < y;}

      bool
      operator() (const key& x, xuid o) const noexcept {return x.first < o;}

      bool
      operator() (xuid o, const key& y) const noexcept {return o < y.first;}
    };

    using relationship_map = std::map<key, relationship, key_compare>;

    // Return the owner's relationships.
    //
    std::ranges::subrange<relationship_map::const_iterator>
    relationships (xuid owner) const;

    relationship_map relationships_;
  };
}
