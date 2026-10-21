/**
 * @file api_hygiene_test.cpp
 * @brief API-shape guard tests (CORE-01/CORE-05/CORE-06/D-16) - success
 *        criteria 1 and 5 made compiling artifacts.
 *
 * Three mechanics, each independently provable (research Pitfall 6):
 *  (a) this TU includes EVERY installed public header and links exactly
 *      sgns-blockstore + p2p::p2p_cid - compiling and running it IS the
 *      standalone-consumer proof;
 *  (b) C++17 SFINAE member-detection (std::void_t + std::declval, NO
 *      C++20 requires expressions - the superbuild pins C++17) proving
 *      the interface shape: no delete/List-shaped member, EvictLocal on
 *      backend types only, tooling interface not a base;
 *  (c) header_hygiene_check.cmake (separate ctest) banning SDK-shaped
 *      includes from public headers.
 *
 * Counter-example these guards exist to prevent: ipfs-lite's
 * InMemoryDatastore::remove() - a datastore interface that grew a
 * delete-shaped member (cited in Plan-01's read_first). CORE-05 bans
 * that shape here permanently.
 */

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

// (a) include-everything: all eleven public headers, dependency order.
// sgns-blockstore now carries a TRANSITIVE consumer dependency on RocksDB
// (resolved through its package config's find_dependency + the PUBLIC
// RocksDB::rocksdb link edge); this TU still links exactly
// sgns-blockstore + p2p::p2p_cid - compiling and running it proves the
// pimpl kept every installed header standalone-compilable with zero
// RocksDB exposure on the consumer's compile/link line.
//
// LOAD-BEARING since Phase 3 (03-01, completed 03-03): minio-cpp is now
// linked PRIVATE into the static library AND the SDK-backed MinioS3Client
// is compiled into it. This TU compiling and running against exactly
// sgns-blockstore + p2p::p2p_cid - with the full SDK inside the lib - IS
// the SC2 no-SDK-type-in-installed-surface proof: any minio type leaking
// into an installed header would break this TU's standalone compile
// (header_hygiene's "minio" include ban is the complementary static
// check). The S3Client seam itself stays internal (D-04; src/ headers,
// asserted by the Head/Delete negative traits below).
#include <sgns-blockstore/Cid.hpp>
#include <sgns-blockstore/BlockStatus.hpp>
#include <sgns-blockstore/Block.hpp>
#include <sgns-blockstore/Options.hpp>
#include <sgns-blockstore/Durability.hpp>
#include <sgns-blockstore/Capabilities.hpp>
#include <sgns-blockstore/BlockStore.hpp>
#include <sgns-blockstore/BlockStoreTooling.hpp>
#include <sgns-blockstore/FaultController.hpp>
#include <sgns-blockstore/MemoryBlockStore.hpp>
#include <sgns-blockstore/LocalBlockStore.hpp>

namespace
{
    //
    // (b) C++17 SFINAE member detection: one trait per banned name.
    // decltype(&T::Name) is ill-formed when the member is absent, which
    // SFINAE-discards the specialization, leaving std::false_type.
    //
    template <class T, class = void>
    struct HasRemove : std::false_type
    {
    };
    template <class T>
    struct HasRemove<T, std::void_t<decltype(&T::Remove)>> : std::true_type
    {
    };

    template <class T, class = void>
    struct HasErase : std::false_type
    {
    };
    template <class T>
    struct HasErase<T, std::void_t<decltype(&T::Erase)>> : std::true_type
    {
    };

    template <class T, class = void>
    struct HasDelete : std::false_type
    {
    };
    template <class T>
    struct HasDelete<T, std::void_t<decltype(&T::Delete)>> : std::true_type
    {
    };

    template <class T, class = void>
    struct HasLowercaseErase : std::false_type
    {
    };
    template <class T>
    struct HasLowercaseErase<T, std::void_t<decltype(&T::erase)>>
        : std::true_type
    {
    };

    template <class T, class = void>
    struct HasLowercaseRemove : std::false_type
    {
    };
    template <class T>
    struct HasLowercaseRemove<T, std::void_t<decltype(&T::remove)>>
        : std::true_type
    {
    };

    template <class T, class = void>
    struct HasList : std::false_type
    {
    };
    template <class T>
    struct HasList<T, std::void_t<decltype(&T::List)>> : std::true_type
    {
    };

    template <class T, class = void>
    struct HasForEach : std::false_type
    {
    };
    template <class T>
    struct HasForEach<T, std::void_t<decltype(&T::ForEach)>> : std::true_type
    {
    };

