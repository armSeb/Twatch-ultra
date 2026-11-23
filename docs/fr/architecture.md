# Architecture et énergie

Français · [English](../en/architecture.md)

## Tâches et propriétaires

```
            ┌──────────── tâche hôte NimBLE (cœur 0) ─────────────┐
            │ ble.c   GAP, sécurité passkey, service batterie,      │
 iPhone ◄──►│         paramètres de connexion, publicité, journal   │
            │ ancs.c  découverte ANCS + CTS + AMS, file de commandes│
            │         réassemblage Data Source (ancs_parse.c)       │
            └──────────┬───────────────────────────┬────────────────┘
                       │ g_notif_queue (copies)    │ app_post(événement)
            ┌──────────▼───────────────────────────▼────────────────┐
 IRQ PMU ──►│ tâche app (cœur 1) : seule propriétaire de LVGL       │
 INT tactile│ main.c   boucle d'événements / lv_timer_handler / veille│
 esp_timers │ ui.c, face_*.c, app_*.c   notif_store.c               │
            │ display.c (CO5300 + CST9217)  power.c, motion.c       │
            └───────────────────────────────────────────────────────┘
```

Règles de stabilité :

- **Un seul propriétaire par état.** LVGL et le stockage des notifications appartiennent à
  la tâche app ; l'état ANCS à la tâche hôte NimBLE. Elles échangent par files et
  événements, jamais par un mutex LVGL partagé.
- **Une seule procédure ATT en vol** côté ANCS, avec un numéro de génération vérifié dans
  chaque callback (les réponses tardives après une reconnexion sont ignorées).
- **Data Source réassemblé octet par octet**, testé sur PC avec des fragmentations
  aléatoires.
- **Rejeu iOS** filtré par `EventFlagPreExisting` et déduplication par contenu (FNV-1a, durée
  de vie 5 minutes) ; seules les 32 notifications les plus récentes sont gardées.
- **Texte iOS nettoyé** (UTF-8 tronqué, formes trop longues, surrogates, caractères qu'aucune
  police ne dessine) avant d'arriver à LVGL ; les émojis sont gardés (voir *Texte et
  émojis* plus bas).
- **Interruption PMU en niveau** : aucune interruption perdue pendant un light sleep.
- **Événements de partout, interface à un seul endroit** : timers, BLE et interruptions ne
  font que poster des événements ; `app_post_event()` permet à une app de réagir écran
  fermé.

Pas d'`esp_lvgl_port` (son timer de tick et sa tâche réveillent le CPU en permanence), pas de
bibliothèque pour le PMU, le tactile, la RTC ou l'haptique (quelques registres suffisent ;
l'API Bosch BHI2xy est le seul pilote de capteur), `esp_lcd` natif, `i2c_master` d'ESP-IDF 5
sur un seul bus I2C.

## Interface

- **Échelle et zone sûre** : l'interface a été dessinée pour 240×240 et est mise en page à
  ×1,5 (`S()` et `UI_K` dans `ui_geom.h`) dans une zone sûre de 360×442 qui garde le contenu
  hors des coins arrondis de la dalle (`UI_PAD_X/Y`, voir [hardware.md](hardware.md)). Les
  polices ont été régénérées à ×1,5 (17, 24, 30, 66 et 108 px) ; les seuils de gestes de
  LVGL sont mis à l'échelle aussi. Le cadran *Pub* et *Envol* dessinent sur toute la dalle.

- **Navigation par origine** : chaque écran retient d'où il a été ouvert ; glisser → ou
  l'en-tête « ‹ » y ramène toujours.
- **Écrans paresseux** : réglages, lecture en cours, lanceur, contrôles rapides et app
  hébergée sont construits à la demande et détruits à l'extinction de l'écran ; la RAM ne
  contient que ce qui est affiché. Écrans permanents : cadran, liste des notifications,
  détail, appel, énergie, appairage, à propos.
