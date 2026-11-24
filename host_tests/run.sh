#!/bin/sh
# Tests PC (gcc) de la logique sans matériel. Le premier échec arrête tout
# (code de sortie non nul : la CI le voit).
set -e
cd "$(dirname "$0")"
gcc -std=gnu17 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined -g \
    -Istubs -I../main -o test_host test_host.c ../main/ancs_parse.c ../main/notif_store.c ../main/fonts/font_cov.c && ./test_host
python3 check_i18n.py
# Púca : vie de l'animal (évolution, sommeil, négligence, pas → trèfles)
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -I../main -o test_puca test_puca.c ../main/puca_model.c -lm && ./test_puca
# MeshCore : ed25519 (vecteurs externes, signatures altérées ou aléatoires),
# base64 des clés de canal. Décalages de valeurs négatives : voulus dans
# l'ed25519 de référence (ref10), arithmétiques avec gcc.
E=../components/meshcore/upstream/lib/ed25519
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize=shift -g -I$E -I../components/meshcore/shim \
    -o test_meshcore test_meshcore.c $E/fe.c $E/ge.c $E/sc.c $E/sha512.c $E/sign.c $E/verify.c $E/keypair.c && ./test_meshcore
# historique MeshCore : coupe UTF-8, tri des conversations, trames de l'app
gcc -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -g -I../main/meshcore -Istubs -o test_mc_text test_mc_text.c \
    ../main/meshcore/mc_text.c && ./test_mc_text
rm -f test_host test_puca test_meshcore test_mc_text
