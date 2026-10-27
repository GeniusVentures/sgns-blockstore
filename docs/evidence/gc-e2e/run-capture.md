# Live s3d GC E2E - Run Capture (TEST-03, Phase 11 plan 11-04)

Run date: 2026-10-27 | Mode: SGNS_BLOCKSTORE_TEST_S3D_SKIP_FLUSH_EVIDENCE=1 (checkpoint-ruled)

Mode note: s3d /objects/flush blocks until Sia slot progression completes; this
session could not observe pendingObjects==0 (Phase-10 accepted-is-max-provable
ceiling), so per the human checkpoint ruling the durable marks were driven
directly. The proven claim - eviction + CID-verified retrieval against the real
s3d backend - is unaffected by how Durable was reached. Uploads were REAL (the
drain asserts passed against the live endpoint).

LOG-CORROBORATED PRECISION (see stats-capture.txt, the s3d console for this
session): s3d accepted every upload into its staging (groups of 3 objects /
173 bytes per run, matching reclaimed_bytes=58 for one ~58-byte block); every
Sia finalize attempt failed ("context canceled" - client read budgets of 30s/5s
canceled s3d's in-flight finalize; waste 100%); and the post-eviction GET was
served from s3d's LOCAL STAGING (no backend.sia GET activity exists). So: the
S3-API-level vertical is PROVEN live; the Sia-cloud durability leg is NOT
proven this session - exactly the documented ceiling. Rerun with funded rent
and a LONG flush budget to prove the Sia leg (short client budgets actively
cancel the finalize).

```
15: [SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-1] spill complete: blocks=3 journal=UploadedDurable flush_evidence=SKIP_MODE_CEILING_DOCUMENTED
15: [SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] eviction complete: local=absent marker=present
15: [SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] blocks=3 reclaimed_count=1 reclaimed_bytes=58
15: [SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] retrieval_digest=af945662a6c32a0e20ece27e9d4a669c894dae791af992dae0df102932ceb81a
15: [SGNS_BLOCKSTORE][s3d-gc-e2e][RUN-2] verdict=SPILL_GC_RETRIEVE_PROVEN_LIVE
15: [  PASSED  ] 2 tests.
1/1 Test #15: s3d_gc_e2e .......................   Passed   10.29 sec
The following tests passed:
100% tests passed, 0 tests failed out of 1
```

Env gates used (NAMES only): SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT/_ACCESS_KEY/_SECRET_KEY/_BUCKET/_ADMIN_PASSWORD (via W:\gnus\s3d-spike-env.ps1), _RUN_STATE, _SKIP_FLUSH_EVIDENCE=1.

