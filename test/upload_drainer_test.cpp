/**
 * @file upload_drainer_test.cpp
 * @brief UploadDrainer journal-composition tests (D-01..D-04): a real
 *        LocalBlockStore (fresh RocksDB dir per case) draining into an
 *        S3BlockStore over the InMemoryS3Client fake.
 *
 * Tier: default CI lane, conformance-free link line
 * (local_block_store_test shape; RocksDB arrives transitively through
 * the library). The drainer's journal assertions drive the same
 * triple the Phase-2 journal tests used - GetUploadState +
 * GetPendingUploads + byte identity - but THROUGH the drain.
 *
 * Test-shape note (documented deviation from the plan letter): the
 * fault tables at BOTH layers are op-kind-grained (fake SetFault, local
 * FaultController), so "exactly one of two items fails" inside a
 * SINGLE drain call is not mechanically expressible. Record-and-
 * continue, journal-default-success, and pending-retention are
 * asserted per item on all-failing drains; sibling independence is
 * proven by convergence after partial clears; the mixed-outcome shape
 * is covered by the fresh-store re-drain (alreadyPresent) path.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <sgns-blockstore/LocalBlockStore.hpp>
#include <sgns-blockstore/S3BlockStore.hpp>
#include <sgns-blockstore/UploadDrainer.hpp>

// Internal fake - src/ include privilege (test-surface-only).
#include "InMemoryS3Client.hpp"

namespace
{
    using sgns::blockstore::BlockStore;
    using sgns::blockstore::Cid;
    using sgns::blockstore::CidOf;
    using sgns::blockstore::DrainItem;
    using sgns::blockstore::DrainPendingUploads;
    using sgns::blockstore::DurabilityLevel;
    using sgns::blockstore::FaultKind;
    using sgns::blockstore::InMemoryS3Client;
    using sgns::blockstore::LocalBlockStore;
    using sgns::blockstore::LocalBlockStoreOptions;
    using sgns::blockstore::S3BlockStore;
    using sgns::blockstore::StatusClass;
    using sgns::blockstore::UploadState;
    using libp2p::multi::MulticodecType;

    gsl::span<const uint8_t> SpanOf(const std::string& s)
    {
        return gsl::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    Cid MakeCid(const std::string& payload)
    {
        return CidOf(SpanOf(payload), MulticodecType::Code::RAW).value();
    }

    // SHORT relative path under the build tree (Windows MAX_PATH; the
    // local_block_store_test NextUniqueDir discipline).
    std::string NextUniqueDir()
    {
        static std::atomic<uint64_t> next{ 0 };
        return "test-data/upload-drainer/case-"
            + std::to_string(next.fetch_add(1));
    }

    // A truly FRESH store per case: the monotonic counter resets every
    // process run, so a leftover dir from a previous run would reopen
    // its stale journal - wipe before Open (this suite never tests
    // recovery-across-runs, unlike local_block_store_test).
    std::string FreshDir()
    {
        auto dir = NextUniqueDir();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return dir;
    }

    class UploadDrainerTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            auto opened_local =
                LocalBlockStore::Open(LocalBlockStoreOptions{ FreshDir() });
            ASSERT_TRUE(opened_local);
            local_ = std::move(opened_local.value());
            fake_ = std::make_shared<InMemoryS3Client>();
            auto opened_remote = S3BlockStore::Open(fake_);
            ASSERT_TRUE(opened_remote);
            remote_ = std::move(opened_remote.value());
        }

        std::unique_ptr<LocalBlockStore> local_;
        std::shared_ptr<InMemoryS3Client> fake_;
        std::unique_ptr<S3BlockStore> remote_;
    };
} // namespace

/**
 * @given two locally-put blocks (journal records born pending)
 * @when DrainPendingUploads runs
 * @then both items report success uploads at the ObjectStoreAccepted
 *        level with successful journal marks, GetUploadState shows the
 *        Accepted level for both, the pending set is empty, and the
 *        remote holds byte-identical payloads.
 */
