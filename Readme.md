# sgns-blockstore

Static cold-storage BlockStore library: memory, local-RocksDB, and S3
object-storage backends behind a frozen CID-verified contract, so older
CRDT/IPLD blocks can spill from hot RocksDB state to durable,
CID-verified object storage — consumable by both SuperGenius and
genius-tube, with local eviction never acting as a CRDT deletion.

- Testing strategy (three tiers, hermetic CI lane, env-gated opt-in
  lanes): [docs/testing-tiers.md](docs/testing-tiers.md)
- s3d staged-vs-durable characterization:
  [docs/s3d-durability.md](docs/s3d-durability.md)

Build: the library and its test battery build through the thirdparty
superbuild entry (`thirdparty/build/<Platform>/<Debug|Release>`, with
`-DTESTING:BOOL=ON`).
