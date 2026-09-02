# VU accuracy backport from armsx2 — STANDALONE (no EE-FPU-format wall)

Branch: `feat/vu-accuracy-standalone` (sur `feat/yaps2-jit`).
Source: `armsx2/local-master` (repo `/home/beudbeud/Documents/Dev/libretro-armsx2`), auteur pstef.

## VERDICT (2026-09-02) — le mur EE-FPU-format N'EXISTE PAS pour le VU

La conclusion précédente (`docs/VU-ACCURACY-PORT.md` : « prérequis Phase −1
refonte FPU EE invasive ») était **fausse**. Elle venait du **banc de tests**
upstream, pas du code runtime. Preuves tracées :

- Le modèle FMAC du VU (`namespace EeFpuModel`) est **domaine binaire pur**
  (`u32`→`u32`). Ses helpers (`eeToDouble(u32)`, `eeMulRound`, `eeGuardedSum`,
  `eeDivide`, `eeSqrtBits`, `eeRoundToSingle`, `eeRoundsOutOfRange`,
  `kEeMinNormal`) sont du calcul entier/double autonome.
- `git grep -nE 'EeFpuFormat|FPRreg::UD|eeFprNarrowBits|g_eeFprSlotsRelocated|SetWord'`
  sur `iCOP2-arm64.cpp, microVU_Upper/Lower-arm64.inl, microVU-arm64.h,
  VUops.cpp, VUflags.cpp/.h, VU0.cpp` = **ZÉRO**. Le chemin VU est relocation-free.
- La relocalisation FPR (`g_eeFprSlotsRelocated`, FPU.cpp:69-84) est une optim de
  stockage registre du **recompilateur COP1 EE** — unité différente du FMAC VU,
  qui partage seulement le *modèle arithmétique* (par valeur, sur des mots).
- Le couplage `FPRreg::UD`/`eeFprNarrowBits` n'existait QUE dans la harness ctest
  (`RecompilerTestEnvironment`, `vu_mul_deficit`) qu'on remplace par un check
  autonome (voir Validation).

**Piège cherry-pick** : les 2 forks ont fait des transplants yaps2 INDÉPENDANTS
(nous `8f73b5bc2b`, armsx2 `ac8258a950`). Le merge-base `3df128d9f9` n'a AUCUN
`pcsx2/arm64/*`. Donc `git cherry-pick` des commits accuracy NE S'APPLIQUE PAS —
c'est un **port manuel/sémantique**.

## Plan ordonné

### Step 1 — Headers modèle (FAIT, commit e3c40c4d91)
Adds purs verbatim d'armsx2 : `pcsx2/EeFpuModel.h`, `VuMulBand.h`, `VuEfuModel.h`,
`arm64/VuFmacFlags-arm64.h`, `arm64/EeFpuModelCall-arm64.h` (+ CMakeLists).
Bonus : GameDB Fate NM00048 `eeClampMode: 3`.

### Step 2 — Modèle bit-domain (FAIT, commit d764dff7e4)
Réalisé en TU AUTONOME `pcsx2/EeFpuModel.cpp` (pas dans FPU.cpp) — plus propre,
zéro risque pour nos opcodes EE-FPU. Extraction verbatim (brace-matcher) des
helpers ee* + EeFpuModel, -ffp-contract=off. Compile clean.

### Step 2-OLD (approche abandonnée : injection dans FPU.cpp)
Notre FPU.cpp (446 l.) ne diverge que de 55 l. de la base ; armsx2 ajoute ~1100.
Porter UNIQUEMENT, en tant que fonctions autonomes neuves :
- helpers file-scope : `eeToDouble`, `eeRoundsOutOfRange`, `kEeMinNormal`,
  `eeMulRound`, + le `namespace EeFpuModel` (queue de FPU.cpp d'armsx2, l.1262-1318 :
  `MakeResult/AddSub/Mul/MulAccumulate/Divide/SqrtBits/RecipSqrt`).