TEST_F(UploadDrainerTest, HappyPathDrainsAllAndAdvancesJournal)
{
    const std::string payload_a = "drainer happy path payload a";
    const std::string payload_b = "drainer happy path payload b";
    const auto cid_a = MakeCid(payload_a);
    const auto cid_b = MakeCid(payload_b);
    ASSERT_TRUE(local_->Put(cid_a, SpanOf(payload_a)));
    ASSERT_TRUE(local_->Put(cid_b, SpanOf(payload_b)));

    auto drained = DrainPendingUploads(*local_, *remote_);
    ASSERT_TRUE(drained);
    ASSERT_EQ(drained.value().size(), 2u);
    for (const auto& item : drained.value())
    {
        ASSERT_TRUE(item.upload);
        EXPECT_EQ(item.upload.value().achieved,
                  DurabilityLevel::ObjectStoreAccepted);
        EXPECT_FALSE(item.upload.value().alreadyPresent);
        ASSERT_TRUE(item.journal); // the mark ran and succeeded
    }

    EXPECT_EQ(local_->GetUploadState(cid_a).value(),
              UploadState::UploadedAccepted);
    EXPECT_EQ(local_->GetUploadState(cid_b).value(),
              UploadState::UploadedAccepted);
    auto pending = local_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());

    auto got_a = remote_->Get(cid_a);
    ASSERT_TRUE(got_a);
    EXPECT_EQ(got_a.value().bytes,
              std::vector<uint8_t>(payload_a.begin(), payload_a.end()));
    auto got_b = remote_->Get(cid_b);
    ASSERT_TRUE(got_b);
    EXPECT_EQ(got_b.value().bytes,
              std::vector<uint8_t>(payload_b.begin(), payload_b.end()));
}

/**
 * @given two pending blocks and a scripted fake Put fault
 * @when the drain runs under the fault
 * @then every item is RECORDED (never aborts): upload carries the
 *        taxonomy failure, journal stays at its default success (no
 *        mark ran), both CIDs remain at the pending level, and the
 *        pending set still holds both - journal discipline holds under
 *        failure (D-03/D-04).
 */
TEST_F(UploadDrainerTest, RemotePutFaultRecordsFailuresAndRetainsPending)
{
    const std::string payload = "drainer fault payload";
    const auto cid_a = MakeCid(payload + " a");
    const auto cid_b = MakeCid(payload + " b");
    ASSERT_TRUE(local_->Put(cid_a, SpanOf(payload + " a")));
    ASSERT_TRUE(local_->Put(cid_b, SpanOf(payload + " b")));

    fake_->SetFault(sgns::blockstore::S3Op::Put,
                    sgns::blockstore::S3FaultKind::Unavailable);
    auto drained = DrainPendingUploads(*local_, *remote_);
    ASSERT_TRUE(drained); // per-item failures are NOT envelope failures
    ASSERT_EQ(drained.value().size(), 2u);
    for (const auto& item : drained.value())
    {
        ASSERT_FALSE(item.upload);
        EXPECT_EQ(item.upload.error().code,
                  StatusClass::TemporarilyUnavailable);
        ASSERT_TRUE(item.journal); // default success - no mark ran
    }

    EXPECT_EQ(local_->GetUploadState(cid_a).value(),
              UploadState::PendingUpload);
    EXPECT_EQ(local_->GetUploadState(cid_b).value(),
              UploadState::PendingUpload);
    auto pending = local_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending.value().size(), 2u);
}

/**
 * @given a drain that failed wholesale under a Put fault, then a clean
 *        re-drain; then a FRESH local store re-putting a block the
 *        remote already holds
 * @when the second drain (convergence) and the fresh-store drain run
 * @then the re-drain uploads both still-pending blocks to Accepted and
 *        empties the pending set, and the fresh store's re-put returns
 *        alreadyPresent=TRUE in the success slot (the conditional
 *        create-only hit - the idempotent retry path) while still
 *        advancing its journal to Accepted.
 */
