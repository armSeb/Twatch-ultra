#!/usr/bin/env bash
# generer-tuiles.sh — cartes hors ligne pour l'app Cartes de la T-Watch Ultra.
#
# Part d'un extrait OpenStreetMap (.osm.pbf de Geofabrik), fabrique les tuiles
# vectorielles avec Planetiler, puis les dessine en PNG 256 × 256 avec un style
# lisible sur la montre, rangées dans un seul fichier <sortie>/tiles.pmtiles à
# copier à la racine de la carte SD. Tout tourne dans Docker. Doc : docs/fr/tiles.md
set -euo pipefail

PLANETILER_IMAGE="ghcr.io/onthegomap/planetiler:0.10.2"
ICI="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage : generer-tuiles.sh [options]

Extrait OpenStreetMap
  -r, --region NOM        extrait Geofabrik à télécharger (défaut : europe/france)
                          exemples : europe/france/bretagne, europe/belgium
  -p, --pbf FICHIER       extrait .osm.pbf déjà sur le disque (au lieu de --region)

Ce qu'on dessine
  -z, --zooms A-B         niveaux pour tout l'extrait (défaut : 0-12) ; « aucun » pour
                          ne dessiner que les zones ci-dessous
  -d, --detail LAT,LON,RAYON_KM[,A-B]
                          zone détaillée autour d'un point (défaut des niveaux : 13-17)
                          ex. : --detail 48.11,-1.68,25  (Rennes, 25 km autour)
  -b, --bbox OUEST,SUD,EST,NORD[,A-B]
                          zone détaillée en rectangle (degrés), niveaux 13-17 par défaut
      (--detail et --bbox peuvent être répétés)

Carte SD et sortie
  -s, --sd DOSSIER        carte SD montée : lit sa place libre, puis y copie
                          tiles.pmtiles à la fin
      --taille T          taille de la carte SD sans --sd (défaut : 2G)
  -o, --sortie DOSSIER    où écrire tiles.pmtiles (défaut : ./tuiles-montre)
  -n, --estimer           compter les tuiles et la place, sans rien dessiner
      --forcer            dessiner même si ça ne rentre pas
      --refaire           tout redessiner (sinon on reprend où on s'était arrêté)

Réglages
  -j, --jobs N            dessins en parallèle (défaut : nombre de cœurs − 1)
      --memoire M         mémoire Java de Planetiler (défaut : moitié de la RAM, ex. 8g)
      --style FICHIER     autre style MapLibre (défaut : style-montre.json)
      --couleurs N        couleurs de la palette PNG, 2 à 256 (défaut : 128)
  -w, --travail DOSSIER   cache : extraits, .mbtiles, données Planetiler
                          (défaut : ~/.cache/twatch-tuiles)
  -h, --help              cette aide

Exemple : la France jusqu'au niveau 12, et Rennes en détail sur 25 km :
  tools/tuiles/generer-tuiles.sh --detail 48.11,-1.68,25 --sd /media/$USER/MONTRE
EOF
}

die() { echo "Erreur : $*" >&2; exit 2; }

REGION="europe/france"
PBF=""
SORTIE="./tuiles-montre"
SD=""
ESTIMER=0
MEMOIRE=""
STYLE=""
TRAVAIL="${XDG_CACHE_HOME:-$HOME/.cache}/twatch-tuiles"
ARGS=()      # options passées telles quelles au dessin

while [ $# -gt 0 ]; do
    case "$1" in
    -h|--help) usage; exit 0 ;;
    -r|--region) REGION="${2:?}"; PBF=""; shift ;;
    -p|--pbf) PBF="${2:?}"; shift ;;
    -o|--sortie) SORTIE="${2:?}"; shift ;;
    -s|--sd) SD="${2:?}"; shift ;;
    -w|--travail) TRAVAIL="${2:?}"; shift ;;
    -n|--estimer) ESTIMER=1 ;;
    --memoire) MEMOIRE="${2:?}"; shift ;;
    --style) STYLE="${2:?}"; shift ;;
    -z|--zooms) ARGS+=(--zooms "${2:?}"); shift ;;
    -d|--detail) ARGS+=(--detail "${2:?}"); shift ;;
    -b|--bbox) ARGS+=(--bbox "${2:?}"); shift ;;
    --taille) ARGS+=(--taille "${2:?}"); shift ;;
    -j|--jobs) ARGS+=(--jobs "${2:?}"); shift ;;
    --couleurs) ARGS+=(--couleurs "${2:?}"); shift ;;
    --forcer) ARGS+=(--forcer) ;;
    --refaire) ARGS+=(--refaire) ;;
    *) usage >&2; echo >&2; die "option inconnue : $1" ;;
    esac
    shift
