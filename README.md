# RgrMiner

Open-source NVIDIA GPU **solo** miner for **RIGOR** (ORU, [rigorchain.com](https://www.rigorchain.com/)), proof of work
`rgrpow01` / OpuHash — **no developer fee**.

> Français : [README.fr.md](README.fr.md)

The official RIGOR v0.2 mainnet release only ships a CPU miner. RgrMiner computes the same proof of work on the GPU and
talks to **your own local RIGOR node** exactly like the official miner does (`getblocktemplate` / `submitblock` on
`127.0.0.1:8761`). There is no pool: every block you find pays 128 ORU (+ fees) to your address.

## Proof of work

```
pow = SHA3-256( "RGRPOW01" || header80 )        FIPS 202 SHA3-256 (padding 0x06, not Keccak-256)
valid  <=>  pow (read as a little-endian uint256) <= target(nBits)
```

The 88-byte preimage fits in a single 136-byte rate block, so one nonce costs exactly **one Keccak-f[1600]
permutation**. The block id, merkle root and txids stay SHA-256d. Reference: `docs/specification/SPEC-008-PROOF-OF-WORK.md`
and `docs/RGRPOW01_REFERENCE_VECTOR_v1.0.md` in the [rigorchain repository](https://github.com/rigorchain/rigorchain).

## How it works

- **GPU engine** (`gpu/rgr_core.h`, `gpu/rgr_gpu.cu` → `rgpu.dll`): lanes held as two 32-bit words (funnel shifts,
  LOP3 on Pascal). Round 0 is precomputed on the host for the 14 lanes that do not depend on the nonce; only the
  11 lanes reached by the nonce (lane 10 itself, columns 1 and 4 through theta) are updated per nonce. The last round
  computes only lane 3 (the 64 most significant bits), compared with the target; every candidate is re-hashed on the
  CPU over the full 256 bits before it is submitted.
- **Node client** (`src/main.cpp`, `src/rpc.h`): `getblocktemplate` with `rules: ["segwit"]` and your
  `coinbase_address`. RIGOR nodes build the coinbase themselves (`rigor_coinbase_immutable`): RgrMiner copies
  `default_coinbase_tx`, checks its txid and the merkle root against the node, and never rebuilds a coinbase.
- **No extranonce exists** (the coinbase cannot change), so each GPU takes its own `nTime` values
  (template time + a slot counter, one slot = 2^32 nonces). Two GPUs never hash the same header. nTime never goes
  past the template `maxtime` nor one hour ahead of the clock.
- **Self-check**: at start and every 10 minutes each GPU recomputes 4,096 digests against the CPU reference; a
  mismatch (unstable overclock...) stops the miner with exit code 6.

## Requirements

- Windows 10/11 x64, NVIDIA Pascal (sm_61: P104-100, GTX 1070/1080) or newer GPU, NVIDIA driver with CUDA 12
  support (R525 or newer). Newer GPUs run the PTX through the driver JIT (works, not tuned).
- The official **RIGOR v0.2 mainnet** wallet / node running on the same computer and synchronized
  ([release](https://github.com/rigorchain/rigorchain/releases/tag/v0.2-mainnet)).

Measured: about **670 MH/s per P104-100** at stock clocks, about **800 MH/s** with the settings of [TUNING.md](TUNING.md)
(core +200 MHz, memory −1000 MHz, 200 W).

## Quick start (release zip)

1. Start the RIGOR mainnet wallet / node, wait until it is synchronized, copy a receive address (`rg1q...`).
2. Edit `rgrminer.conf`: `address = rg1q...` and `auth-file = <node data folder>\rpc-auth.conf`
   (the file the node writes on its first start; you can use `rpc-user` / `rpc-pass` instead).
3. Run `test.bat` (all GPUs against the CPU reference), then `check_node.bat`
   (the node validates a full RgrMiner block in `proposal` mode and the last 5 chain blocks are re-checked).
4. Run `start_rgr.bat`. Log in `rgrminer.log`; one statistics line per minute.

```
rgrminer --config rgrminer.conf [--gpus 0,1] [--log file]
         --test | --bench 60 | --propose | --check-chain 5 | --help
```

Keep the node RPC on `127.0.0.1` only. Never expose port 8761.

## Building

- GPU DLL: CUDA 12.9 + Visual Studio 2022 → `windows\build_gpu_dll.bat` (sm_61 + PTX).
- Host: MSYS2 MINGW64 g++ → `windows/build_host.sh` (links `rgpu.dll`, static, only ws2_32).
- `build_all.bat` builds `dev\` and runs the tests (`run_test.bat`); `run_bench.bat` for a benchmark.
- Kernel variants: `run\nvcc_extra.txt` (e.g. `-DRGR_UNROLL=2`, `-DRGR_ROT=1`); default `RGR_UNROLL=11`.

Tests without a GPU (Linux): `tests/core_test.cpp` (GPU code path emulated on the CPU vs reference SHA3-256),
`tests/rg_stub.cpp` (CPU stand-in for `rgpu.dll`), `tests/fake_node.py` (fake rigord that checks every submitted
block: structure, merkle root, time window, proof of work), `tools/nvrtc_check.py` (NVRTC + ptxas register report).

```
g++ -O2 -std=gnu++17 -Isrc -Igpu -o rgrminer_stub src/main.cpp tests/rg_stub.cpp -pthread
./rgrminer_stub --test --tpb 256 --batch 20
python3 tests/fake_node.py 18761 1f00ffff rg1qtest &
./rgrminer_stub --rpc-port 18761 --rpc-user tuser --rpc-pass tpass --address rg1qtest --tpb 256 --batch 20
```

## License

GPL-3.0 (see `LICENSE`). The Keccak helpers come from the sha3-256t core of FjarMiner (GPL-3.0).
RgrMiner is an independent project, not affiliated with the RIGOR developers. Mining gives no guaranteed reward.
