# Cartes hors ligne — fabriquer les tuiles

**Français** · [English](../en/tiles.md)

L'app **Cartes** lit les cartes rangées dans un seul fichier **`tiles.pmtiles`** à la racine
de la carte micro-SD (format [PMTiles](https://github.com/protomaps/PMTiles), tuiles PNG
256 × 256). Un seul gros fichier se copie vite, là où des dizaines de milliers de petites
tuiles prennent des heures sur une carte SD.
Le script `tools/tuiles/generer-tuiles.sh` le fabrique sur le PC à partir d'un extrait
OpenStreetMap de [Geofabrik](https://download.geofabrik.de) (la France, une région, un pays),
avec un style fait pour l'écran de la montre : gros libellés, routes larges et contrastées,
peu de détails inutiles. Une seule commande, rien à installer à part Docker.

![Aperçu sur l'écran de la montre, niveaux 15, 16 et 17](../img/tiles.png)

*Aperçu sur un petit extrait de test (Helsinki), aux dimensions de l'écran.*

## Ce qu'il faut

- **Linux** avec **Docker** (`sudo apt install docker.io`, puis
  `sudo usermod -aG docker $USER` et se reconnecter).
- Pour la **France entière** : environ **12 Go** de disque libre (extrait 4,5 Go, trait de côte
  et Natural Earth 1,3 Go, tuiles vectorielles 3 à 4 Go) et **8 Go de RAM** au moins.
  Pour une région (Bretagne, Normandie…), dix fois moins.
- Une connexion Internet pour le premier lancement (extrait, images Docker, données de côte).

## Utilisation

Depuis le dossier du dépôt :

```sh
# La France jusqu'au niveau 12, et 25 km autour de Rennes en détail (niveaux 13 à 16)
tools/tuiles/generer-tuiles.sh --detail 48.11,-1.68,25

# Pareil, et copie directe sur la carte SD montée
tools/tuiles/generer-tuiles.sh --detail 48.11,-1.68,25 --sd /media/$USER/MONTRE

# Juste compter les tuiles et la place, sans rien dessiner
tools/tuiles/generer-tuiles.sh --detail 48.11,-1.68,25 --estimer

# Une seule région, détaillée partout jusqu'au niveau 14
tools/tuiles/generer-tuiles.sh --region europe/france/bretagne --zooms 0-14

# Deux zones détaillées (domicile et vacances), la seconde jusqu'au niveau 17
tools/tuiles/generer-tuiles.sh --detail 48.11,-1.68,25 --detail 43.30,5.37,10,13-17
```

Ce qui se passe :

1. **Téléchargement** de l'extrait Geofabrik (seulement s'il y a plus récent), vérifié par sa
   somme MD5. Rangé dans `~/.cache/twatch-tuiles`.
2. **Estimation** : nombre de tuiles par niveau et place sur la carte SD. Si ça ne rentre pas,
   le script s'arrête là et dit quoi réduire (`--forcer` pour passer outre).
3. **Tuiles vectorielles** avec [Planetiler](https://github.com/onthegomap/planetiler) (schéma
   OpenMapTiles), une seule fois par extrait : de 30 min (8 cœurs) à plus d'une heure (4 cœurs) pour la France.
   Le premier lancement télécharge aussi le trait de côte et Natural Earth (≈ 1,3 Go, gardés).
4. **Dessin** des tuiles PNG 256 × 256 dans `./tuiles-montre/tiles` (MapLibre Native, palette
   de 128 couleurs). La progression et le temps restant s'affichent. Compter de l'ordre d'une
   heure pour la France jusqu'au niveau 12 avec une zone détaillée sur un PC récent, plusieurs
   heures sur un petit processeur (Atom, Celeron).
5. **Rangement** de toutes les tuiles dans `./tuiles-montre/tiles.pmtiles` (quelques secondes).
   Les tuiles identiques (mer, zones vides) n'y sont stockées qu'une fois.
6. Avec `--sd` : **copie** de `tiles.pmtiles` à la racine de la carte, puis `sync`. Éjecter la
   carte avant de la retirer. Sans `--sd`, copier ce fichier soi-même, par exemple d'une autre
   machine : `rsync --info=progress2 serveur:tuiles-montre/tiles.pmtiles /media/$USER/MONTRE/`.

Lancé à distance par SSH, utiliser `tmux` ou `screen` : une session coupée arrête le script.

Interrompu (Ctrl+C, coupure), il suffit de relancer **la même commande** : le dessin reprend
où il s'était arrêté. `--refaire` force à tout redessiner. Pour mettre la carte à jour plus
tard, relancer : un extrait plus récent est retéléchargé et tout est refait.

## Options

| Option | Rôle |
|---|---|
| `-r, --region NOM` | extrait Geofabrik, chemin de son URL : `europe/france` (défaut), `europe/france/bretagne`, `europe/belgium`… |
| `-p, --pbf FICHIER` | extrait `.osm.pbf` déjà téléchargé |
| `-z, --zooms A-B` | niveaux pour tout l'extrait (défaut `0-12`) ; `aucun` pour ne faire que les zones |
| `-d, --detail LAT,LON,RAYON_KM[,A-B]` | zone détaillée autour d'un point (niveaux `13-16` par défaut), répétable |
| `-b, --bbox OUEST,SUD,EST,NORD[,A-B]` | zone détaillée en rectangle, répétable |
| `-s, --sd DOSSIER` | carte SD montée : sa place libre sert à l'estimation, `tiles.pmtiles` y est copié à la fin |
| `--taille T` | sans `--sd` : taille de la carte (défaut `2G`) |
| `-o, --sortie DOSSIER` | où écrire `tiles.pmtiles` et les tuiles de travail (défaut `./tuiles-montre`) |
| `-n, --estimer` | estimation seulement |
| `--forcer`, `--refaire` | dessiner même si c'est trop gros ; tout redessiner |
| `-j, --jobs N` | dessins en parallèle (défaut : cœurs − 1) |
| `--memoire M` | mémoire Java de Planetiler (défaut : la moitié de la RAM) |
| `--style FICHIER` | autre style MapLibre |
| `--couleurs N` | couleurs de la palette PNG (défaut 128 ; moins = plus petit) |
| `-w, --travail DOSSIER` | cache (défaut `~/.cache/twatch-tuiles`) |

Les coordonnées s'obtiennent par un clic droit sur [openstreetmap.org](https://www.openstreetmap.org)
(« Afficher l'adresse ») ou dans l'app GPS de la montre.

## Que choisir pour une carte de 2 Go

Chaque niveau compte **quatre fois plus** de tuiles que le précédent :

| France entière | Tuiles | Taille au plus |
|---|---|---|
| niveaux 0 à 11 | 9 400 | ≈ 85 Mo |
| niveaux 0 à 12 | 37 000 | ≈ 320 Mo |
| niveau 13 en plus | + 110 000 | + 950 Mo |

| Zone détaillée, niveaux 13 à 16 | Tuiles | Taille au plus |
|---|---|---|
| rayon 10 km | 3 400 | ≈ 25 Mo |
| rayon 25 km | 20 500 | ≈ 140 Mo |
| rayon 50 km | 80 000 | ≈ 540 Mo |

Ce sont des maximums : la mer et les pays voisins (vides, hors de l'extrait) ne prennent
presque rien. Le niveau 12 montre les villages et les routes principales, le 14 toutes les
rues, le 16 les bâtiments et les sentiers. La montre zoome deux niveaux plus loin que le plus
détaillé présent, en agrandissant.

**Conseil** : la France de 0 à 12 et une zone de 25 km autour de chez soi, de 13 à 16, soit
moins de 500 Mo. Avec un seul fichier, la taille des blocs de la carte (FAT16 ou FAT32) ne
compte plus. La montre lit un fichier de **2 Go au plus** : le script le vérifie avant de dessiner.

## Le style

`tools/tuiles/style-montre.json` est un style MapLibre (schéma OpenMapTiles) : libellés de 16
à 30 pixels avec un contour blanc, routes colorées par importance et bordées, eau bleue
franche, bois et parcs verts, bâtiments à partir du niveau 15, sentiers en tirets rouges à
partir du niveau 15, chemins en tirets bruns. Pas de points d'intérêt (illisibles sur la
montre).

Il est généré par `tools/tuiles/style_montre.py` (couleurs, tailles et niveaux en tête de
fichier) : modifier le script, relancer `python3 tools/tuiles/style_montre.py
tools/tuiles/style-montre.json`, puis relancer le script de tuiles (le dessin est refait, les
tuiles vectorielles non). `--style` permet aussi d'essayer un autre style sans toucher au dépôt.

## Détails techniques

- `generer-tuiles.sh` : téléchargement, contrôle de la carte SD (`stat -f` : place libre),
  Planetiler dans son image officielle
  (`ghcr.io/onthegomap/planetiler`, couches `poi` et `housenumber` exclues), puis le dessin.
- `render.cjs` tourne dans une image bâtie sur `maptiler/tileserver-gl` (MapLibre Native,
  sharp et la police Noto Sans y sont déjà) :
  - lit l'emprise de l'extrait dans l'en-tête du `.osm.pbf` pour estimer avant Planetiler ;
  - dessine des blocs de 8 × 8 tuiles d'un coup avec 128 pixels de marge, puis découpe :
    les libellés ne sont pas coupés entre deux tuiles voisines du même bloc ;
  - une tuile unie (mer, hors de l'extrait) dont la parente est unie de la même couleur n'est
    pas écrite, tant que la montre peut agrandir la parente (4 niveaux au plus,
    `MT_UPSCALE` dans `main/maptiles.h`) ;
  - écrit des PNG palette (lus par `main/png_tile.c`) et un journal `.journal-tuiles` pour la
    reprise ;
  - range ensuite les tuiles dans `tiles.pmtiles` (PMTiles v3 : répertoires gzip, tuiles PNG
    non recompressées, rangées par numéro de Hilbert, contenus identiques stockés une fois ;
    vérifié avec la bibliothèque Python officielle `pmtiles`). La commande interne `paquet`
    refait seulement ce fichier à partir du dossier `tiles`.
- Tout tourne avec l'utilisateur courant : les fichiers créés lui appartiennent.

## Licences

Les cartes contiennent des données © les contributeurs d'OpenStreetMap, sous licence
[ODbL](https://www.openstreetmap.org/copyright), mises en forme selon le schéma
© OpenMapTiles (CC-BY 4.0). Police Noto Sans : SIL Open Font License. Planetiler : Apache 2.0,
TileServer GL et MapLibre Native : BSD. Ces logiciels ne sont pas copiés dans le dépôt : Docker
les télécharge.