TEST_F(UploadDrainerTest, ReDrainConvergesAndRePutIsAlreadyPresent)
{
    const std::string payload_a = "drainer convergence payload a";
    const std::string payload_b = "drainer convergence payload b";
    const auto cid_a = MakeCid(payload_a);
    const auto cid_b = MakeCid(payload_b);
    ASSERT_TRUE(local_->Put(cid_a, SpanOf(payload_a)));
    ASSERT_TRUE(local_->Put(cid_b, SpanOf(payload_b)));

    // Partial progress state: the drain fails wholesale under the fault.
    fake_->SetFault(sgns::blockstore::S3Op::Put,
                    sgns::blockstore::S3FaultKind::Unavailable);
    ASSERT_TRUE(DrainPendingUploads(*local_, *remote_));
    fake_->ClearFaults();

    // Convergence: the re-drain retries EXACTLY the still-pending
    // blocks and they reach the Accepted level.
    auto converged = DrainPendingUploads(*local_, *remote_);
    ASSERT_TRUE(converged);
    ASSERT_EQ(converged.value().size(), 2u);
    for (const auto& item : converged.value())
    {
        ASSERT_TRUE(item.upload);
        ASSERT_TRUE(item.journal);
    }
    auto pending = local_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());

    // Idempotent re-put: a FRESH local store pending the same CID
    // drains with the remote Put returning alreadyPresent=true (the
    // create-only hit) and its journal still advances to Accepted.
    auto opened_second = LocalBlockStore::Open(
        LocalBlockStoreOptions{ FreshDir() });
    ASSERT_TRUE(opened_second);
    auto second = std::move(opened_second.value());
    ASSERT_TRUE(second->Put(cid_a, SpanOf(payload_a)));
    auto re_drained = DrainPendingUploads(*second, *remote_);
    ASSERT_TRUE(re_drained);
    ASSERT_EQ(re_drained.value().size(), 1u);
    ASSERT_TRUE(re_drained.value()[0].upload);
    EXPECT_TRUE(re_drained.value()[0].upload.value().alreadyPresent);
    ASSERT_TRUE(re_drained.value()[0].journal);
    EXPECT_EQ(second->GetUploadState(cid_a).value(),
              UploadState::UploadedAccepted);
    auto second_pending = second->GetPendingUploads();
    ASSERT_TRUE(second_pending);
    EXPECT_TRUE(second_pending.value().empty());
}

/**
 * @given two pending blocks and a scripted LOCAL fault (the local Get
 *        fails for every block)
 * @when the drain runs under the local fault
 * @then each item records the local-fetch failure in its upload slot
 *        with the journal untouched (default success), both CIDs stay
 *        pending, and clearing the fault lets the next drain converge.
 *
 * @note the literal bytes-without-journal local miss (evicted bytes,
 *       retained pending record) is not publicly reachable -
 *       LocalBlockStore::EvictLocal removes the bytes AND the journal
 *       record in one atomic batch - so the local-failure branch is
 *       exercised through the local fault seam (documented deviation;
 *       see the file banner).
 */
TEST_F(UploadDrainerTest, LocalGetFaultRecordsPerItemFailureJournalUntouched)
{
    const std::string payload = "drainer local fault payload";
    const auto cid_a = MakeCid(payload + " a");
    const auto cid_b = MakeCid(payload + " b");
    ASSERT_TRUE(local_->Put(cid_a, SpanOf(payload + " a")));
    ASSERT_TRUE(local_->Put(cid_b, SpanOf(payload + " b")));

    local_->SetFault(FaultKind::Unavailable);
    auto drained = DrainPendingUploads(*local_, *remote_);
    ASSERT_TRUE(drained);
    ASSERT_EQ(drained.value().size(), 2u);
    for (const auto& item : drained.value())
    {
        ASSERT_FALSE(item.upload); // the local fetch failure recorded
        EXPECT_EQ(item.upload.error().code,
                  StatusClass::TemporarilyUnavailable);
        ASSERT_TRUE(item.journal); // no mark ran - default success
    }
    // GetUploadState is fault-gated too (policy::terminate - never
    // .value() a possibly-failed envelope): clear BEFORE asserting.
    local_->SetFault(FaultKind::None);
    EXPECT_EQ(local_->GetUploadState(cid_a).value(),
              UploadState::PendingUpload);
    EXPECT_EQ(local_->GetUploadState(cid_b).value(),
              UploadState::PendingUpload);

    auto converged = DrainPendingUploads(*local_, *remote_);
    ASSERT_TRUE(converged);
    ASSERT_EQ(converged.value().size(), 2u);
    auto pending = local_->GetPendingUploads();
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending.value().empty());
}

/**
 * @given a fresh local store with an empty pending set
 * @when the drain runs
 * @then the outer envelope is a SUCCESS carrying an empty vector (the
 *        documented fallback shape for the wholesale-envelope test:
 *        GetPendingUploads has no fault seam, and post-Close store use
 *        is unguarded-by-design (Phase-3 review W-1) - the empty-pending
 *        drain IS the reachable outer-envelope assertion).
 */
TEST_F(UploadDrainerTest, EmptyPendingDrainReturnsEmptyVector)
{
    auto drained = DrainPendingUploads(*local_, *remote_);
    ASSERT_TRUE(drained);
    EXPECT_TRUE(drained.value().empty());
}
