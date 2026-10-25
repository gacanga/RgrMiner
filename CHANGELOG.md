# Changelog

## 0.1.1 — 2026-10-25
- GPU kernel: `RGR_UNROLL` 11 by default (11 rounds per loop iteration): +0.5 % on P104-100, measured and
  reproduced; 2 and 22 are slower. Compared against the kernel of another RIGOR CUDA miner
  (1 to 5 % slower than RgrMiner on the same card).
- Tuning notes for the NVIDIA P104-100 8 GB: `TUNING.md` / `TUNING.fr.md` (also in the zip).

## 0.1.0 — 2026-10-25
- First public release.
