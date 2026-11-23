# Compiler, flasher, tester

Français · [English](../en/building.md)

## Prérequis

- **ESP-IDF 5.5.6** (cible `esp32s3`). C'est la version avec laquelle le firmware est
  compilé et testé, et celle de la CI.
- LVGL **9.2.2**, récupéré par le gestionnaire de composants ESP-IDF
  (`main/idf_component.yml`).
- Python 3 et gcc pour les tests PC.

## Compiler

```bash
idf.py set-target esp32s3          # une fois
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`idf.py flash` écrit aussi le paquet d'émojis (`assets/emoji.bin`) dans sa partition.

Options, en variables d'environnement à la compilation :

| Variable | Effet |
|---|---|
| `TW_EXTRA_COMPONENT_DIRS=/chemin` | dossier de composants en plus (par exemple une copie locale de LVGL pour compiler hors ligne, avec `IDF_COMPONENT_MANAGER=0`) |
| `TW_DIAG_BLE_ONLY=1`, `TW_DIAG_BARE=1` | builds de diagnostic (BLE seul / sans périphériques) |

Après **toute modification de `sdkconfig.defaults`**, supprimez le `sdkconfig` généré
(gardez `sdkconfig.defaults`) et recompilez ; sinon les anciennes valeurs restent.

## Organisation de la flash (16 Mo)

| Adresse | Taille | Contenu |
|---|---|---|
| `0x0` | | bootloader (avec retour arrière) |
| `0x8000` | | table des partitions |
| `0x9000` | 24 Ko | NVS (réglages, appairages, historique batterie, pas, Púca…) |
| `0xf000` | 4 Ko | calibration PHY |
| `0x10000` | 8 Ko | données OTA (slot qui démarre) |
| `0x20000` | 5 Mo | `ota_0` |
| `0x520000` | 5 Mo | `ota_1` |
| `0xa20000` | 3 Mo | `emoji` : paquet d'émojis en couleur (non touché par les mises à jour Wi-Fi) |
| `0xd20000` | 64 Ko | core dump |
| `0xd30000` | 2,8 Mo | `storage` (FAT) |

## Flasher

**Première installation** (efface tout, appairages compris) :

```bash
esptool.py --chip esp32s3 -b 921600 write_flash 0x0 twatch-ultra-full.bin
```

**Par USB en gardant appairages et réglages** :

```bash
esptool.py --chip esp32s3 -b 921600 write_flash \
  0x0 bootloader.bin 0x8000 partition-table.bin \
  0x10000 ota_data_initial.bin 0x20000 twatch-ultra-app.bin 0xa20000 emoji.bin
```

`ota_data_initial.bin` fait démarrer la montre sur `ota_0`, où l'application est écrite.

**Par Wi-Fi** : voir le README (*Réglages › Mise à jour*). La page de mise à jour est servie
par la montre à `http://192.168.4.1` ; elle suit la langue du navigateur (français ou
anglais). La vérification refuse les images d'un autre projet (le firmware 2020, par
exemple).

## Binaires publiés (CI)

`.github/workflows/build.yml` lance les tests PC et la compilation du firmware à chaque push.
Sur une étiquette `v*`, il publie :

- `twatch-ultra-full.bin` — image complète pour `0x0`, émojis compris ;
- `twatch-ultra-app.bin` — application seule (mise à jour Wi-Fi ou USB à `0x20000`) ;
- `LICENSE` et `THIRD_PARTY_NOTICES.md`.

L'artefact du workflow contient aussi le bootloader, la table des partitions, les données
OTA, le paquet d'émojis et l'ELF (pour décoder un plantage).

## Versions

La version est `PROJECT_VER` dans `CMakeLists.txt` ; la montre l'affiche dans *Réglages ›
À propos*. Chaque image de test diffusée incrémente le
dernier chiffre et reçoit une entrée dans `CHANGELOG.md`, et son image d'application est
nommée `twatch-ultra-X.Y.Z.bin` : deux images de test ne portent jamais le même nom. Une
version publiée est l'étiquette `vX.Y.Z` de cette version.

## Tests PC

```bash
host_tests/run.sh
```

Lance, sous AddressSanitizer et UBSan :

- l'analyseur ANCS (10 000 découpages aléatoires du flux Data Source), le nettoyage UTF-8
  (émojis gardés, écritures non dessinables retirées, formes trop longues et surrogates
  refusées), le stockage des notifications (déduplication, tri, capacité) ;
- le modèle du Púca (croissance, sommeil, négligence, pas → trèfles) ;
- `check_i18n.py` : chaque chaîne `_()`/`N_()` a une traduction anglaise avec les mêmes
  spécificateurs printf dans le même ordre.

## Dépannage

- **Plantage** : le core dump reste en flash.
  `esp-coredump info_corefile build/twatch_ultra.elf`.
- **BLE sans ordinateur** : *Réglages › À propos* affiche un journal BLE horodaté, les
  compteurs de réveil, le tas, la PSRAM et les registres des rails d'alimentation.
- **Batterie** : écran *Batterie* (jauge, consommation moyenne estimée, décharge
  précédente).

## Polices et émojis

`main/fonts/` est généré par `tools/gen_fonts.py` (il faut
[lv_font_conv](https://github.com/lvgl/lv_font_conv), fontTools et une copie de LVGL 9.2.2
pour les sources Montserrat, DejaVu Sans et Font Awesome) :

```bash
LVGL_DIR=/chemin/vers/lvgl python3 tools/gen_fonts.py
```

Il écrit aussi `main/fonts/font_cov.c`, la liste des caractères que les polices de texte
savent dessiner, qui sert à nettoyer les textes reçus. Pour ajouter une icône, ajoutez son
code Font Awesome à `FA_BASE` (ou définissez sa séquence UTF-8 dans `main/ui_kit.h`, que le
script lit) puis régénérez.

`assets/emoji.bin` est généré par `tools/gen_emoji.py` à partir des PNG 72×72 de Twemoji
(il faut Pillow) :

```bash
git clone --depth 1 https://github.com/jdecked/twemoji
TWEMOJI_DIR=twemoji/assets/72x72 python3 tools/gen_emoji.py
```