done

# ------------------------------------------------------------- vérifications
command -v docker >/dev/null || die "Docker n'est pas installé (paquet docker.io ou docker-ce)."
docker info >/dev/null 2>&1 || die "Docker ne répond pas. Démarrer le service (sudo systemctl start docker) \
et s'ajouter au groupe docker (sudo usermod -aG docker \$USER, puis se reconnecter)."
command -v curl >/dev/null || die "curl manquant."

mkdir -p "$TRAVAIL" "$SORTIE"
TRAVAIL="$(cd "$TRAVAIL" && pwd)"
SORTIE="$(cd "$SORTIE" && pwd)"
USERFLAG=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
TTY=(); [ -t 1 ] && TTY=(-t)

# ------------------------------------------------------------ carte SD montée
if [ -n "$SD" ]; then
    [ -d "$SD" ] || die "$SD n'existe pas (la carte SD est-elle montée ?)"
    SD="$(cd "$SD" && pwd)"
    [ -w "$SD" ] || die "$SD n'est pas accessible en écriture"
    FS="$(stat -f -c %T "$SD")"
    LIBRE=$(( $(stat -f -c %a "$SD") * $(stat -f -c %S "$SD") ))
    if [ -f "$SD/tiles.pmtiles" ]; then         # l'ancien fichier sera remplacé
        LIBRE=$(( LIBRE + $(stat -c %s "$SD/tiles.pmtiles") ))
    fi
    echo "Carte SD : $SD ($FS, $(( LIBRE / 1048576 )) Mo utilisables)"
    case "$FS" in msdos|vfat|fat*|exfat) ;; *) echo "Attention : la montre lit des cartes FAT, pas $FS." ;; esac
    ARGS+=(--libre "$LIBRE")
fi

# ------------------------------------------------------------------ extrait
if [ -n "$PBF" ]; then
    [ -f "$PBF" ] || die "$PBF introuvable"
    PBF="$(cd "$(dirname "$PBF")" && pwd)/$(basename "$PBF")"
    NOM="$(basename "$PBF" .osm.pbf)"
else
    [[ "$REGION" =~ ^[a-z0-9/-]+$ ]] || die "nom de région Geofabrik invalide : $REGION"
    NOM="${REGION//\//_}"
    PBF="$TRAVAIL/$NOM-latest.osm.pbf"
    URL="https://download.geofabrik.de/$REGION-latest.osm.pbf"
    echo "Extrait Geofabrik : $URL"
    TMP="$PBF.part"
    rm -f "$TMP"
    if [ -f "$PBF" ]; then
        curl -fL --progress-bar -z "$PBF" -o "$TMP" "$URL" || die "téléchargement impossible (nom de région ? voir https://download.geofabrik.de)"
    else
        curl -fL --progress-bar -o "$TMP" "$URL" || die "téléchargement impossible (nom de région ? voir https://download.geofabrik.de)"
    fi
    if [ -s "$TMP" ]; then
        if MD5="$(curl -fsL "$URL.md5" | cut -d' ' -f1)" && [ -n "$MD5" ]; then
            echo "$MD5  $TMP" | md5sum -c --quiet - || die "extrait abîmé (somme MD5 fausse), relancer"
        fi
        mv -f "$TMP" "$PBF"
    else
        rm -f "$TMP"
        echo "Extrait déjà à jour."
    fi
