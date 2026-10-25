# Changelog

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
