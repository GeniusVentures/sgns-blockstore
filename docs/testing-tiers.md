# Testing Tiers (TEST-01 / SC3)

This document inventories every registered `sgns-blockstore` ctest and
its tier assignment: what runs in the hermetic CI lane, what is
env-gated opt-in, and what is dev-gated live evidence. It is the
human-facing tier contract that pairs with the labels in
`test/CMakeLists.txt`.

**Every row below traces to a `set_tests_properties(... LABELS ...)`
line in `test/CMakeLists.txt`, an env-var gate grepped from the test
sources, or a committed artifact under `docs/evidence/s3d-spike/`**
(D-16). Phase 5 does **not** re-run s3d — the Tier-3 rows reuse the
Phase-4 committed evidence, never refreshed (D-17).

## Tier inventory

| Test | Tier | Gate env vars | Notes / evidence pointers |
|---|---|---|---|
| `memory_block_store_test` | Tier1 | — | conformance kit vs `MemoryBlockStore` |
| `cid_spike_test` | Tier1 | — | Phase-1 C1 CID spike suite |
| `local_block_store_test` | Tier1 | — | conformance + backend suite |
| `local_block_store_crash_test` | Tier1 | — | crash/corruption suite; extended in Phase 5 (four-state crash matrix) |
| `api_hygiene_test` | Tier1 | — | API-shape guards |
| `header_hygiene` | Tier1 | — | CMake-script static audit: installed-header include hygiene (CORE-01) |
| `tier1_static_audit` | Tier1 | — | CMake-script static audit (Phase 5, D-14): fixed-sleep + non-loopback-host scan over test sources, plus the registration-vs-label count guard |
| `minio_cpp_spike_test` | Tier1;Tier2 | live case: `SGNS_BLOCKSTORE_TEST_MINIO_BIN` | MIXED binary — hermetic stub cases run in CI; the env-gated MinIO-binary TLS smoke loud-skips without the gate |
| `in_memory_s3_client_test` | Tier1 | — | fake op coverage |
| `s3_block_store_test` | Tier1 | — | conformance + key layout over the fake |
| `upload_drainer_test` | Tier1 | — | drainer suite; extended in Phase 5 (key-scoped partial failure) |
| `minio_s3_client_test` | Tier1;Tier2 | live cases: `SGNS_BLOCKSTORE_TEST_MINIO_BIN` | MIXED binary — hermetic stub matrix runs in CI; env-gated live roundtrips loud-skip |
| `s3_block_store_minio_test` | Tier2 | `SGNS_BLOCKSTORE_TEST_MINIO_BIN` | MinIO-binary store roundtrip; harness child process, no Docker |
| `s3d_spike_test` | Tier3 | `SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT`, `SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY`, `SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY`, `SGNS_BLOCKSTORE_TEST_S3D_BUCKET`, `SGNS_BLOCKSTORE_TEST_S3D_USE_TLS` | dev-gated spike; evidence: `docs/evidence/s3d-spike/` (`sdk-error-shapes.txt`, `staged-read-headers.txt`, `stats-*.txt`) |
| `s3d_roundtrip_e2e` | Tier3 | the five above + `SGNS_BLOCKSTORE_TEST_S3D_RUN_STATE` | two-run fresh-process E2E (S3D-02); evidence: `docs/evidence/s3d-spike/e2e-stats-captures.txt` + `README.md`; finding doc: `docs/s3d-durability.md` |

## The label x env-gate interplay rule (D-13/D-15)

Env-gating stays the **inner** enforcement layer — every live case
loud-skips with a labelled `[SGNS_BLOCKSTORE][...]` message when its
gate env var is unset; it can never fail a lane it was not invited
into. Labels carry the **inspectability** layer: the tier assignment of
every test is readable directly from `test/CMakeLists.txt`.

The two MIXED binaries carry `Tier1;Tier2` (multi-label via semicolon),
so CI's `-L Tier1` runs their hermetic half while their env-gated live
cases loud-skip without the gate; a developer's
`ctest -L Tier2` with `SGNS_BLOCKSTORE_TEST_MINIO_BIN` set becomes the
complete opt-in Tier-2 lane. There is deliberately **no skip meta-test**
anywhere (D-15) — the env-gate + loud-labelled-skip pattern is the
enforcement inner layer, and the absence of a skip-counting test is a
recorded decision, not an omission.

## The CI lane (D-13)

```
ctest --test-dir sgns-blockstore/src/sgns-blockstore-build -C <type> --output-on-failure -L Tier1 --no-tests=error
```

This is the exact invocation in `thirdparty/.github/workflows/build.yml`
("Run sgns-blockstore tests", Windows/Linux/OSX legs). `-L Tier1`
**fails closed** — ctest `-L` never includes an unlabeled test, so a
future unlabeled test cannot leak into the hermetic lane (a `-LE` form
would fail open) — and `--no-tests=error` fails an empty selection, so
a label typo cannot pass vacuously. The `tier1_static_audit` ctest
doubles as a local tripwire: it fails when a registration in
`test/CMakeLists.txt` carries no `LABELS` line. Dev-machine full runs
stay unlabeled `ctest` — every gated case skips loudly.

## Tier-3 evidence (D-17)

The Tier-3 rows cite Phase-4 committed artifacts under
`docs/evidence/s3d-spike/` and the characterization in
`docs/s3d-durability.md`. **Phase 5 does not re-run s3d** — the
evidence is reused as-is, never refreshed, by decision.
