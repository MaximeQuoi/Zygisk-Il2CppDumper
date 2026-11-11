# Zygisk-Il2CppDumper v2

Module Zygisk qui **gèle le processus avant de dumper** les métadonnées il2cpp.

Cette v2 est basée sur [Perfare/Zygisk-Il2CppDumper](https://github.com/Perfare/Zygisk-Il2CppDumper).
Elle conserve le fonctionnement d'origine (dump de `dump.cs` à l'exécution, support du
native bridge ARM sur x86) et ajoute un gel complet des threads avant la lecture des
métadonnées.

---

## Ce qui change par rapport à la v1

| | v1 | v2 |
|---|---|---|
| Gel du processus | non | **oui**, tous les threads sauf celui du dump |
| Snapshot cohérent | non | oui, le jeu ne peut plus modifier les métadonnées pendant le dump |
| Écriture du fichier | directe dans `dump.cs` | fichier temporaire puis `rename()` atomique |
| Mémoire pendant le dump | accumulation de toutes les classes dans un `vector<string>` | écriture en flux, mémoire quasi constante |
| Initialisation il2cpp | à la volée pendant le dump | pré-parcours à froid avant le gel, puis lecture seule |
| Attente du runtime | boucle sur `il2cpp_is_vm_thread(nullptr)` | attente de `il2cpp_domain_get_assemblies()` non vide |
| Configuration | `game.h` | `game.h` + `config.h` (gel, timeout, délai, fichier de sortie) |
| Build CI | actions v3 | actions v4, options de gel configurables |

---

## Nouveautés v2.2 → v2.5

- **Gel par ptrace via le companion root Zygisk** (méthode par défaut). Le companion,
  lancé en root par Magisk, arrête chaque thread du jeu sauf celui du dump avec
  `PTRACE_SEIZE` + `PTRACE_INTERRUPT`. L'arrêt se fait dans le noyau, sans coopération
  des threads : il fonctionne aussi sous native bridge (Houdini), où les signaux
  n'atteignent pas les threads x86. Le module et le companion communiquent par un bloc
  de contrôle en mémoire, lu et écrit par le companion avec `process_vm_readv`/`writev`
  (en API Zygisk v2, les sockets ouvertes avant la spécialisation sont fermées).
  La barrière de signaux décrite plus bas ne sert plus qu'en secours, sur un vrai
  appareil ARM.
- **Watchdog** : quelle que soit la méthode, le jeu n'est jamais gelé plus de
  `DUMPER_FREEZE_MAX_MS` (8 s). L'API il2cpp prend un verrou global même sur des classes
  déjà initialisées : si un thread gelé le tenait, le dump resterait bloqué. Le watchdog
  relâche alors le processus et le dump se termine sur le jeu vivant.
- **Dump mémoire** (`DUMPER_MEMORY_DUMP`). Quand un protecteur retire les exports il2cpp
  (par exemple NetEase HTProtect), `dump.cs` est impossible. Le module copie alors dans
  `files/il2cpp-dump/` l'image déchiffrée de `libil2cpp.so` et le `global-metadata`
  déchiffré trouvé en mémoire. La metadata est reconnue par son magic, ou, si celui-ci a
  été effacé, par la disposition de ses sections (contiguës, la première juste après le
  header) ; le header est ensuite restauré. Il suffit de passer les deux fichiers à
  Il2CppDumper, avec l'adresse du dump indiquée dans le nom du `.so`. Si le header ELF
  a lui aussi été effacé en mémoire, il faut le reconstruire avant.
- **Native bridge** : la copie ARM du module écrit elle aussi dans `dumper.log`, et
  chaque copie indique sa version et son ABI.
- Les attentes reprennent après une interruption par un signal (`EINTR`), fréquente
  sous Houdini, au lieu de se terminer trop tôt.
- `scripts/build-ndk.sh <ndk r25c>` compile le module avec le seul NDK, sans Java,
  Gradle ni CMake.

## Comment fonctionne le gel

`SIGSTOP` ne peut pas être utilisé : sous Linux, un arrêt de type job-control est un
**group stop**. Envoyer `SIGSTOP` à un seul thread arrête tout le processus, y compris
le thread qui doit écrire le dump.

La v2 utilise donc une barrière de signaux :

1. Le module installe un handler pour un signal temps réel libre (`SIGRTMIN+n`).
   Il choisit un signal qui n'est pas déjà utilisé par l'application et qui n'est
   bloqué par aucun thread.
2. Juste avant de parcourir les métadonnées, il envoie ce signal à tous les threads
   du processus, sauf le thread courant.
