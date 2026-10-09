// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <chrono>
#include <compare> // strong_ordering
#include <utility> // to_underlying()

#include <libxle/types.hxx>

// The persistent classes of the PostgreSQL stores (see pgsql.hxx).
//
// This header is the input of the ODB compiler, which generates the
// database support code (model-odb.?xx) from it. In the consumption build
// the pregenerated support code is used (see buildfile for details), so any
// change to this header requires a development build to regenerate it.
//
// The classes mirror the store interfaces' types rather than being them so
// that the database representation is spelled out here and can evolve on
// its own. They are private to the library.

// The database schema versions.
//
// The base version is the oldest version the database can be migrated from
// and the current version is the one this code uses. While the current
// version is open, its changes are not yet released and are folded into it
// (see the ODB manual, Database Schema Evolution). Before releasing a new
// schema version, close it and once released bump the current version for
// the subsequent changes.
//
// The changelog (model.xml), which records the released versions, is used
// by ODB to generate the migration statements. It is both the input and
// output of the ODB compiler and so is kept in the repository next to this
// header.
//
#define LIBXLE_SCHEMA_VERSION_BASE 1
#define LIBXLE_SCHEMA_VERSION      1, open

#pragma db model version(LIBXLE_SCHEMA_VERSION_BASE, LIBXLE_SCHEMA_VERSION)

// The value types stored as integers (see the ODB manual, C++ Type Mapping
// Pragmas).
//
// The XUIDs are stored as their values. Note that PostgreSQL has no unsigned
// integers so they end up in the signed columns of the same size, with the
// most significant bit in the sign bit (see the ODB manual, PostgreSQL Type
// Mapping). This is lossless and the XUIDs are only compared for equality.
//
#pragma db map type(xle::xuid)               \
               as(std::uint64_t)             \
               to(std::to_underlying (?))    \
               from(xle::xuid {(?)})

// The timestamps are stored as nanoseconds since the epoch, which fits until
// the year 2262.
//
#pragma db map type(xle::timestamp)                                     \
  as(std::int64_t)                                                      \
  to(std::chrono::duration_cast<std::chrono::nanoseconds> (             \
       (?).time_since_epoch ()).count ())                               \
  from(xle::timestamp (                                                 \
    std::chrono::duration_cast<xle::duration> (                         \
      std::chrono::nanoseconds (?))))

namespace xle
{
  // The owner follows the target (see social_store).
  //
  #pragma db value
  struct relationship_key
  {
    xuid owner;
    xuid target;

    friend std::strong_ordering
    operator<=> (const relationship_key&, const relationship_key&) = default;

    friend bool
    operator== (const relationship_key&, const relationship_key&) = default;
  };

  // The relationship is looked up by the owner (the primary key prefix) as
  // well as by the target (whether the target follows the owner back).
  //
  #pragma db object table("relationship") pointer(unique_ptr)
  class relationship_record
  {
  public:
    #pragma db id column("")
    relationship_key id;

    bool      favorite;
    timestamp added;

    #pragma db index("relationship_target_i") members(id.target)
  };

  // The relationship together with whether the target follows the owner
  // back (the reverse relationship exists), in the order the targets were
  // followed (see social_store::people()).
  //
  #pragma db view object(relationship_record = forward)                 \
    object(relationship_record = reverse left:                          \
           forward::id.target == reverse::id.owner &&                   \
           forward::id.owner == reverse::id.target)                     \
    query((?) + "ORDER BY" + forward::added + "," + forward::id.target) \
    pointer(unique_ptr)
  struct relationship_entry
  {
    #pragma db column(forward::id.target)
    xuid target;

    #pragma db column(forward::favorite)
    bool favorite;

    #pragma db column(forward::added)
    timestamp added;

    #pragma db column("(" + reverse::id.owner + " IS NOT NULL)")
    bool following;
  };

  // The number of relationships.
  //
  #pragma db view object(relationship_record)
  struct relationship_count
  {
    #pragma db column("count(*)")
    uint64_t result;
  };
}
