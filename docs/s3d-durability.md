# s3d Durability Characterization (S3D-03)

This document characterizes the staged-vs-durable semantics of **Sia
s3d v0.2.1** as they concern the `sgns-blockstore` library: what an
object-store "accepted" acknowledgement proves, what it does not, and
which primitives exist to move between the two. It is the grounding
for the `ObjectStoreAccepted` durability level documented in
`include/sgns-blockstore/Durability.hpp` (D-12).

**Every claim below traces to a committed artifact under
`docs/evidence/s3d-spike/` or to the pinned tag's sources** (D-13).

## 1. Scope and pin

- Characterized version: **s3d v0.2.1**, commit `312789a`, released
  2026-10-05 (build date 2026-10-06). Version banner captured live in
  `docs/evidence/s3d-spike/sdk-error-shapes.txt`.
- Sources: the tag's `openapi.yml` (admin API schema), the tag's
  `README.md` and `CHANGELOG`, and the committed live-spike evidence
  captured at a dev-managed loopback endpoint (S3 API `127.0.0.1:8000`,
  admin `127.0.0.1:8001`, plain HTTP — the s3d lane per TEST-01/D-08:
  dev-gated, never CI).
- The library's `DurabilityLevel::ObjectStoreAccepted` maps to this
  reality: an S3 200-OK from s3d means *received into local staging*,
  not *durable on the Sia network*.

## 2. The finding: no per-object staged-vs-durable signal exists at v0.2.1

The admin API (`openapi.yml` at the tag) exposes upload-pipeline state
through `GET /stats/uploads` (`UploadStats` schema) only. Its verbatim
field inventory:

| Field | openapi.yml description (verbatim, abridged where marked) |
|-------|------------------------------------------------------------|
| `pendingObjects` | Objects buffered on disk waiting to be uploaded to Sia |
| `pendingSize` | Total size of pending objects in bytes |
| `uploadedObjects` | Objects fully uploaded to the Sia network |
| `uploadedSize` | Total size of uploaded objects in bytes |
| `unpinnedObjects` | Objects uploaded to Sia but not yet pinned in the indexer |
| `failedUploads` | Failed upload attempts since the process started |
| `orphanedObjects` | Deleted or overwritten objects pending cleanup |
| `multipartUploads` | In-progress multipart uploads |
| `bufferUsed` | Bytes held on disk in the uploads directory (…) |
| `bufferLimit` | Configured disk buffer limit in bytes. Zero means unlimited |
| `transfer` | `TransferStats`: ingress/upload byte totals and rates plus `activeUploads` |

