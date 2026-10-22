# s3d Spike Evidence — Runbook (Phase 4, 04-02 / D-05/D-06/D-13)

Live-session procedure for capturing the five committed evidence
artifacts at a **dev-managed s3d v0.2.1** endpoint. Everything here is
executed in a dev session only — the S3D tier never runs in CI
(`s3d_spike_test` loud-skips when the env vars are unset).

## Artifacts (this directory)

| File | Contents |
|------|----------|
| `stats-before-put.txt` | raw `GET /stats/uploads` body, before the spike PUT |
| `stats-after-put.txt` | raw `GET /stats/uploads` body, while the spike object is staged |
| `stats-after-flush.txt` | raw `GET /stats/uploads` body, after `POST /objects/flush` |
| `staged-read-headers.txt` | GET + HEAD response headers against the STAGED object (before flush) |
| `sdk-error-shapes.txt` | s3d version banner + the `[EVIDENCE]` banner lines from the two spike cases |

## Prerequisites (dev-owned, blocking)

1. **s3d v0.2.1** (pinned tag, commit `312789a`): download
   `s3d_windows_amd64.zip` (~7 MB) from
   <https://github.com/SiaFoundation/s3d/releases/tag/v0.2.1>, unpack,
   verify `s3d version` prints `0.2.1`. No Go/Docker needed.
2. **Sia account/wallet setup** per the s3d README (login, user + key
   creation, indexer registration, funded storage).
3. **Bucket + access keys**: create the spike bucket and an access-key
   pair (`s3d keys create`).
4. **Start s3d on loopback** (S3 API `:8000`, admin `:8001` defaults;
   plain HTTP — never TLS-verify the per-start self-signed cert).

## Environment (names only — values are NEVER committed)

| Variable | Source |
|----------|--------|
| `SGNS_BLOCKSTORE_TEST_S3D_ENDPOINT` | S3 API host:port (e.g. `127.0.0.1:8000`) |
| `SGNS_BLOCKSTORE_TEST_S3D_ACCESS_KEY` | `s3d keys create` (credential) |
| `SGNS_BLOCKSTORE_TEST_S3D_SECRET_KEY` | `s3d keys create` (credential) |
| `SGNS_BLOCKSTORE_TEST_S3D_BUCKET` | pre-provisioned bucket |
| `SGNS_BLOCKSTORE_TEST_S3D_ADMIN_ENDPOINT` | admin API base (default `http://127.0.0.1:8001`) |
| `SGNS_BLOCKSTORE_TEST_S3D_ADMIN_PASSWORD` | s3d config adminPassword (credential) |

## Session sequence

`$ADMIN` = `SGNS_BLOCKSTORE_TEST_S3D_ADMIN_ENDPOINT`, `$PW` =
`SGNS_BLOCKSTORE_TEST_S3D_ADMIN_PASSWORD`, `$SPIKE` = the inner build
dir (`thirdparty/build/Windows/Debug/sgns-blockstore/src/sgns-blockstore-build`).
All captures MUST be ASCII/UTF-8: use `curl.exe` (OS-bundled) output
redirection or `Out-File -Encoding ascii` — NEVER the default
PowerShell redirect (UTF-16LE — the 03-04 anti-precedent).

```powershell
# 0. Version banner -> sdk-error-shapes.txt (header line)
s3d version  # transcribe the version/commit/date line by hand

# 1. Before PUT (raw body)
curl.exe -s -u ":$PW" "$ADMIN/stats/uploads" > stats-before-put.txt

# 2. Run BOTH spike cases live (verbose; the [EVIDENCE] banner lines
#    transcribe into sdk-error-shapes.txt: which mapper shape fired
#    for the 412, which absence path fired for HEAD)
ctest --test-dir $SPIKE -C Debug -R s3d_spike_test -V

# 3. While the spike object is STAGED (before flush): staged reads
curl.exe -s -D staged-read-headers.txt -o NUL -u "$AK:$SK" `
  "http://$ENDPOINT/$BUCKET/sgns-s3d-spike/<run-key>"
curl.exe -s -I -u "$AK:$SK" "http://$ENDPOINT/$BUCKET/sgns-s3d-spike/<run-key>" >> staged-read-headers.txt

# 4. After PUT (staged): stats again
curl.exe -s -u ":$PW" "$ADMIN/stats/uploads" > stats-after-put.txt
# expect: pendingObjects reflects the staged spike object (+1 vs step 1)

# 5. Flush (blocking per the pinned openapi: "Uploads all pending
#    objects to Sia immediately, regardless of padding, and pins them
#    ... Blocks until done."), then stats once more
curl.exe -s -X POST -u ":$PW" "$ADMIN/objects/flush"
curl.exe -s -u ":$PW" "$ADMIN/stats/uploads" > stats-after-flush.txt
# expect: pendingObjects back down, uploadedObjects up
```

## Capture discipline

- **ASCII/UTF-8 only** — never the PowerShell default redirect.
- **No secrets**: endpoint HOST at most in any artifact; the scripted
  session scan asserts every set
  `SGNS_BLOCKSTORE_TEST_S3D_{SECRET_KEY,ACCESS_KEY,ADMIN_PASSWORD}`
  VALUE is absent before anything is committed. Scan before commit.
- Raw bodies preserved verbatim (the stats files ARE the UploadStats
  JSON — no reformatting, no truncation).
- Counter deltas are process-global and racy under concurrent uploads:
  keep the session single-supplier (one drain in flight) when capturing
  before/after deltas, and never read a delta as per-object proof.
