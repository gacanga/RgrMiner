# RgrMiner

Open-source NVIDIA GPU **solo** miner for **RIGOR** (ORU, [rigorchain.com](https://www.rigorchain.com/)), proof of work
`rgrpow01` / OpuHash — **no developer fee**.

> Français : [README.fr.md](README.fr.md)

The official RIGOR v0.2 mainnet release only ships a CPU miner. RgrMiner computes the same proof of work on the GPU and
talks to **your own local RIGOR node** exactly like the official miner does (`getblocktemplate` / `submitblock` on
`127.0.0.1:8761`): every block you find pays 128 ORU (+ fees) to your address.

Since 0.2.0 it can also mine on the **Gozero pool** ([pool.gozero.trade/ORU](https://pool.gozero.trade/ORU)), which shares
the rewards between its miners (see [Pool mode](#pool-mode-gozero)).

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

## Pool mode (Gozero)

The Gozero pool does not use Stratum: it serves RIGOR block templates over HTTPS (JSON-RPC, "pooled-GBT"), with a
coinbase paying the pool and a share target. RgrMiner submits every share as a full block.

- Endpoint `https://rgr.pool.gozero.trade/rpc`, user `<your rg1q address>.<worker>`, password `x`. Only your public
  address is needed.
- `getblocktemplate` with `gozero_pool: 1` and `gozero_share_work` (requested hashes per share, `share-work`,
  default 1e10 ≈ one share every 12 s on a P104-100); shares go back with `submitblock [block, {workid}]`.
- Choosing `share-work`: the pool estimates your hashrate from the shares it receives, so few shares per second
  make its graph jumpy. About 0.5 to 1 share per second is a good target: share-work ≈ total hashrate (H/s) × 1.25,
  for example 5e9 for a rig of five P104-100 (about 4 GH/s). Rewards do not depend on it: the pool pays for the work.
- TLS: the pool certificate and host name are verified against `ca-bundle.crt` (shipped in the zip); a connection
  that cannot be verified is refused.
- Shares are sent by separate threads (`share-threads`, default 4, each submission is one HTTPS request that can take
  about 1 s): the GPUs never wait for the network. Shares found on a previous block or on an expired job are dropped
  instead of being sent. The stats line shows the average submission time and the longest queue: if the queue keeps
  growing, raise `share-work` or `share-threads`.
- Optional: with a local RIGOR node (`auth-file`), RgrMiner checks the node tip every 250 ms and asks the pool for new
  work as soon as a block is found; otherwise it polls the pool every 3 s.
- Pool rules (fee, payout scheme, minimum payment, confirmations) are set by the pool: read
  [pool.gozero.trade/ORU](https://pool.gozero.trade/ORU) before mining. RgrMiner is not affiliated with the pool.

Quick start: edit `rgrminer_pool.conf` (`pool-user = rg1q....rig01`), run `test.bat`, then `start_pool.bat`.

## Requirements

- Windows 10/11 x64, NVIDIA Pascal (sm_61: P104-100, GTX 1070/1080) or newer GPU, NVIDIA driver with CUDA 12
  support (R525 or newer). Newer GPUs run the PTX through the driver JIT (works, not tuned).
- Solo mining: the official **RIGOR v0.2 mainnet** wallet / node running on the same computer and synchronized
  ([release](https://github.com/rigorchain/rigorchain/releases/tag/v0.2-mainnet)). Pool mining: only a receive address
  (a local node is optional).

Measured: about **670 MH/s per P104-100** at stock clocks, about **800 MH/s** with the settings of [TUNING.md](TUNING.md)
(core +200 MHz, memory −1000 MHz, 200 W).

## Quick start, solo (release zip)

1. Start the RIGOR mainnet wallet / node, wait until it is synchronized, copy a receive address (`rg1q...`).
2. Edit `rgrminer.conf`: `address = rg1q...` and `auth-file = <node data folder>\rpc-auth.conf`
   (the file the node writes on its first start; you can use `rpc-user` / `rpc-pass` instead).
3. Run `test.bat` (all GPUs against the CPU reference), then `check_node.bat`
   (the node validates a full RgrMiner block in `proposal` mode and the last 5 chain blocks are re-checked).
4. Run `start_rgr.bat`. Log in `rgrminer.log`; one statistics line per minute.

```
rgrminer --config rgrminer.conf [--gpus 0,1] [--log file]
         --test | --bench 60 | --propose | --check-chain 5 | --help
rgrminer --config rgrminer_pool.conf        (pool mode: --pool-url, --pool-user, --share-work, --share-threads, --ca-file)
```

Keep the node RPC on `127.0.0.1` only. Never expose port 8761.

## Building

- GPU DLL: CUDA 12.9 + Visual Studio 2022 → `windows\build_gpu_dll.bat` (sm_61 + PTX).
- Host: MSYS2 MINGW64 g++ + OpenSSL (`mingw-w64-x86_64-openssl`) → `windows/build_host.sh` (links `rgpu.dll`,
  static); `-DRGR_NO_TLS` builds a solo-only host without OpenSSL.
- `build_all.bat` builds `dev\` and runs the tests (`run_test.bat`); `run_bench.bat` for a benchmark.
- Kernel variants: `run\nvcc_extra.txt` (e.g. `-DRGR_UNROLL=2`, `-DRGR_ROT=1`); default `RGR_UNROLL=11`.

Tests without a GPU (Linux): `tests/core_test.cpp` (GPU code path emulated on the CPU vs reference SHA3-256),
`tests/rg_stub.cpp` (CPU stand-in for `rgpu.dll`), `tests/fake_node.py` (fake rigord that checks every submitted
block: structure, merkle root, time window, proof of work), `tests/fake_pool.py` (fake Gozero pool over HTTPS with a
self-signed certificate: auth, workid, block structure, share target), `tools/nvrtc_check.py` (NVRTC + ptxas register report).

```
g++ -O2 -std=gnu++17 -Isrc -Igpu -o rgrminer_stub src/main.cpp tests/rg_stub.cpp -lssl -lcrypto -pthread
./rgrminer_stub --test --tpb 256 --batch 20
python3 tests/fake_node.py 18761 1f00ffff rg1qtest &
./rgrminer_stub --rpc-port 18761 --rpc-user tuser --rpc-pass tpass --address rg1qtest --tpb 256 --batch 20
```

## License

GPL-3.0 (see `LICENSE`). The Keccak helpers come from the sha3-256t core of FjarMiner (GPL-3.0). The Windows host is
statically linked with OpenSSL 3 (Apache-2.0).
RgrMiner is an independent project, not affiliated with the RIGOR developers nor with the Gozero pool. Mining gives no guaranteed reward.
