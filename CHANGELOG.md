# Changelog

## 0.2.2 — 2026-10-25
- **Pool mode fix: duplicate shares.** The Gozero pool serves the same job (same `workid`, same `curtime`) until its
  transactions change and silently ignores a share sent twice (answer `null`, not counted). 0.2.0 / 0.2.1 rebuilt the
  job every 60 s from the same time, so the cards hashed the same headers again: about 25-30 % of the "accepted" shares
  were not credited by the pool. Now the job is kept while the pool serves it, and every nTime comes from one counter
  per header (version, previous block, merkle root, bits) for the whole run, starting from the clock. Reproduced and
  checked with `tests/fake_pool.py` (`FAKE_POOL_JOB_S`, `FAKE_POOL_SILENT_DUP`): 43 duplicates out of 54 shares with
  0.2.1, 0 with 0.2.2.
- Hashrate report to the pool (`report-hashrate`, default 1): `gozero_telemetry` (measured hashrate, device, client)
  with `getblocktemplate`, every 5 s or more. Display only on the pool side.
- `template-max-age` is no longer used.

## 0.2.1 — 2026-10-25
- Pool mode: shares are submitted by several threads in parallel (`share-threads`, default 4). On the real pool one
  submission takes about 1 s (one HTTPS request each), so a single sender could not keep up beyond about one share per
  second: the queue grew and the shares still waiting at each new block were lost as stale.
- Stats line: average submission time and longest share queue.
- README and `rgrminer_pool.conf`: how to choose `share-work` (about 0.5 to 1 share per second).
- `tests/fake_pool.py`: `FAKE_POOL_SUBMIT_MS` simulates a slow pool.

## 0.2.0 — 2026-10-25
- **Pool mode for the Gozero pool** (`pool-url`, `pool-user`, `share-work`, `ca-file`, ...): pooled getblocktemplate over
  HTTPS, share target, `submitblock` with `workid`; certificate and host name verification (OpenSSL); shares sent by a
  separate thread; stale shares dropped; optional local node tip watch (new pool work within ~0.25 s of a new block).
- `rgrminer_pool.conf` and `start_pool.bat` in the zip; `ca-bundle.crt` shipped.
- `--run-seconds` (timed runs for tests); `tests/fake_pool.py`.

## 0.1.1 — 2026-10-25
- GPU kernel: `RGR_UNROLL` 11 by default (11 rounds per loop iteration): +0.5 % on P104-100, measured and
  reproduced; 2 and 22 are slower. Compared against the kernel of another RIGOR CUDA miner
  (1 to 5 % slower than RgrMiner on the same card).
- Tuning notes for the NVIDIA P104-100 8 GB: `TUNING.md` / `TUNING.fr.md` (also in the zip).

## 0.1.0 — 2026-10-25
- First public release.