- **Apps** (`app.h`) : voir [apps-and-i18n.md](apps-and-i18n.md). L'app ouverte reste
  construite écran éteint (elle reçoit `on_hide()`, puis `on_show()` au réveil) et la montre
  se réveille dessus.
- **Bannière de notification** : hors du cadran, une nouvelle notification glisse sur la
  couche supérieure au lieu de changer d'écran ; la toucher ouvre le détail, qui ramène
  exactement à l'écran d'avant. Les appels prennent toujours tout l'écran et y ramènent
  ensuite.
- **Analyseur Wi-Fi** (`app_wifi.c`) : le pilote Wi-Fi n'existe que tant que l'app est
  visible écran allumé (`esp_wifi_init` dans `on_show`, `esp_wifi_deinit` dans `on_hide`) ;
  chaque relève allume la radio, fait un balayage actif (~1,2 s) puis la coupe 4 s. Le BLE
  reste connecté (coexistence logicielle).
- **WebRX** (`app_webrx.c`, `webrx.c`, `webrx_cfg.c`) : client OpenWebRX / OpenWebRX+ (guide :
  [webrx.md](webrx.md)). Les réseaux Wi-Fi (jusqu'à 3) et les serveurs (jusqu'à 4, nom et
  adresse ; état d'écoute retenu par serveur) se saisissent sur une page servie par la
  montre (`/webrx`, même point d'accès et même serveur web que la mise à jour ; la session se
  termine par un redémarrage qui rouvre l'app). `webrx.c` fait tourner une tâche sur le cœur 0 :
  Wi-Fi station (réseaux connus par ordre de préférence, modem sleep), `wss://` par
  `esp_transport` (TLS avec le paquet de racines d'ESP-IDF, tampons mbedTLS en PSRAM ;
  `utilisateur:mot_de_passe@` dans l'adresse devient une authentification HTTP Basic), poignée
  de main OpenWebRX, `connectionproperties` (audio à 12 kHz pour tous les modes),
  `selectprofile` et `dspcontrol`. La liste `modes` du serveur (modes ajoutés compris) donne
  les modes écoutables et leur bande passante ; les modes numériques passent par leur mode
  sous-jacent. Les trames audio (IMA-ADPCM avec mots de synchronisation) sont décodées dans un
  tampon de 2,7 s en PSRAM ; une seconde tâche sur le cœur 1 garde ~0,4 s d'avance et
  alimente l'ampli I2S, en sautant ou doublant un échantillon par bloc de 20 ms pour suivre
  l'horloge du serveur ; un filtre de tonalité (passe-haut, bosse de présence, passe-bas
  d'ordre 2 ou 4, 5 paliers) l'adapte au petit haut-parleur. Les trames texte sont construites
  et envoyées en une seule écriture : le serveur OpenWebRX lâche un client dont l'en-tête et la
  charge d'une trame arrivent dans des segments TCP séparés, ce que fait `esp_transport_ws`. Les trames FFT donnent la mini-waterfall, seulement écran allumé. Le
  CPU est plafonné à 160 MHz (80 MHz écran éteint) tant que la radio tourne, et le Bluetooth
  n'est suspendu que si le pilote Wi-Fi manque de RAM interne. La partie station Wi-Fi
  (recherche, réseaux connus, raisons d'échec, repli Bluetooth) est dans `wifi_sta.c`, le filtre
  de tonalité à 5 paliers dans `audio_fx.c` ; les deux servent aussi à Radio.
- **Radio** (`app_radio.c`, `radio.c` ; stations dans `webrx_cfg.c`, guide :
  [radio.md](radio.md)) : lecteur de webradios. Jusqu'à 12 stations (nom, adresse) se saisissent
  sur la même page `/webrx`. La tâche `rad` (cœur 0) connecte le Wi-Fi, ouvre le flux avec
  `esp_http_client` (TLS avec le paquet de racines, redirections suivies à la main,
  `Icy-MetaData: 1`), suit les listes `.m3u`/`.pls`, choisit le codec d'après `Content-Type` ou
  les premiers octets, retire les blocs de métadonnées ICY (`StreamTitle`, Latin-1 converti en
  UTF-8) et remplit un tampon compressé de 128 Ko en PSRAM ; la lecture démarre après ≈ 2 s
  d'audio (d'après `icy-br`). La tâche `radplay` (cœur 1) décode avec les décodeurs Helix en
  virgule fixe (`components/helix_mp3`, `components/helix_aac` : MP3, AAC-LC, HE-AAC/SBR),
  passe en mono, applique tonalité et volume et alimente l'ampli I2S par `sound_stream_*`. Elle
  mesure la part du temps passée à décoder : au-dessus de 60 % elle prend un verrou
  `CPU_FREQ_MAX` (160 MHz), sous 25 % elle le rend, si bien que le plafond de 80 MHz écran éteint
  tient dès que le flux le permet. Toutes les 2 s, écran éteint et RSSI au-dessus de −65 dBm, la
  puissance d'émission Wi-Fi descend à 11 dBm (19,5 dBm sinon). Les échecs sont réessayés après
  5, 10, 20, 30 puis 60 s ; un flux coupé reprend après 2 s. Testé sur PC avec les vrais
  décodeurs et un faux serveur HTTP (redirection, `.pls`, titres ICY).
- **Púca** (vue `app_puca.c`, vie `puca_model.c`, dessins `puca_gfx.c`, sons `puca_sfx.c` ;
  guide : [puca.md](puca.md)) : animal virtuel. Le modèle n'a ni tâche ni minuteur : à chaque
  affichage il avance par pas de 5 minutes depuis la dernière visite (7 jours au plus ; testé
  sur PC), état gardé en un blob NVS de 76 octets. Les pas du jour deviennent des trèfles. La
  vue est un petit moteur de sprites : un tampon de 80×50 indices de palette redessiné à
  12,5 images/s écran allumé (ciel par images clés placées autour des vrais lever et coucher
  du soleil du jour — `puca_sun()`, calculés pour le centre de la France avec le décalage local,
  la nuit tombe donc tôt l'hiver — étoiles, lune ou soleil,
  nuages, collines, crottes, l'animal, particules), puis agrandi ×4 dans un canevas RGB565 de
  320×240 en PSRAM (10 lignes de ciel en plus en haut). Les expressions sont composées : les corps n'ont pas de visage, des glyphes
  d'yeux et de bouche 3×3 communs sont posés aux ancres de chaque corps, d'où les visages
  ouvert / clignement / content / endormi / triste / malade pour toutes les formes. La nuit,
  lumière éteinte, toute la palette s'assombrit sauf les couleurs « lumineuses » (yeux, lune,
  étoiles). Tous les sprites sont des grilles de texte dans `tools/puca_art.py`, qui génère
  `puca_gfx.c/.h` et une planche d'aperçu. Les effets sonores sont de courts airs en onde
  carrée joués par une tâche éphémère sur le flux I2S.
- **Souris** (`app_mouse.c`, `ble_hid.c`, guide : [mouse.md](mouse.md)) : HID over GATT.
  `ble_set_hid_mode()` puis `ble_start()` redémarrent NimBLE avec une autre identité (adresse
  statique dérivée autrement de la MAC, nom, apparence 0x03C2), les services HID (rapports
  souris 1, clavier 2, multimédia 3, références de rapport, mode protocole), DIS (PnP ID) et
  batterie, sans ANCS, en « Just Works » ; événements GAP propres (pas de paramètres éco,
  liaison 11,25–27,5 ms latence 4, sans IRK distribuée). Le pavé lit le tactile toutes les 10 ms, accélère selon la
  vitesse et regroupe les déplacements (~80 rapports/s au plus).
- **Démo** (`app_demo.c`, guide : [demo.md](demo.md)) et **Envol** (`app_flight.c`, guide :
  [flight.md](flight.md)) partagent trois modules. `demo_gfx.c` : couleurs RGB565, police 5×7
  avec accents, triangles, agrandissement vers le canevas. `demo_synth.c` : synthétiseur façon
  tracker dans sa tâche `demosnd` (cœur 0), flux I2S à 16 kHz ouvert du début à la fin (couper
  le son n'envoie que du silence), morceau de 64 mesures pour la démo ou boucle pour le jeu,
  bruitages mixés par-dessus ; il sert d'horloge : chaque image lit sa position dans le
  morceau (et l'enveloppe de grosse caisse) plutôt qu'un compteur d'images. `voxel.c` : monde
  de 512×512 (hauteurs 256 Ko, couleurs éclairées 512 Ko, en PSRAM) généré par une tâche
  `voxgen` de basse priorité sur le cœur 0 — relief, rivières, prés de Voronoï bordés de haies,
  forêts, fermes, éclairage avec ombres portées par lancer de rayon vers le soleil — puis rendu
  colonne par colonne avec tampon de hauteur, interpolation au premier plan, brume, reflets,
  nuages projetés, roulis par colonne et table d'occultation optionnelle (haut du relief à 16
  distances par colonne). Après la génération, hauteur et couleur sont fusionnées dans un mot
  de 32 bits par case (1 Mo) : une seule lecture PSRAM par échantillon, c'est ce qui limite
  le débit. Le rendu est partagé entre les deux cœurs : une tâche `voxrend` (cœur 0, créée au
  premier rendu, arrêtée avec le monde) calcule la moitié gauche pendant que l'appelant
  calcule la droite, synchronisés par deux sémaphores. La démo rend en 120×120 triplé dans un
  canevas LVGL de 360×360 ; le jeu en 205×168, colonnes doublées et lignes triplées pour
  remplir toute la dalle de 410×502, envoyé directement à l'écran (`display_direct()` : deux
  bandes de 26 lignes en RAM DMA, l'une remplie pendant que l'autre part, départ calé sur le
  signal TE de la dalle, sans canevas en PSRAM) ; son interface est dessinée à pleine
  résolution dans la zone sûre. Les deux tiennent 240 MHz tant qu'elles sont affichées et
  lisent l'accéléromètre en direct (`motion_accel_*` : l'accéléromètre corrigé du BHI260AP à
  100 Hz, son rail allumé pour le jeu si besoin). Le record d'Envol est un blob NVS
  (`flight`/`best`).
- **Changer de langue** reconstruit tous les écrans sur place, sans redémarrer.

## Texte et émojis

- **Polices** : Montserrat (latin, latin étendu, vietnamien, cyrillique, ponctuation,
  monnaies) complétée par DejaVu Sans (grec, flèches, maths, formes géométriques, symboles,
  dingbats), plus les icônes Font Awesome. `tools/gen_fonts.py` écrit aussi `font_cov.c`, la
  liste exacte des caractères que les polices de texte savent dessiner ; `notif_store.c`
  garde ceux-là et les plages d'émojis, et retire le reste.
- **Paquet d'émojis** (`assets/emoji.bin`, construit par `tools/gen_emoji.py`, flashé dans la
  partition `emoji`) : 1 949 images Twemoji — formes de base (sans tons de peau : la montre
  affiche la variante jaune), drapeaux, keycaps et séquences ZWJ — en 20 et 28 px, RGB565 +
  alpha, chacune compressée en deflate brut (2,6 Mo). La partition est projetée en mémoire ;
  une image est décompressée à la demande par le `tinfl` de la ROM dans un cache en PSRAM
  (192 par taille).
- **Séquences** (`emoji.c`) : une séquence ne se dessine pas point de code par point de code.
  `emoji_text()` cherche à chaque position la plus longue séquence connue (recherche
  dichotomique dans le paquet trié, en suivant les règles des ZWJ, keycaps, tags et
  indicateurs régionaux), la remplace par un seul point de code privé `U+F0000 + rang`, et
  retire les sélecteurs de variante, les ZWJ orphelins et les tons de peau. `emoji_font_20`
  et `emoji_font_28` sont des polices LVGL qui dessinent ces points de code en images
  (`LV_FONT_GLYPH_FORMAT_IMAGE`) ; ce sont les polices de secours de `font_small`,
  `font_text` et `font_title` : tout label affiche les émojis dès que son texte est passé
  par `emoji_text()` (`emoji_label_set_text()`).
- Les objets LVGL sont alloués sur le tas ; les allocations de plus de 4 Ko vont en PSRAM, la
  RAM interne est gardée pour les piles, les tampons DMA et la radio.

## Énergie

L'ESP32-S3 prend l'horloge basse consommation du Bluetooth sur le quartz externe de
32,768 kHz (`BT_CTRL_LPCLK_SEL_EXT_32K_XTAL`) : le contrôleur dort entre deux événements de
connexion, donc **le light sleep automatique reste possible iPhone connecté**.

| Levier | Réglage |
|---|---|
| Fréquence | CPU à **40 MHz** au repos ; **240 MHz** seulement pendant l'interaction ; apps radio plafonnées à 160 MHz (80 MHz écran éteint) |
| Light sleep | automatique (tickless idle), avec les interruptions PMU, tactile et capteur en niveau comme sources de réveil |
| Liaison BLE | une fois ANCS prêt, des paramètres économes sont demandés (intervalle 150–300 ms, latence 4, timeout 6 s, dans les règles d'Apple) et **redemandés jusqu'à acceptation**, avec un délai croissant |
| Tâches | aucun timer périodique écran éteint ; la tâche app dort jusqu'à un événement (60 s max) ; tick LVGL lu à la demande |
| Écran | CO5300 en veille (SLPIN) ; tactile en balayage lent si le double tap réveille la montre, sinon en veille profonde (réveil par reset) |
| AXP2101 | tout rail inutilisé coupé (SD, LoRa, GNSS, NFC, DCDC2–5) ; rail de l'ampli allumé seulement pendant un son ; rail du BHI260AP coupé si ni lever le poignet ni les pas ne sont activés |
| Périphériques | broches des puces non alimentées laissées en haute impédance (pas d'alimentation fantôme par les diodes de protection) |
| Mode avion | Bluetooth arrêté |

L'AXP2101 donne un pourcentage de batterie (sa propre jauge, chargée avec un modèle de
batterie au premier démarrage) mais pas le courant de la batterie. L'écran *Batterie* estime
donc la consommation moyenne d'après la baisse de ce pourcentage sur la décharge en cours
(1 % ≈ 11 mAh), et garde en NVS la décharge précédente et la part du temps écran allumé.

## Capteur de mouvement (`motion.c`)

Le BHI260AP est un concentrateur de capteurs programmable. Quand *Lever le poignet* ou le
*Compteur de pas* est activé, `motion_init()` (appelé par `power_init()`) allume ALDO4,
charge le firmware Bosch (`components/bhy2/`, BSD-3-Clause) dans sa RAM (environ 1 s) et
active les capteurs virtuels utiles : compteur de pas et geste du poignet. Les deux
désactivés : le rail est coupé et le capteur ne consomme rien.

- **Lever le poignet** : le geste arrive par la ligne d'interruption (GPIO8, active haute,
  aussi source de réveil du light sleep) sous forme d'`EV_MOTION` ; la tâche app lit la FIFO
  et, écran éteint, l'allume. `motion_wrist_confirm()` vérifie ensuite la position de la
  montre selon la sensibilité : moyenne = gravité surtout sur Z (écran vers le haut) sur
  deux mesures à 250 ms d'écart avec peu de mouvement ; basse = plus stricte, et une montre
  posée à plat est refusée ; haute = le geste Bosch seul.
- **Coup d'œil** (`display_set_glance()`) : l'écran s'allume à la luminosité du coup d'œil
  sans le verrou `CPU_FREQ_MAX` et s'éteint après la durée du coup d'œil ; un toucher ou le
  bouton en font un réveil normal.
- **Compteur de pas** : le capteur compte seul. Son compteur ne se remet pas à zéro sans
  redémarrer son firmware : la montre garde sa valeur au dernier zéro logiciel et la
  soustrait. Total du jour = base gardée en NVS + compteur depuis ce zéro. À minuit (heure
  locale), le total passe dans un historique de 7 jours. Il est sauvegardé en NVS toutes les
  15 minutes s'il a changé, au changement de jour, à la désactivation et avant tout
  redémarrage.

## Mise à jour Wi-Fi (OTA)

- La montre coupe le Bluetooth, démarre un point d'accès WPA2 (`T-Watch-OTA`, mot de passe
  tiré une fois et gardé en NVS), un petit serveur DNS qui répond 192.168.4.1 à tout, et un
  serveur HTTP. Les sondes de connectivité Apple et Android reçoivent une réponse « succès »
  pour que le téléphone reste sur le réseau.