- helpers `namespace R5900::Interpreter::OpcodeImpl::COP1` : `eeGuardedSum`,
  `eeRoundToSingle`, `eeMulRound`, `eeDivideCap`, `eeDivideSignificand`,
  `eeDivide`, `eeSqrtBits` (+ transitifs).
- **NE PAS porter** : `g_eeFprSlotsRelocated` (69-84), aucun `FPRreg::UD`/`SetWord`,
  ni les opcodes COP1 EE d'armsx2 (ils lisent le FPR en `SetWord/Word` — on garde
  NOS opcodes EE-FPU en `UL`). Les helpers sont neufs → pas de collision de noms
  (notre FPU.cpp est l'ancienne version, sans la famille ee*).
- Build-check : les nouvelles fonctions compilent, inutilisées jusqu'au step 3.

### Step 3 — Interpréteur (FAIT, commit 4b81876e29)
VUflags.h/.cpp + VU0.cpp = diffs armsx2 appliqués clean (ours==base). VUops.cpp
= merge 3-way (nos _vuSQRT/_vuRSQRT superseded par le modèle armsx2). Implémente
VuMulBand + VuEfuModel. eeMulOneUlpLow appelé en ::global. Compile+linke clean.

### Step 3-DETAIL (référence)
Signatures neuves + appelants (nos fichiers très peu divergents de la base) :
- `VUflags.h/.cpp` : `VU_MACx_UPDATE(VU, u32, bool overflow=false, bool underflow=false)`,
  `VU_STAT_UPDATE(VU, u32 extraSticky=0)`.
- `VUops.cpp` (+672 l., nous ~38 l. off base) : appelants FMAC → `EeFpuModel::*` +
  nouvelle signature MAC ; porte aussi `vuMulShortTailBand*` + corps `VuEfuModel::*`.
- `VU0.cpp` (98 l.).
- Build-check + test unitaire du modèle (voir Validation tier 1).

### Step 4 — Émetteurs arm64 (ZONE DE DANGER — merge à la main)
armsx2 vs son transplant : `iCOP2-arm64.cpp` +1240/−637, `microVU_Lower` +709,
`microVU_Upper` +448, `microVU-arm64.h` +26. NOTRE travail (AX-15 clone-mov fold
3-op NEON + GE COP2-mul) vit EXACTEMENT dans `mVU_FMACa`/COP2-mul où armsx2 insère
la bande one-ULP, la saturation FMAC EE-max, le guard-mask, les appels EFU/EDIV.
Ordre d'application dans ce set :
(a) saturation FMAC EE-max (`aae785033e`/`a1ab67fce5`) + signe du mot saturé (`4bf3e1e3e3`) ;
(b) MAC U/O (`65e445aba8`/`1c16fc4934`/`ec2359af58`) ;
(c) guard-mask (`d3c67ff28b`/`550da1e3d3`) ;
(d) one-ULP + `VuMulBand` out-of-line (`ac9a629311`,`5390256c4f`,`68ebf484be`,`ff5bcaf99a`) ;
(e) EDIV clampMode 4 (`3556d327f1`/`74c7561b4b`, zero-tests off host `c889fb6851`/`23173b0cd7`) ;
(f) EFU (`c5b55d7fcc`/`7c67183afd`).
**Ré-établir AX-15 à la main** par-dessus chaque : notre fold et la tail-branch
d'armsx2 réécrivent la même séquence de store du produit — fold AUTOUR des sites
de spill `armEmitEeFpuModelCall`, sans que l'un clobber les operand-stores de
l'autre (réf `f02cea753e` « move the multiply's operand stores off the hot path »).