Every counter is **pipeline-global**. The only per-object-adjacent
signal is `transfer.activeUploads[].label` — *"The bucket and key as
bucket/key when the group holds a single object, otherwise the number
of objects it holds"* — which exists **only while a group is actively
streaming** to Sia (*"Upload groups currently streaming to Sia …
Omitted when none are in flight"*). It is transient in-flight progress,
not a durable-state query; there is no admin endpoint that answers "is
object K staged or durable".

**Consequence (the finding):** `accepted-is-max-provable` is the
correct epistemic ceiling for a writer against s3d v0.2.1 — exactly
what `ObjectStoreAccepted` encodes
(*"object store acknowledged receipt (not yet proven durable)"*,
`Durability.hpp`). A store never reports an achieved level it has not
verified; nothing at this tag lets a writer verify durability per
object.

**Concurrency limitation:** the counters are process-global and racy
under concurrent uploads — counter deltas must NEVER be read as
per-object proof. The committed evidence was captured on a
single-supplier workload (one drain in flight); see the raw bodies in
`docs/evidence/s3d-spike/stats-before-put.txt`,
`docs/evidence/s3d-spike/stats-after-put.txt`, and
`docs/evidence/s3d-spike/stats-after-flush.txt`.

## 3. The positive staged-to-durable mechanism: POST /objects/flush

`openapi.yml` at the tag, verbatim:

> `/objects/flush` (POST): *"Uploads all pending objects to Sia
> immediately, regardless of padding, and pins them so their local
> pending files are removed. Blocks until done."*

Committed evidence — the before/put/flush triple on a single 38-byte
spike object:

- before PUT: `pendingObjects: 0, uploadedObjects: 0`
  (`stats-before-put.txt`)
- after PUT (staged): `pendingObjects: 1, pendingSize: 38,
  bufferUsed: 38, uploadedObjects: 0` (`stats-after-put.txt`) — GET
  and HEAD already succeed against the staged object (section 4)
- after flush: `pendingObjects: 0, uploadedObjects: 1,
  uploadedSize: 38, bufferUsed: 0` (`stats-after-flush.txt`)

The flush is **bucket/global-scoped, not per-object**, and it is the
only documented primitive that moves pending objects to uploaded+pinned
state on demand. Future durability polling (fast-follow work) can
compose a flush + counter observation, but a per-object durability
answer would require a future s3d signal; none exists at this tag.

## 4. Staged reads: success from local staging, no distinguishing header

Objects are served (GET/HEAD) from local staging before upload — the
README's Upload Packing section: *"All objects are first written to a
local uploads directory on disk"* and served from it. Captured wire
evidence in `docs/evidence/s3d-spike/staged-read-headers.txt`
(`mc --debug` full request/response dumps against the staged object
BEFORE flush): the object GET and HEAD return **200 OK** while
`pendingObjects` is still 1.

**Proof of absence:** the captured response headers show **no header
that distinguishes a staged read from a durable read** at this tag —
nothing in the README, `openapi.yml`, or the repo documents one, and
the real captured headers carry none. A reader cannot tell staging
from durability by inspection.

**ETag is never a content proof:** s3d encrypts client-side before
upload (Sia Storage SDK), so digests/ETags are over ciphertext —
content verification must be done client-side against the CID (which
`S3BlockStore::Get` does); an ETag must never be treated as a
content-durability attestation.

## 5. Spike-rendering evidence (the mapper shapes that fired live)

Captured live at v0.2.1 and transcribed in
`docs/evidence/s3d-spike/sdk-error-shapes.txt`:

- **Conditional re-PUT (create-only hit):** s3d's 412 renders through
  minio-cpp as the `"PreconditionFailed: At least one specified
  precondition did not hold."` string — **shape 1 of the three** the
  adapter's `IsPreconditionFailure` classifier handles
  (`src/MinioS3Client.cpp`); absorbed to the SUCCESS slot with
  `alreadyPresent=true` (D-02). Three consecutive conditional PUTs were
  idempotent at the live endpoint.
- **HEAD on a missing key:** s3d's StatObject error string classifies
  **natively** as authoritative absence (`"NoSuchKey:"` prefix) — the
  adapter returned the SUCCESS slot `exists=false`. **The D-05
  contingency was not needed**; `IsAuthoritativeAbsence` is unchanged
  at v0.2.1.

These two results are the live gate (D-05/D-07) that unblocked the
s3d-dependent E2E work: `S3BlockStore`'s `Has`/re-put semantics hold
against s3d as-is.

## 6. Operational notes

- **Disk-pressure behavior:** a PUT waiting for disk space *"gives up
  after two minutes and returns a 503 SlowDown"* (CHANGELOG, since
  0.1.2). Through the SDK this surfaces as an unrecognized error string
  → the adapter mapper's default bucket `TemporarilyUnavailable` —
  the correct class by design (never NotFound).
- **Staging durability narrowed in v0.2.0:** the upload directory is
  synced after staging (v0.2.0 fix), which narrows — but does not
  close — the window where a staged object exists only in unflushed
  filesystem state. Staged ≠ durable remains the model.
- **Pin moves require re-characterization:** this document describes
  v0.2.1 only (33 commits on master at research time — ungated, pin
  does not move this phase). A future pin bump requires re-running the
  spike session (runbook:
  `docs/evidence/s3d-spike/README.md`) and updating this doc.