- L'envoi est écrit au fil de l'eau par blocs de 4 Ko dans l'emplacement inactif (5 Mo chacun). Le
  premier bloc est vérifié (en-tête d'image ESP32-S3 et descripteur d'application au nom
  `twatch_ultra`), l'image complète est vérifiée (SHA-256) avant activation. La partition des
  émojis ne fait pas partie de la mise à jour.
- Retour arrière : une nouvelle image démarre « à vérifier » et est validée après 30 s de
  fonctionnement ; un plantage avant cela fait revenir à l'emplacement précédent.
- Annuler redémarre la montre (retour propre au Bluetooth). Pendant un envoi, le redémarrage
  est immédiat : arrêter le serveur HTTP attendrait la fin du gestionnaire, qui reçoit
  toujours.
- *Revenir à la x.y.z* vérifie entièrement l'autre emplacement avant d'y démarrer.

## Fichiers sources

| Fichier | Rôle |
|---|---|
| `main/main.c` | `app_main`, tâche app, boucle d'événements, mise en veille de l'écran |
| `main/board.h`, `ui_geom.h` | broches, rails, géométrie de la dalle, échelle de l'interface et zone sûre |
| `main/ble.c` | NimBLE : GAP, sécurité, annonces, paramètres de connexion, service batterie, mode avion, journal de diagnostic |
| `main/ancs.c` / `ancs_parse.c` | client ANCS + CTS + AMS (`media.h`) ; réassemblage du Data Source |
| `main/notif_store.c` | stockage, tri par date, déduplication, nettoyage UTF-8 |
| `main/emoji.c`, `fonts/` | paquet d'émojis, reconnaissance des séquences, polices d'images ; polices de texte générées et table de couverture |
| `main/ui.c`, `ui_kit.h` | écrans, navigation, briques d'interface communes |
| `main/face_classic.c`, `face_pub.c` | cadrans analogiques (dessinés en code) |
| `main/app.h`, `apps.c`, `app_*.c` | cadre des apps, registre, apps fournies |
| `main/display.c` | CO5300 (`esp_lcd`, QSPI, synchro TE), CST9217, glue LVGL, dessin direct, double tap |
| `main/power.c`, `axp2101.c` | PMIC, rails, batterie, verrous PM, luminosité, motifs de vibration, historique d'énergie |
| `main/haptic.c` | pilote haptique DRV2605 |
| `main/xl9555.c`, `i2c_bus.c` | expandeur d'E/S, bus I2C partagé |
| `main/motion.c` | BHI260AP : chargement du firmware, pas, lever le poignet, accéléromètre pour les jeux |
| `main/sound.c` | bips et flux sur l'ampli I2S |
| `main/wclock.c` | PCF85063 (UTC), fuseau, resynchronisation de l'horloge système |
| `main/settings.c` | réglages en NVS |
| `main/i18n.c` | table français → anglais |
| `main/ota.c` | mise à jour Wi-Fi : point d'accès, page web, écriture OTA, validation, retour arrière |
| `components/bhy2/` | API Bosch BHI2xy et firmware du BHI260AP (BSD-3-Clause) |
| `components/helix_mp3/`, `helix_aac/` | décodeurs Helix pour Radio (RPSL / RCSL) |