    template <class T, class = void>
    struct HasEnumerate : std::false_type
    {
    };
    template <class T>
    struct HasEnumerate<T, std::void_t<decltype(&T::Enumerate)>>
        : std::true_type
    {
    };

    template <class T, class = void>
    struct HasEvictLocal : std::false_type
    {
    };
    template <class T>
    struct HasEvictLocal<T, std::void_t<decltype(&T::EvictLocal)>>
        : std::true_type
    {
    };

    // Phase 3 (03-03): Head is an INTERNAL S3Client op name (D-03/D-04) -
    // its presence on a public type would mean the adapter seam climbed
    // to the installed surface. The public existence probe is Has.
    template <class T, class = void>
    struct HasHead : std::false_type
    {
    };
    template <class T>
    struct HasHead<T, std::void_t<decltype(&T::Head)>> : std::true_type
    {
    };
}

// CORE-05: no delete-shaped member may EVER exist on the BlockStore
// interface - local EvictLocal on concrete backend types is the only
// removal-shaped surface (D-16).
static_assert(!HasRemove<sgns::blockstore::BlockStore>::value,
              "CORE-05 violation: BlockStore must not expose Remove");
static_assert(!HasErase<sgns::blockstore::BlockStore>::value,
              "CORE-05 violation: BlockStore must not expose Erase");
static_assert(!HasDelete<sgns::blockstore::BlockStore>::value,
              "CORE-05 violation: BlockStore must not expose Delete");
static_assert(!HasLowercaseErase<sgns::blockstore::BlockStore>::value,
              "CORE-05 violation: BlockStore must not expose erase");
static_assert(!HasLowercaseRemove<sgns::blockstore::BlockStore>::value,
              "CORE-05 violation: BlockStore must not expose remove");

// CORE-06: no enumeration member on the data-path interface.
static_assert(!HasList<sgns::blockstore::BlockStore>::value,
              "CORE-06 violation: BlockStore must not expose List");
static_assert(!HasForEach<sgns::blockstore::BlockStore>::value,
              "CORE-06 violation: BlockStore must not expose ForEach");
static_assert(!HasEnumerate<sgns::blockstore::BlockStore>::value,
              "CORE-06 violation: BlockStore must not expose Enumerate");

// D-16 placement: EvictLocal exists on backend types only - never on the
// interface.
static_assert(!HasEvictLocal<sgns::blockstore::BlockStore>::value,
              "D-16 violation: EvictLocal must not exist on BlockStore");
static_assert(HasEvictLocal<sgns::blockstore::MemoryBlockStore>::value,
              "D-16 violation: MemoryBlockStore must expose EvictLocal");
static_assert(HasEvictLocal<sgns::blockstore::LocalBlockStore>::value,
              "D-16 violation: LocalBlockStore must expose EvictLocal");

// D-03 (Phase 3): delete stays an INTERNAL S3Client adapter op - the
// concrete public backends stay delete-free exactly like the interface.
static_assert(!HasDelete<sgns::blockstore::MemoryBlockStore>::value,
              "D-03 violation: MemoryBlockStore must not expose Delete");
static_assert(!HasDelete<sgns::blockstore::LocalBlockStore>::value,
              "D-03 violation: LocalBlockStore must not expose Delete");

// S3-symbol negative (03-03, SC2/BACK-06): the internal adapter's op
// names (Head - the S3 existence probe) must never appear on the public
// type family; the public existence probe is Has, never Head.
static_assert(!HasHead<sgns::blockstore::BlockStore>::value,
              "SC2 violation: BlockStore must not expose an S3-shaped Head");
static_assert(!HasHead<sgns::blockstore::MemoryBlockStore>::value,
              "SC2 violation: MemoryBlockStore must not expose Head");
static_assert(!HasHead<sgns::blockstore::LocalBlockStore>::value,
              "SC2 violation: LocalBlockStore must not expose Head");

// CORE-06 structural separation: the tooling/enumeration interface is not
// a base of the data-path interface.
static_assert(
    !std::is_base_of<sgns::blockstore::BlockStoreTooling,
                     sgns::blockstore::BlockStore>::value,
    "CORE-06 violation: BlockStoreTooling must not be a base of BlockStore");

/**
 * @given the compiled guard TU (static_asserts above already proved the
 *        shape at build time)
 * @when the trivial shell test runs
 * @then the binary registers with ctest and passes - the standalone-
 *        consumer compile+link claim is exercised end to end
 */
TEST(ApiHygieneTest, HygieneShell)
{
    auto store = std::make_shared<sgns::blockstore::MemoryBlockStore>();
    SUCCEED() << "standalone consumer TU compiles, links, and runs against "
                 "exactly sgns-blockstore + p2p::p2p_cid";
    (void)store;
}
