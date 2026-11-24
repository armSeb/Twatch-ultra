# MeshCore — messages LoRa depuis la montre

**Français** · [English](../en/meshcore.md)

La montre est un **nœud compagnon MeshCore** : sa radio LoRa SX1262 rejoint le réseau maillé
[MeshCore](https://meshcore.co.uk), et l'**app MeshCore** officielle de l'iPhone s'y connecte
en Bluetooth pour lire et envoyer des messages, gérer contacts et canaux, et régler la radio.
Le protocole est celui du firmware compagnon d'origine (v1.18.0) : l'app voit la montre comme
n'importe quel appareil MeshCore.

![App MeshCore : onglet Contacts, conversation de canal, onglet Radio](../img/meshcore.png)

## Activation

1. Montre : *Réglages › MeshCore*, activer **MeshCore**. La radio LoRa démarre et la montre
   annonce le service Bluetooth MeshCore en plus des siens.
2. iPhone : la montre doit déjà être appairée (*Réglages › Bluetooth › Appairer un iPhone*,
   voir le [README](../../README.fr.md)). Ouvrir l'app MeshCore et se connecter à
   **T-Watch-Ultra**. C'est le même lien Bluetooth que les notifications : rien d'autre à
   appairer.

Le premier démarrage crée l'identité du nœud (clés) et l'enregistre sur la montre. Les
contacts (350 au plus), les canaux (40 au plus) et les réglages radio sont stockés dans la
partition `storage` et survivent aux redémarrages et aux mises à jour.

## Réglages radio

Préréglage par défaut : **EU/UK Narrow**, 869,618 MHz, bande de 62,5 kHz, SF8, CR 4/8,
22 dBm. Tous les paramètres radio (fréquence, bande, facteur d'étalement, codage, puissance)
se règlent **depuis l'app MeshCore** et sont enregistrés sur la montre ; la montre les
affiche sans les modifier.

**Antenne** : *Réglages › MeshCore › Antenne externe* bascule entre l'antenne intégrée
(coupé) et une antenne sur le connecteur USB-C (activé). Effet immédiat.

## L'app MeshCore de la montre

L'app **MeshCore** (la première de la liste) a trois onglets :

- **Contacts** : les contacts de discussion et les salons, plus les conversations dont le
  contact a été supprimé depuis ;
- **Canaux** : les canaux réglés dans l'app iPhone ;
- **Radio** : nom du nœud, fréquence, SF et puissance, état de l'app iPhone (connectée, à
  jour, ou nombre de messages qui l'attendent), compteurs radio (`TX` avec les ratés, `RX`
  avec les abîmés, bruit de fond) et état de la carte SD.

Les listes mettent en tête les conversations qui ont des **non lus**, puis suivent l'ordre
**alphabétique** (émojis et ponctuation en début de nom ignorés, sans majuscules ni accents).
Chaque ligne montre le dernier message, son heure et une pastille avec le nombre de non lus ;
le titre des onglets compte aussi les non lus. Au-delà de 40 lignes, *Afficher plus* charge
la suite.

Toucher une ligne fait **glisser la conversation** : bulles, les plus anciennes en haut, nom
de l'expéditeur sur les canaux, messages envoyés depuis l'iPhone à droite, repère *Non lus*.
*Messages plus anciens* remonte dans l'historique (montre, puis carte SD). Retour : la flèche,
un glissé vers la droite ou le bouton. Émojis et tout texte UTF-8 s'affichent comme sur le
téléphone.

### Lu et non lu

- Ouvrir une conversation sur la montre la marque **lue**.
- Un message que l'app iPhone a **récupéré**, ou une conversation où l'on vient d'**écrire**
  depuis l'app, passe en lu sur la montre.
- **Lire un message sur la montre ne le retire pas à l'app** : il reste dans sa file (256
  messages au plus) et lui est remis quand elle se connecte. Le protocole MeshCore ne sait pas
  dire à l'app qu'un message est lu : il y apparaît donc comme nouveau.

### Historique et carte SD

La montre garde les **128 derniers messages** (reçus, et envoyés depuis l'app), avec leur état
lu / non lu, en mémoire et en flash : ils survivent à un redémarrage. Avec une **carte
micro-SD**, chaque message y est aussi archivé, un fichier par conversation dans `/meshcore`,
et les plus anciens se relisent depuis la carte.

La carte est détectée à l'insertion (en 3 s environ), montée tout de suite et démontée si on
la retire. *Réglages › Carte SD* affiche l'espace libre et propose **Éjecter la carte** (les
tampons sont écrits, puis la carte est démontée : on peut la retirer) et **Remonter la carte**.
Elle doit être formatée en FAT32. Le journal de la montre y est aussi écrit chaque minute,
dans `/logs/AAAA-MM-JJ.log`.

Quand MeshCore est coupé, l'app propose un bouton pour l'activer.

Pendant une mise à jour par Wi-Fi, MeshCore continue de recevoir (le Bluetooth est coupé :
l'app iPhone ne peut pas se connecter en attendant) ; la file et l'historique sont
enregistrés avant le redémarrage de la montre.

## Consommation

MeshCore activé, la radio écoute en permanence : l'autonomie baisse nettement. Le couper
dans *Réglages › MeshCore* quand il ne sert pas ; l'alimentation de la radio est alors
coupée.

Le SX1262 partage son bus SPI avec la carte SD et la puce NFC : tant que la radio tourne,
leurs alimentations restent allumées, sélection de puce au niveau haut, sinon les puces non
alimentées tirent le bus vers le bas et la radio ne répond plus (détails dans les
[notes matérielles](hardware.md)).

## Dépannage

| Ce que l'on voit | Signification |
|---|---|
| *Radio LoRa arrêtée : ESP_ERR_NOT_FOUND* | Le SX1262 ne répond pas sur le bus SPI. Couper puis réactiver MeshCore ; le journal série (lignes `sx1262:`) donne l'état de la puce. |
| `TX` augmente mais personne ne vous entend | Vérifier le réglage d'antenne, et que le préréglage est celui du réseau local. |
| `RX` reste à 0 | Idem ; un bruit de fond vers −120 dBm est normal, beaucoup plus haut signale un brouillage local. |
| L'app ne voit pas la montre | MeshCore doit être activé, et la montre appairée avec cet iPhone. |

## Pas encore

Alertes de message (bandeau et vibration), réponse depuis la montre, réception basse
consommation, et liaison TCP pour les clients sur ordinateur.
