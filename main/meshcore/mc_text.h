/*
 * mc_text.h — petites fonctions sans matériel pour l'historique MeshCore
 * (testées sur PC : host_tests/test_mc_text.c).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* copie src (len octets au plus, SIZE_MAX : jusqu'au NUL) dans dst sans couper
 * un caractère UTF-8 ; dst toujours terminé */
void mc_copy_utf8(char *dst, size_t cap, const char *src, size_t len);
uint32_t mc_fnv32(const void *p, size_t n);          /* jamais 0 */
/* identifiant d'un canal : empreinte de sa clé (6 octets) */
void mc_channel_peer(const uint8_t *secret, size_t len, uint8_t out[6]);
/* clé de tri : émojis et ponctuation de tête ignorés, minuscules, sans accents */
void mc_sort_key(const char *name, char *key, size_t cap);
/* trame de la file de l'app (message reçu) : type de conversation et
 * horodatage de l'expéditeur ; false si ce n'est pas un message texte */
bool mc_frame_info(const uint8_t *f, int len, uint8_t *kind, uint32_t *ts);
/* message de canal « nom: texte » : copie le nom dans sender (vide sinon) et
 * renvoie le début du texte */
const char *mc_split_sender(const char *text, char *sender, size_t cap);

#ifdef __cplusplus
}
#endif