### Step 4 — BLOCKER MESURÉ (02/09) : divergence de convention registre
Tentative de swap wholesale (prendre iCOP2/microVU d'armsx2 + ré-appliquer AX-15
après) : **échoue sur une divergence de convention registre COP2-macro**. Notre
`microVU_IR-arm64.h:105` réserve **q8-15** en neonCop2Mode (nos consts de clamp EE,
SL-13) ; armsx2 réserve **q25/q26** (`i != 25 && i != 26`). L'iCOP2 d'armsx2 (qu'on
prendrait) suppose la convention 25/26 → clash avec notre IR. Les 2 transplants
ont divergé là. Donc step 4 = VRAI hand-merge sémantique :
1. Réconcilier la convention registre COP2 (q8-15 nôtre vs q25/26 armsx2) —
   choisir une et adapter les consts de clamp + le spill du model-call.
2. Porter les helpers accuracy (mVUemitFmacUO / mVUemitSaturateAtMax / mVUclearUO,
   nouvelle signature mVUupdateFlags avec underflow/overflow) dans NOS émetteurs.
   Ils sont gated en interne par le mode → clampModes 0-3 gardent l'ancien
   comportement, mode 4 = accuracy. mVUemitFmacUO/SaturateAtMax vivent dans
   microVU_Upper d'armsx2.
3. Le VuMulBand out-of-line (store fs/ft/product, armEmitEeFpuModelCall(
   vuMulShortTailBandVu0/1), reload) au site du multiply — voir armsx2
   microVU_Upper l.133-140.
4. Ré-appliquer AX-15 (foldCloneMov aux sites Fmul par-élément) + GE (iCOP2).
5. Re-valider les 10 jeux (comportement VU change même gated).
Piège armsx2 IR +1 : `return !neonCop2Mode || (i != 25 && i != 26)` — leur
exclusion, à réconcilier avec notre `(i < 8 || i > 15)`.
Note: notre AX-15 dans l'IR reste inerte (note sans fold) si les émetteurs
n'appellent pas foldCloneMov — sûr mais perte perf jusqu'à ré-application.

### Step 5 — Config + GameDB
- `vuClampMode 4` : enum/rung (`662b114168`) ; picker Android (`b760dfd29c`) skippable.
- GameDB : SC2 NM00007 tester `vuClampMode: 4` ; Fate NM00048 `eeClampMode: 3` (FAIT step 1).

## Validation (SANS la harness upstream)
1. **Check unitaire autonome** (recommandé) : petit test liant SEULEMENT le
   `EeFpuModel` de FPU.cpp + les headers modèle (pas de recompilateur), asserte
   `Mul/AddSub/Divide` contre les tables autocase console d'armsx2
   (`autocases_vusat.h`, `autocases_vuflow.h`). Buildable justement PARCE QUE le
   modèle est autonome.
2. **A/B en jeu** : bug IA inerte SC2 arcade (cause candidate) + non-régression
   SC3/T4/SC2 (fenêtre combat 6 échantillons). AX-15 à re-valider ici (re-merge main).

## Backports secondaires autonomes (priorisés)
1. FAIT — Fate NM00048 `eeClampMode 3` (armsx2 `2412ea7041`).
2. `CLOCK_MONOTONIC` fallback si `CNTFRQ_EL0`=0 (`19dec9c211`) —
   `common/Linux/LnxMisc.cpp` +42, `VMManager.cpp` +8. Bas risque, assurance RPi5.
3. ASTC natif GLES/Vulkan (`dec880e5c8`, ~15 fichiers GS) — seulement si packs texture.
4. Pad macro timed-sequence (`dc975cda71`) — skip sauf demande.
NE PAS porter comme « autonome » : `8e4760858c` (EE rec fpu fallback filter) —
atterrit dans `iR5900-arm64.cpp` (notre fichier le plus divergent), fait partie
de la machinerie EE-FPU, pas du VU. Haut conflit, valeur arcade faible.

## Effort
Steps 1-3 mécaniques (~1-2 sessions), step 4 le vrai travail (merge main AX-15/GE,
~2-3 sessions), step 5 trivial. PAS de fondation EE-FPU, PAS de harness 100 fichiers.
