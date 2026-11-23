# Notes matérielles — T-Watch Ultra

Français · [English](../en/hardware.md)

Références : [LilyGoLib](https://github.com/Xinyuan-LilyGO/LilyGoLib) (définition de la
carte T-Watch Ultra), [SensorLib](https://github.com/lewisxhe/SensorLib) et
[XPowersLib](https://github.com/lewisxhe/XPowersLib). Toutes les broches sont dans
`main/board.h`.

ESP32-S3 avec flash externe de 16 Mo et PSRAM QSPI de 8 Mo, et un quartz externe de
32,768 kHz qui sert d'horloge basse consommation au Bluetooth.

## Broches

| Fonction | Broches |
|---|---|
| AMOLED CO5300 410×502 (QSPI, SPI2) | CS 41, SCLK 40, D0–D3 38/39/42/45, RST 37, TE 6 |
| I2C (toutes les puces) | SDA 3, SCL 2 — tactile CST9217 `0x1A`, expandeur XL9555 `0x20`, BHI260AP `0x28`, AXP2101 `0x34`, RTC PCF85063 `0x51`, haptique DRV2605 `0x5A` |
| Interruptions | RTC 1, NFC 5, PMU 7, BHI260AP 8 (active haute), tactile 12, LoRa DIO1 14 (active haute) ; les autres sont actives basses |
| SPI partagé (SPI3) | MOSI 34, MISO 33, SCK 35 — carte SD CS 21, NFC (ST25R3916) CS 4, SX1262 CS 36 / RST 47 / BUSY 48 |
| Ampli I2S MAX98357A | BCLK 9, WCLK 10, DOUT 11 |
| Micro PDM | CLK 17, DATA 18 |
| GNSS u-blox MIA-M10Q | TX 43 (ESP → GNSS), RX 44, PPS 13 |
| Bouton BOOT | 0 (actif bas) ; le bouton latéral est la touche marche de l'AXP2101 |

**Expandeur XL9555** : alimentation du DRV2605 (6), alimentation de l'écran (7), reset du
tactile (8), détection de la carte SD (10), commutateur d'antenne LoRa (11).

## Rails de l'AXP2101

| Rail | Alimente | Firmware |
|---|---|---|
| DCDC1 3,3 V | ESP32-S3 | toujours allumé, jamais touché |
| ALDO1 3,3 V | carte SD | coupé tant qu'inutilisé ; allumé pendant que la radio LoRa tourne (bus SPI partagé) |
| ALDO2 3,3 V | écran | allumé |
| ALDO3 3,3 V | LoRa SX1262 | allumé seulement quand MeshCore tourne |
| ALDO4 1,8 V | BHI260AP | allumé seulement si lever le poignet ou le compteur de pas est activé |
| BLDO1 3,3 V | GNSS | coupé tant qu'inutilisé |
| BLDO2 3,3 V | ampli MAX98357A | allumé seulement pendant un son |
| DLDO1 3,3 V | NFC | coupé tant qu'inutilisé ; allumé pendant que la radio LoRa tourne (bus SPI partagé) |
| VRTC | sauvegarde du GNSS | toujours allumé |
| DCDC2–5 | inutilisés | coupés |

**L'AXP2101 garde ses registres tant que la batterie est branchée** : un firmware précédent
qui a laissé un rail allumé le laisse allumé pour le suivant, donc chaque rail et chaque
tension sont réglés explicitement au démarrage. Les broches d'une puce non alimentée restent
en haute impédance (ni entrée ni sortie) : une E/S tenue à 3,3 V vers une puce sans
alimentation la nourrit par ses diodes de protection.

L'AXP2101 ne mesure pas le courant de la batterie : la consommation moyenne affichée sur
l'écran *Batterie* est estimée d'après la baisse du pourcentage de sa jauge (1 % ≈ 11 mAh
sur l'accu de 1100 mAh), il lui faut donc quelques heures. Le courant de charge est de
400 mA (sous 0,5 C ; l'AXP2101 chauffe au-delà de 500 mA).

## Écran

- Le CO5300 se pilote comme un SH8601 : un registre s'écrit avec l'instruction `0x02` sur une
  ligne puis l'adresse `0x00 RR 00` ; les pixels partent avec l'instruction `0x32`, adresse
  `0x002C00`, données sur 4 lignes. `esp_lcd` fait exactement cela en `quad_mode` avec une
  commande de 32 bits.
- La dalle est au milieu de la mémoire du CO5300 : colonnes 22 à 431 (`BOARD_LCD_GAP_X`).
- La fenêtre d'écriture doit commencer et finir sur des colonnes et lignes paires : les zones
  invalidées par LVGL sont arrondies (`LV_EVENT_INVALIDATE_AREA`).
- Chaque image commence sur le signal TE : la dalle n'affiche jamais une image à moitié
  dessinée. À 80 MHz sur 4 lignes, une image pleine (≈400 Ko) part en 10 ms environ.
- Pas de rétroéclairage : la luminosité est un registre du CO5300. Un pixel noir est éteint.
- **Coins arrondis** : le rayon des coins de la dalle est d'environ 84 px
  (`BOARD_LCD_CORNER_R`, à confirmer sur la montre). Les écrans gardent leur contenu dans une
  zone sûre à 25 px des côtés et 30 px du haut et du bas (`BOARD_SAFE_INSET_X/Y`) ; le cadran
  *Pub* et *Envol* occupent toute la dalle.

## Tactile (CST9217)

Registres sur 16 bits : lecture de 12 octets à `0xD000`, puis acquittement par `0xAB`.
L'octet 6 doit valoir `0xAB`, le quartet bas de l'octet 0 vaut `0x06` pour « appuyé »,
l'octet 5 donne le nombre de points. Reset par le XL9555.

## Mouvement (BHI260AP)

Le BHI260AP est un concentrateur de capteurs programmable : le firmware Bosch
(`components/bhy2/`) est chargé dans sa RAM au démarrage (environ 1 s), puis le compteur de
pas et le geste du poignet tournent dedans. Son compteur de pas ne se remet pas à zéro sans
redémarrer ce firmware : la montre garde sa valeur au dernier zéro logiciel et la soustrait.

La puce est tournée de 180° autour de Z (`TOP_LAYER_BOTTOM_RIGHT_CORNER` de LilyGoLib) : la
matrice d'orientation donnée à l'accéléromètre et au gyroscope est `{ -1, 0, 0,  0, -1, 0,  0, 0, 1 }`.
Le geste du poignet de Bosch s'appuie sur ce repère (Z positif écran vers le haut) ; la 0.2.6
avait essayé une rotation de 180° autour de Y, qui l'a cassé. Le sens du tangage du jeu de vol
se règle dans l'app.

## Pièges

- **Confidentialité gérée par l'hôte NimBLE** : la reconnexion échoue quand l'iPhone change
  d'adresse privée (~15 min, timeout SMP). La résolution d'adresse est laissée au
  contrôleur.
- **`BT_NIMBLE_GATT_CLIENT` dépend de `BT_NIMBLE_ROLE_CENTRAL`** dans ESP-IDF 5.5 : désactiver
  le rôle central retire sans prévenir `ble_gattc_*`, donc ANCS.
- **iOS ignore parfois la Security Request** : la montre l'envoie 500 ms après la connexion et
  coupe la liaison si le chiffrement n'a pas eu lieu 8 s plus tard, puis annonce à nouveau.
- **Collisions de paramètres de connexion** (0x22A) quand la montre demande des paramètres
  économes pendant qu'iOS les change : nouvel essai avec un délai croissant jusqu'à
  acceptation.
- **Annuler un envoi OTA** : `httpd_stop()` attend le gestionnaire d'envoi, qui continue de
  recevoir tant que le téléphone envoie, et la montre se fige. Elle redémarre directement à
  la place.
- **Le XL9555 garde ses sorties d'un reset de l'ESP32 à l'autre** : la dalle peut rester
  noire après un reset. La broche d'alimentation de l'écran (7) est coupée puis rétablie au
  démarrage.
- **Commutateur d'antenne LoRa (broche 11 du XL9555)** : niveau bas = antenne intégrée, haut
  = antenne externe sur le connecteur USB-C, l'inverse des commentaires de LilyGoLib
  (vérifié sur la montre).
- **SX1262, carte SD et NFC partagent SPI3** : une carte SD ou une puce NFC non alimentée
  tire le bus vers le bas par ses diodes de protection et le SX1262 ne répond jamais (statut
  0x00). Tant que la radio tourne, les deux rails restent allumés, sélections de puce au
  niveau haut.
- **Réglages du SX1262** (comme la variante t-watch-ultra de Meshtastic) : TCXO 1,8 V sur
  DIO3, DIO2 en commutateur RF, régulateur DC-DC, gain de réception renforcé.
- **L'image complète à 0x0 efface la NVS** (appairages). Pour une mise à jour USB, écrivez
  l'application à `0x20000` avec `ota_data_initial.bin`.

## À vérifier sur la montre

Rayon des coins, orientation de la dalle, correspondance des coordonnées tactiles, délais du
reset de la dalle, consommation réelle en veille.