fi
PBF_DIR="$(dirname "$PBF")"; PBF_BASE="$(basename "$PBF")"

# ------------------------------------------------------- image de dessin
echo "Préparation de l'image de dessin…"
docker build -q -t twatch-tuiles "$ICI" >/dev/null

STYLE_MOUNT=()
if [ -n "$STYLE" ]; then
    [ -f "$STYLE" ] || die "$STYLE introuvable"
    STYLE_MOUNT=(-v "$(cd "$(dirname "$STYLE")" && pwd)/$(basename "$STYLE"):/opt/style.json:ro")
    ARGS+=(--style /opt/style.json)
fi

dessin() {     # dessin <commande> [options…]
    docker run --rm --init "${TTY[@]}" "${USERFLAG[@]}" \
        -v "$PBF_DIR/$PBF_BASE:/extrait/$PBF_BASE:ro" \
        -v "$TRAVAIL:/travail" -v "$SORTIE:/sortie" "${STYLE_MOUNT[@]}" \
        twatch-tuiles "$@" --pbf "/extrait/$PBF_BASE" "${ARGS[@]}"
}

# ------------------------------------------------------------- estimation
set +e
dessin estimer
RC=$?
set -e
[ $RC = 0 ] || exit $RC
[ "$ESTIMER" = 1 ] && exit 0

# --------------------------------------------- tuiles vectorielles (Planetiler)
MBTILES="$TRAVAIL/$NOM.mbtiles"
if [ ! -f "$MBTILES" ] || [ "$PBF" -nt "$MBTILES" ]; then
    if [ -z "$MEMOIRE" ]; then
        RAM_MO=$(awk '/MemTotal/ {print int($2 / 1024)}' /proc/meminfo)
        MEMOIRE="$(( RAM_MO / 2 ))m"
    fi
    echo
    echo "Tuiles vectorielles avec Planetiler (mémoire Java : $MEMOIRE)."
    echo "La première fois, il télécharge aussi le trait de côte et Natural Earth (≈ 1 Go, gardés en cache)."
    rm -f "$TRAVAIL/en-cours.mbtiles"
    docker run --rm --init "${TTY[@]}" "${USERFLAG[@]}" -e JAVA_TOOL_OPTIONS="-Xmx$MEMOIRE" \
        -v "$TRAVAIL:/data" -v "$PBF_DIR/$PBF_BASE:/extrait/$PBF_BASE:ro" \
        "$PLANETILER_IMAGE" \
        --osm-path="/extrait/$PBF_BASE" --output=/data/en-cours.mbtiles \
        --download --force --storage=mmap --exclude-layers=poi,housenumber,aerodrome_label
    mv -f "$TRAVAIL/en-cours.mbtiles" "$MBTILES"
else
    echo "Tuiles vectorielles déjà faites : $MBTILES"
fi

# ------------------------------------------------------------------- dessin
echo
dessin dessiner --mbtiles "/travail/$NOM.mbtiles"
echo "Fichier prêt : $SORTIE/tiles.pmtiles"

# ------------------------------------------------------- copie sur la carte
if [ -n "$SD" ]; then
    echo
    echo "Copie sur la carte SD ($SD/tiles.pmtiles)…"
    rm -f "$SD/tiles.pmtiles"          # place libérée avant la copie
    if command -v rsync >/dev/null; then
        rsync --info=progress2 "$SORTIE/tiles.pmtiles" "$SD/tiles.pmtiles.part"
    else
        cp "$SORTIE/tiles.pmtiles" "$SD/tiles.pmtiles.part"
    fi
    mv "$SD/tiles.pmtiles.part" "$SD/tiles.pmtiles"
    echo "Écriture sur la carte…"
    sync
    [ -d "$SD/tiles" ] && echo "Un ancien dossier tiles/ est sur la carte : la montre préfère tiles.pmtiles, il peut être effacé."
    echo "Fini. Éjecter la carte (umount $SD) avant de la retirer."
else
    echo "Copier tiles.pmtiles à la racine de la carte SD (Réglages › Carte SD › partager avec le PC, ou un lecteur)."
fi
