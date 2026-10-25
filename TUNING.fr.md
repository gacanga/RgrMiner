# RgrMiner — notes de réglage (NVIDIA P104-100 8 Go)

> English: [TUNING.md](TUNING.md)

Mesures du 25/10/2026 avec RgrMiner 0.1.1 sur cinq cartes identiques. Vos cartes, leur refroidissement et votre
alimentation peuvent se comporter autrement : changez un réglage à la fois et lancez `test.bat` après chaque
changement.

## Matériel et logiciel

| Élément | Valeur |
|---|---|
| Carte | **NVIDIA P104-100, 8 Go** (8192 Mio), Pascal GP104, 15 SM, compute capability 6.1 |
| Limite de puissance par défaut / min / max | 180 W / 90 W / 217 W (`nvidia-smi --query-gpu=power.default_limit,power.min_limit,power.max_limit --format=csv`) |
| Pilote | NVIDIA 581.42 (Windows 10/11 x64) |
| Compilation | CUDA 12.9, code natif sm_61, 512 threads par bloc, 2^27 nonces par lancement |
| Décalages | Décalages de fréquence P0 appliqués via NVAPI (même sens que les curseurs cœur / mémoire des outils d'overclocking habituels) |

## Pourquoi ces réglages

- SHA3 tourne entièrement dans les registres : chaque carte n'utilise qu'environ 97 Mio de mémoire, et **la
  vitesse de la mémoire ne compte pas**. Baisser la fréquence mémoire libère de la puissance pour le cœur.
- À chaque palier ci-dessous, les cartes étaient **limitées par leur limite de puissance** : le hashrate suit la
  fréquence du cœur que le budget de puissance permet.
- Au-delà de +200 MHz sur le cœur, ces cartes ne sont plus fiables : à +300 MHz, trois cartes sur cinq donnaient
  des hashs faux ou des erreurs « illegal memory access » dans `--test`. RgrMiner revérifie chaque GPU contre le CPU
  toutes les 10 minutes (code de sortie 6 en cas d'écart), mais un pilote figé ne se récupère pas toujours sans
  redémarrage.

## Résultats (5 cartes, banc de 45 s par palier, `--test` réussi après chaque palier)

| Décalage cœur / décalage mémoire / limite | Total | Gain |
|---|---|---|
| 0 / 0 / 150 W (fréquences d'origine, 150 W) | ≈ 3 355 MH/s | — |
| 0 / −1000 MHz / 150 W | 3 442 MH/s | +2,6 % |
| 0 / −1000 MHz / 180 W (limite par défaut) | 3 627 MH/s | +8,1 % |
| +100 / −1000 MHz / 180 W | 3 765 MH/s | +12,2 % |
| +200 / −1000 MHz / 180 W | 3 912 MH/s | +16,6 % |
| **+200 / −1000 MHz / 200 W** | **4 008 MH/s** | **+19,5 %** |

Réglage final, par carte : 792 / 808 / 812 / 796 / 800 MH/s ; 194–200 W ; cœur à 1923–1974 MHz ; mémoire affichée
à 2999 MHz au lieu de 5005 MHz par `nvidia-smi` ; 58–72 °C. Les cinq cartes sont à moins de 3 % l'une de l'autre :
elles ont toutes le même réglage.

Nous nous sommes arrêtés à 200 W au lieu du maximum de 217 W pour garder une charge raisonnable sur les
alimentations. Dimensionnez votre alimentation (et les risers) pour la limite choisie.

## Variantes du noyau (une carte, +200 / −1000 / 200 W)

| Compilation du noyau | MH/s |
|---|---|
| `RGR_UNROLL=2` (0.1.0) | 798–799 |
| **`RGR_UNROLL=11` (défaut de la 0.1.1)** | **802–803** |
| `RGR_UNROLL=22` (boucle entièrement déroulée) | 461 |
| `RGR_ROT=1` (rotations par décalage + addition) | 797 |
| `RGR_UNROLL=11`, `--tpb 256` | 787 |

Une boucle entièrement déroulée est environ 42 % plus lente : le code ne tient plus dans le cache d'instructions.
Le noyau est proche de la limite de calcul de la carte : la fréquence et la puissance sont les leviers restants.

## Appliquer les réglages

1. Limite de puissance (administrateur) : `nvidia-smi -i 0,1,2,3,4 -pl 200`
2. Cœur +200 MHz, mémoire −1000 MHz avec votre outil d'overclocking.
3. `test.bat` (doit finir par `TEST ALL OK`), puis `bench.bat`, puis le minage. Si une carte affiche
   `WRONG candidate` ou `SELF-CHECK FAILED`, descendez son décalage cœur à +100, puis à 0.
