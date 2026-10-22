/**
 * @file UploadDrainer.cpp
 * @brief Implementation of the pending-upload drain (D-01..D-04).
 */

#include <sgns-blockstore/UploadDrainer.hpp>

#include <utility>

namespace sgns::blockstore
{
    result<std::vector<DrainItem>, BlockStatus> DrainPendingUploads(
        LocalBlockStore& local, BlockStore& remote,
        const WriteOptions& options)
    {
        // (1) Snapshot first (PutMany outer-failure precedent): an
        // enumeration failure is wholesale malfunction - per-item
        // reporting is impossible, so the whole drain aborts with the
        // BlockStatus envelope. Marks below can never affect this
        // snapshot (a succeeded CID leaves the pending set only AFTER
        // this vector was taken).
        auto pending = local.GetPendingUploads();
        if (!pending)
        {
            return pending.error();
        }

        std::vector<DrainItem> results;
        results.reserve(pending.value().size());
        for (const auto& cid : pending.value())
        {
            // (2) Fetch the bytes. A local miss (e.g. the block was
            // evicted) is a RECORDED per-item failure - the drain
            // never aborts its siblings over it (D-03).
            auto block = local.Get(cid);
            if (!block)
            {
                results.push_back(
                    DrainItem{ cid, block.error(), outcome::success() });
                continue;
            }

            // (3) Remote Put. S3BlockStore's Put is conditional, so a
            // re-drain re-put of a block the remote already holds
            // returns alreadyPresent in the SUCCESS slot - the
            // idempotent retry path that makes interrupted drains
            // converge.
            auto upload = remote.Put(cid, block.value().bytes, options);
            if (!upload)
            {
                results.push_back(
                    DrainItem{ cid, upload.error(), outcome::success() });
                continue;
            }

            // (4) D-04: the journal advances to the Accepted level
            // ONLY on remote-present outcomes (the success slot -
            // achieved OR alreadyPresent). Any mark not grounded in a
            // remote-present outcome would fabricate upload state; a
            // mark failure never rolls back the upload, and the CID
            // stays pending so the mark retries on the next drain.
            auto mark = local.MarkUploadedAccepted(cid);
            results.push_back(DrainItem{ cid, std::move(upload),
                                         std::move(mark) });
        }
        return results;
    }
} // namespace sgns::blockstore