3. Chaque thread entre dans le handler, incrémente un compteur, puis se bloque sur
   un futex privé.
4. Quand tous les threads sont parqués, le dump lit les métadonnées il2cpp. Le reste
   du processus est figé : aucune classe, aucun assembly, aucun objet ne peut bouger.
5. À la fin du dump (ou en cas d'erreur), le futex est réveillé et chaque thread
   reprend exactement là où il avait été interrompu.

Le handler reste installé après le premier gel, mais désarmé. Cela évite qu'un signal
resté en attente (thread qui bloquait ce signal) ne tue le processus plus tard.

Pendant le gel, le thread du dump n'appelle ni `malloc` ni aucune fonction susceptible
d'allouer : le fichier et son en-tête sont préparés avant le gel, l'énumération des
threads et la lecture de `/proc/self/task/<tid>/status` se font par appels système
bruts avec des tampons statiques, et les logs sont différés après le réveil. Cela évite
un interblocage si un thread parqué détenait le verrou de l'allocateur libc.

De plus, un **pré-parcours à froid** (`DUMPER_WARMUP_BEFORE_FREEZE`) parcourt les
métadonnées une première fois avant le gel. Il force il2cpp à construire ses tables de
méthodes, ses classes génériques et ses champs statiques paresseux. Le parcours effectué
pendant le gel ne fait alors que lire de la mémoire, sans allocation.

Pour les jeux antérieurs à Unity 2018.3, la résolution des types par réflexion
(`Assembly.Load` / `GetTypes`) est elle aussi effectuée **avant** le gel ; le gel ne
fait ensuite que parcourir les classes déjà résolues.

Aucun `ptrace`, aucun processus séparé, aucune permission supplémentaire : le module
reste dans le processus de l'application.

---

## Compilation

### GitHub Actions

1. Forkez le dépôt.
2. Onglet **Actions** → workflow **Build** → **Run workflow**.
3. Renseignez :
   - `package_name` : le nom de paquet du jeu, par exemple `com.example.game` ;
   - `freeze` : `1` pour geler avant le dump, `0` pour l'ancien comportement ;
   - `freeze_timeout_ms` : temps d'attente maximal du gel (3000 ms par défaut) ;
   - `delay_before_dump_ms` : délai supplémentaire avant le gel (0 par défaut).
4. Téléchargez l'artefact `zygisk-il2cppdumper-v2`, qui contient le zip Magisk.

### Android Studio / ligne de commande

1. Modifiez `module/src/main/cpp/game.h` et remplacez `com.game.packagename`.
2. Ajustez si besoin `module/src/main/cpp/config.h`.
3. Compilez :

```bash
./gradlew :module:assembleRelease
```

Le zip est généré dans `out/`.

---

## Installation et utilisation

1. Installez [Magisk](https://github.com/topjohnwu/Magisk) v24 ou plus récent et
   activez Zygisk.
2. Installez le zip du module dans Magisk, puis redémarrez.
3. Lancez le jeu ciblé. Le processus se fige le temps du dump, puis reprend.
4. Récupérez le fichier :

```bash
adb shell su -c "cp /data/data/<package>/files/dump.cs /sdcard/Download/dump.cs"
```

ou, avec les droits root :

```bash
adb pull /data/data/<package>/files/dump.cs
```

Le dump est aussi utilisable tel quel avec [Il2CppDumper](https://github.com/Perfare/Il2CppDumper)
accompagné du `libil2cpp.so` et du `global-metadata.dat` du jeu.

### Logs

```bash
adb logcat -s Il2CppDumperV2
```

Exemple :

```
I Il2CppDumperV2: detected target: com.example.game
I Il2CppDumperV2: libil2cpp.so found after 1500 ms
I Il2CppDumperV2: il2cpp ready: 42 assembly(ies)
I Il2CppDumperV2: dumping...
I Il2CppDumperV2: Version greater than 2018.3
I Il2CppDumperV2: warming up il2cpp metadata...
I Il2CppDumperV2: warm-up done
I Il2CppDumperV2: freezer: using signal 41 (SIGRTMIN+6), 0 thread(s) block it
I Il2CppDumperV2: freezer: 37 thread(s) resumed
I Il2CppDumperV2: freezer: 37/37 thread(s) parked, 0 skipped, 0 failed
I Il2CppDumperV2: freeze report: OK, signal=41, parked=37/37, skipped=0, failed=0, dump=1200 ms
I Il2CppDumperV2: dump done: /data/data/com.example.game/files/dump.cs
```

Le même journal est écrit dans `/data/data/<package>/files/dumper.log`. Il est donc
consultable sans adb, avec un gestionnaire de fichiers root, et il permet de savoir
si le gel a réellement eu lieu :

- `freeze report: OK` : les threads ont bien été garés pendant le dump ;
- `freeze report: NOT FROZEN` : le gel n'a pas pu s'appliquer, le dump reste valable
  mais moins cohérent ;
- absence de ligne `freeze report` : le dump n'a pas atteint l'étape du gel (voir
  les lignes précédentes du journal).

Par défaut, le processus reste figé environ 3 secondes de plus que le dump
(`DUMPER_FREEZE_HOLD_MS`), afin que le gel soit clairement visible.

---

## Configuration (`module/src/main/cpp/config.h`)

| Macro | Défaut | Rôle |
|---|---|---|
| `DUMPER_FREEZE_BEFORE_DUMP` | `1` | `1` : gel avant le dump, `0` : comportement v1 |
| `DUMPER_FREEZE_TIMEOUT_MS` | `3000` | attente maximale du gel |
| `DUMPER_FREEZE_SIGNAL_OFFSET` | `6` | premier signal candidat, relatif à `SIGRTMIN` |
| `DUMPER_FREEZE_HOLD_MS` | `3000` | maintien du gel après l'arrêt des threads (0 = désactivé) |
| `DUMPER_DELAY_BEFORE_DUMP_MS` | `0` | délai après l'attache il2cpp avant le gel |
| `DUMPER_WARMUP_BEFORE_FREEZE` | `1` | pré-parcours à froid pour forcer l'initialisation il2cpp avant le gel |
| `DUMPER_OUTPUT_FILE` | `"dump.cs"` | nom du fichier écrit dans `files/` |
| `DUMPER_LIB_WAIT_MS` | `60000` | attente maximale de `libil2cpp.so` |
| `DUMPER_IL2CPP_WAIT_MS` | `60000` | attente maximale du runtime il2cpp |

---

## Comportement attendu et limites

- **L'application paraît figée** pendant le dump. Sur un gros jeu, cela peut dépasser
  le délai d'ANR d'Android : une boîte « L'application ne répond pas » peut apparaître.
  Ne répondez pas et attendez la fin du dump ; l'application reprend ensuite.
- Les threads qui bloquent le signal choisi ne peuvent pas être parqués. Le module les
  ignore et le signale dans les logs. Le dump continue avec les autres threads figés.
- Un thread dans un appel système non interruptible (état `D`) ne peut pas être parqué
  avant la fin de cet appel. Le timeout évite de bloquer indéfiniment.
- Le gel étant préemptif, un thread peut être figé en plein `malloc`, en tenant un verrou
  libc. La v2 évite donc toute allocation dans le freezer pendant le gel et prépare le
  fichier de sortie avant de figer. Les appels il2cpp restent en lecture seule, mais un
  blocage interne au runtime ne peut pas être totalement exclu ; relancez le jeu si cela
  arrive.
- Certains jeux avec protection anti-debug/anti-tamper peuvent détecter le signal ou le
  module. Le gel n'est pas une technique de furtivité.

---

## Structure du projet

```
module/src/main/cpp/
├── main.cpp          # entrée Zygisk, détection du paquet
├── hack.cpp/.h       # attente de libil2cpp.so, native bridge, orchestration
├── freeze.cpp/.h     # barrière de gel des threads (nouveau)
├── il2cpp_dump.cpp/.h# résolution de l'API et écriture de dump.cs
├── config.h          # configuration de compilation
├── game.h            # nom du paquet cible
├── log.h             # logs Android
├── xdl/              # xDL (chargement de symboles)
└── zygisk.hpp        # API Zygisk
```

---

## Crédits

- Projet original : [Perfare/Zygisk-Il2CppDumper](https://github.com/Perfare/Zygisk-Il2CppDumper)
- xDL : [HexHacking Team](https://github.com/hexhacking/xDL)
- Licence : MIT (voir `LICENSE`)

---

## Quick start (English)

This v2 freezes every thread of the target process (except the dumper thread) right
before reading il2cpp metadata. It uses a real-time signal + private futex barrier
instead of `SIGSTOP`/`ptrace`.

Build with the **Build** workflow: set `package_name`, optionally `freeze=1`,
`freeze_timeout_ms=3000` and `delay_before_dump_ms=0`. Install the resulting Magisk
zip, launch the game, then pull `/data/data/<package>/files/dump.cs`.
