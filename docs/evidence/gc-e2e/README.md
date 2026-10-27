# GC Live-Tier Evidence (TEST-03, v1.1 Phase 11 plan 11-04)

Dev-gated live evidence that the Phase-11 GC vertical — spill → GC →
retrieve — works against the REAL s3d backend. Never CI: both targets are
env-gated with loud labelled skips and labeled Tier2/Tier3 only.

## Targets

| Target | Tier | Gate |
|--------|------|------|
| `s3d_gc_e2e` | Tier3 | S3D endpoint quartet + admin password + run-state path (below) |
| `gc_minio_roundtrip_test` | Tier2 | `SGNS_BLOCKSTORE_TEST_MINIO_BIN` |

## Env gates (NAMES only — never commit values)

- `SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT` — the running s3d S3 API endpoint
- `SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY` / `SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY`
- `SGNS_BLOCKSTORE_TEST_S3D_BUCKET` — a fresh bucket per run (create via `mc.exe`)
- `SGNS_BLOCKSTORE_TEST_S3D_ADMIN_PASSWORD` — the s3d admin listener credential
- `SGNS_BLOCKSTORE_TEST_S3D_ADMIN_ENDPOINT` — optional (default `http://127.0.0.1:8001`)
- `SGNS_BLOCKSTORE_TEST_S3D_RUN_STATE` — the two-run handoff file path
- `SGNS_BLOCKSTORE_TEST_MINIO_BIN` — `W:\gnus\minio-community.exe` (RELEASE.2025-04-22 community build; NEVER the license-gated `minio.exe`)

## Binary inventory (W:\gnus, operator-provisioned)

- s3d daemon: `W:\gnus\s3d_windows_amd64` (session env via `W:\gnus\s3d-spike-env.ps1` — dot-source per terminal; env does not persist)
- `mc.exe` (sha256-pinned) for bucket creation (`mc mb` via `MC_HOST_<alias>`)
- `minio-community.exe` for the Tier-2 leg

## The two-run procedure (s3d_gc_e2e — Tier3)

```
ctest --test-dir thirdparty/build/Windows/Debug/sgns-blockstore/src/sgns-blockstore-build \
  -C Debug -R s3d_gc_e2e --output-on-failure
```
The test itself runs both cases in one process invocation; the strict
two-fresh-process form is:
```
s3d_gc_e2e.exe --gtest_filter=S3dGcE2E.SpillRun
s3d_gc_e2e.exe --gtest_filter=S3dGcE2E.EvictRun
```
Run 1 spills N=3 blocks, drains to s3d, observes the flush evidence
(`pendingObjects == 0`) and marks Durable. Run 2 reopens the SAME store in
a fresh process, backdates one record (the age policy is hermetically
proven — the live leg proves the mechanics), evicts it, and retrieves the
bytes CID-verified from s3d.

## Node-level operator section (optional confirmation)

The library-level E2E composes shipped surfaces (no heads-aware policy).
For a node-level confirmation, run a SuperGenius dev session with a
`crdt_config.json` `blockstore` section containing `retention`
(`retention_age_ms` — see the Phase-11 config gate) and capture:
`GetColdStorageStatus()` output (the `gc_*` fields:
candidates/reclaimed_count/reclaimed_bytes/disposed_marker_count) and the
`gc:` log lines from one pass. Save to this directory.

## Evidence artifacts (this directory)

| Artifact | Content |
|----------|---------|
| `run-capture.md` | The `[SGNS_BLOCKSTORE][s3d-gc-e2e]` capture block: blocks spilled, reclaimed count/bytes, retrieval digest |
| `stats-capture.txt` | Optional admin stats / bucket listing captures (NO credentials) |

## REDACTION RULE (hard)

No credential VALUES, no passwords, no keys — env var NAMES only, ever.
The plan's redaction gate greps this directory for `SECRET|PASSWORD=`
shapes and must count zero.
