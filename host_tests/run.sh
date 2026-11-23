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
rm -f test_host test_puca
