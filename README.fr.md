# RgrMiner

Mineur GPU NVIDIA **solo** open source pour **RIGOR** (ORU, [rigorchain.com](https://www.rigorchain.com/)), preuve de
travail `rgrpow01` / OpuHash, **sans frais de développeur**.

> English: [README.md](README.md)

La version officielle RIGOR v0.2 mainnet ne fournit qu'un mineur CPU. RgrMiner calcule la même preuve de travail sur
le GPU et dialogue avec **votre propre nœud RIGOR local** comme le mineur officiel (`getblocktemplate` / `submitblock`
sur `127.0.0.1:8761`). Il n'y a pas de pool : chaque bloc trouvé paie 128 ORU (+ frais) à votre adresse.

## Preuve de travail

```
pow = SHA3-256( "RGRPOW01" || en-tête 80 octets )   SHA3-256 FIPS 202 (padding 0x06, pas Keccak-256)
valide  <=>  pow (lu comme un uint256 little-endian) <= cible(nBits)
```

La préimage de 88 octets tient dans un seul bloc de 136 octets : un nonce coûte exactement **une permutation
Keccak-f[1600]**. L'identifiant du bloc, la racine de Merkle et les txid restent en SHA-256d. Référence :
`docs/specification/SPEC-008-PROOF-OF-WORK.md` et `docs/RGRPOW01_REFERENCE_VECTOR_v1.0.md` du
[dépôt rigorchain](https://github.com/rigorchain/rigorchain).

## Fonctionnement

- **Moteur GPU** (`gpu/rgr_core.h`, `gpu/rgr_gpu.cu` → `rgpu.dll`) : chaque lane est tenue en deux mots de 32 bits
  (funnel shifts, LOP3 sur Pascal). La ronde 0 est précalculée sur l'hôte pour les 14 lanes indépendantes du nonce ;
  seules les 11 lanes atteintes par le nonce (la lane 10, les colonnes 1 et 4 via theta) sont recalculées. La
  dernière ronde ne calcule que la lane 3 (les 64 bits de poids fort), comparée à la cible ; chaque candidat est
  recalculé sur le CPU sur 256 bits avant d'être soumis.
- **Client du nœud** (`src/main.cpp`, `src/rpc.h`) : `getblocktemplate` avec `rules: ["segwit"]` et votre
  `coinbase_address`. Les nœuds RIGOR construisent eux-mêmes la coinbase (`rigor_coinbase_immutable`) : RgrMiner
  recopie `default_coinbase_tx`, vérifie son txid et la racine de Merkle avec le nœud, et ne reconstruit jamais de
  coinbase.
- **Il n'y a pas d'extranonce** (la coinbase ne change pas) : chaque GPU prend ses propres valeurs de `nTime`
  (heure du modèle + un compteur de créneaux, un créneau = 2^32 nonces). Deux GPU ne calculent jamais le même
  en-tête. nTime ne dépasse jamais le `maxtime` du modèle ni une heure d'avance sur l'horloge.
- **Auto-contrôle** : au démarrage puis toutes les 10 minutes, chaque GPU recalcule 4 096 hashs comparés à la
  référence CPU ; un écart (overclock instable...) arrête le mineur avec le code 6.

## Prérequis

- Windows 10/11 x64, carte NVIDIA Pascal (sm_61 : P104-100, GTX 1070/1080) ou plus récente, pilote NVIDIA compatible
  CUDA 12 (R525 ou plus récent). Les cartes plus récentes exécutent le PTX via le JIT du pilote (fonctionne, non
  optimisé).
- Le wallet / nœud officiel **RIGOR v0.2 mainnet** lancé sur le même ordinateur et synchronisé
  ([release](https://github.com/rigorchain/rigorchain/releases/tag/v0.2-mainnet)).

Mesure : environ **670 MH/s par P104-100** aux fréquences d'origine.

## Démarrage rapide (zip de la release)

1. Lancez le wallet / nœud RIGOR mainnet, attendez la synchronisation, copiez une adresse de réception (`rg1q...`).
2. Modifiez `rgrminer.conf` : `address = rg1q...` et `auth-file = <dossier data du nœud>\rpc-auth.conf`
   (fichier écrit par le nœud à son premier démarrage ; vous pouvez utiliser `rpc-user` / `rpc-pass` à la place).
3. Lancez `test.bat` (tous les GPU contre la référence CPU), puis `check_node.bat`
   (le nœud valide un bloc complet de RgrMiner en mode `proposal` et les 5 derniers blocs de la chaîne sont revérifiés).
4. Lancez `start_rgr.bat`. Journal dans `rgrminer.log`, une ligne de statistiques par minute.

```
rgrminer --config rgrminer.conf [--gpus 0,1] [--log fichier]
         --test | --bench 60 | --propose | --check-chain 5 | --help
```

Gardez le RPC du nœud sur `127.0.0.1` uniquement. N'exposez jamais le port 8761.

## Compilation

- DLL GPU : CUDA 12.9 + Visual Studio 2022 → `windows\build_gpu_dll.bat` (sm_61 + PTX).
- Hôte : MSYS2 MINGW64 g++ → `windows/build_host.sh` (lié à `rgpu.dll`, statique, seulement ws2_32).
- `build_all.bat` compile dans `dev\` et lance les tests (`run_test.bat`) ; `run_bench.bat` pour un banc d'essai.
- Variantes du noyau : `run\nvcc_extra.txt` (par ex. `-DRGR_UNROLL=1`, `-DRGR_ROT=1`).

Tests sans GPU (Linux) : `tests/core_test.cpp` (chemin GPU émulé sur le CPU contre la référence SHA3-256),
`tests/rg_stub.cpp` (remplaçant CPU de `rgpu.dll`), `tests/fake_node.py` (faux rigord qui vérifie chaque bloc soumis :
structure, racine de Merkle, fenêtre de temps, preuve de travail), `tools/nvrtc_check.py` (NVRTC + registres ptxas).

## Licence

GPL-3.0 (voir `LICENSE`). Les fonctions Keccak viennent du cœur sha3-256t de FjarMiner (GPL-3.0).
RgrMiner est un projet indépendant, sans lien avec les développeurs de RIGOR. Le minage ne garantit aucun gain.
