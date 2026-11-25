# Protocole radio Frisquet Visio

Synthèse des observations sur une installation Visio d'avant 2015, deux thermostats et un module hydraulique. Protocole non officiel, obtenu par rétro-ingénierie : les interprétations et la compatibilité avec d'autres modèles restent à confirmer.

L'émission `TrNum +4` a été validée sur les deux zones avec le firmware d'origine. **La bibliothèque extraite n'a pas encore été testée sur matériel.**

## Couche radio

| Paramètre | Valeur utilisée |
|---|---|
| Modulation | FSK, configuration `beginFSK` de RadioLib pour SX1262 |
| Fréquence | 868,96 MHz |
| Débit | 25 kbit/s |
| Déviation | 50 kHz |
| Bande RX | 187,2 kHz ; réglable selon la réception |
| Préambule | Valeur `4` passée à RadioLib (son API l'exprime en bits). N'agit que sur l'émission. Le forum HACF mesure 24 à 40 bits (3 à 5 × `AA`) sur les équipements d'origine ; valeur `4` validée sur l'installation d'origine |
| Sync word | Network ID propre à l'installation, quatre octets |
| CRC | Contrôlé par le SX1262 avec les paramètres RadioLib utilisés. Trame en erreur CRC : octets quand même lus ; traitée par défaut si plausible (voir ci-dessous), ignorée si `acceptCrcMismatch = false` |

Le SX1262 est **half-duplex** : il ne reçoit pas pendant qu'il émet. Le CRC vérifie une trame détectée, mais ne compte pas les trames jamais reçues.

### CRC

Un CRC faux indique qu'au moins un bit est erroné, sans dire lequel. L'en-tête (longueur, adresses, type, `0x17`) est contrôlé au décodage ; pour une trame CRC, les octets exploités doivent en plus être dans ces plages :

| Trame | Octets | Plage |
|---|---|---|
| Thermostat (23) | 15-16 ambiance / 17-18 consigne / 20 mode | 0-40 °C / 5-30 °C / `00`-`05`, `10`, `14` |
| Chaudière (49) | 7-8 T° extérieure / 17 mode | -20 à 35 °C (saturée à 35) ou `05 0A` (sans sonde) / `00`-`3F` |
| Chaudière (49), par zone gérée | `19+10z` ambiance / `21+10z` consigne | 0-40 °C / 5-30 °C |
| Chaudière → module (19) | 15-16 T° eau / 18 mode | 0-70 °C / 0-3, 9 (chaudière arrêtée), 42 |
| Module → chaudière (9) | 7-8 T° eau | 0-70 °C |
| Chaudière → module (15) | 13 mode | `00`-`3F` |
| Message thermostat (`43`) | 6-7 longueur | = taille de la trame − 8 |

Seules les zones gérées (`zoneCount`) sont contrôlées : une zone absente contient des valeurs quelconques (ex. `04 F6`). Une petite corruption restée dans la plage n'est pas détectable. Pour une trame thermostat CRC, la bibliothèque attend aussi la réponse chaudière et compare exactement ses octets 15–22 avec l'écho chaudière (`19 + 10 × zone` à `26 + 10 × zone`). Si les blocs diffèrent, l'injection est annulée ; s'ils correspondent, l'émission simulée peut continuer.

## Équipements et en-tête

Adresses observées : chaudière `0x80`, thermostats Z1/Z2/Z3 `0x08`/`0x09`/`0x0A`, module hydraulique 1 `0x25`. Le module hydraulique 2 en `0x26` est **déduit** du firmware d'origine, jamais observé. La bibliothèque ne décode ni ne pilote actuellement une chaudière d'adresse différente.

| Octet | Contenu |
|---|---|
| 0 | Destinataire |
| 1 | Émetteur |
| 2 | `TrID`, identifiant de liaison |
| 3 | `TrNum`, numéro de transaction |
| 4 | Type : `01` ou `03` pour une demande, `81` ou `83` pour une réponse |
| 5 | `17` pour les trames de fonctionnement observées, `43` pour un message thermostat |

Les températures ci-dessous sont des entiers signés sur deux octets, poids fort en premier, divisés par 10. Exemple : `00 CD` = 20,5 °C.

### Compteur `TrNum`

- Une nouvelle transaction du thermostat incrémente le numéro de **4**.
- Une répétition d'une transaction sans réponse l'incrémente de **1**. Le nombre d'essais et leur intervalle restent à confirmer selon le modèle ; certaines captures montraient environ 3,9 s entre répétitions.
- La réponse chaudière observée reprend destination/source inversées, `TrID` et `TrNum`.
- L'implémentation suppose que les deux bits faibles indiquent la répétition : `base = TrNum & 0xFC`, puis `TrNum simulé = (base + 4) & 0xFF`. Exemple : `B8`, `B9`, `BA` et `BB` donnent tous `BC`. `FC` donne `00`.

La chaudière répondait à l'ancien essai `+1`, mais la régulation Z1 n'appliquait pas la consigne. Avec `+4`, elle l'a appliquée sur les deux zones testées. La cause interne n'est pas connue.

## Trames utiles

### Thermostat vers chaudière : 23 octets

Adresses `80 08/09/0A`, type `01` :

| Octets | Contenu |
|---|---|
| 15–16 | Température ambiante |
| 17–18 | Consigne du thermostat physique |
| 20 | Mode radio de la zone |

### Chaudière vers thermostat : 49 octets

Source `80`, type `81` :

| Octets | Contenu |
|---|---|
| 7–8 | Température extérieure. Avec un module hydraulique, c'est la sonde filaire du module (voir trame 17 octets). `05 0A` (129,0 °C) = pas de sonde : `outdoorTemperature = NAN` |
| 9–10 | Toujours `00 00` |
| 11–16 | Date et heure BCD : année, mois, jour, heure, minute, seconde |
| 17 | État chaudière, champ de bits (voir ci-dessous) |
| 18 | Jour de la semaine, `01` = lundi (relevés forum : `01` le lundi 05/01/2026, `02` le mardi 14/10/2025) |
| 19–20 / 21–22 / 24 | Z1 : température ambiante / consigne / mode ; 23 = `00`, 25–28 constants (`00 C6 00 C6` observé) |
| 29–30 / 31–32 / 34 | Z2 : température ambiante / consigne / mode |
| 39–40 / 41–42 / 44 | Z3 : température ambiante / consigne / mode ; non validés sur matériel |

Une zone absente renvoie une ambiance `04 F6` (127,0 °C) et une consigne `00 00`.

#### État chaudière (octet 17 de la trame 49, octet 13 de la trame 15)

Valeur sur 8 bits, `boiler().mode` la conserve brute. Interprétation du [forum HACF](https://forum.hacf.fr/t/pilotage-chaudiere-frisquet-eco-radio-system-visio-suite-partie-1/67032/620), à confirmer :

| Bit | Sens |
|---|---|
| `0x08` | Chauffage en fonctionnement (`08` relevé sur l'installation d'origine) |
| `0x04` | Arrêt chauffage par contact sec |
| `0x10` ou `0x20` | Frisquet Connect présent (`28` relevé chez un utilisateur Connect) |
| `09` | Anomalie (libellé du firmware d'origine) |

Le capteur HA `ModeChaudiere` affiche `Heating Off`, `Heating On`, `Contact sec` ou `Anomalie`, suivi de `+ Connect` si un bit `0x30` est présent.

### Chaudière et modules hydrauliques

Module 1 `0x25`, module 2 `0x26` (supposé). Accès : `visio.hydraulic(0)` ou `hydraulic(1)`.

| Taille | Sens | Type | Champs observés |
|---|---|---|---|
| 17 | Module → chaudière | `01` | Température extérieure **filaire**, mesurée par le module hydraulique, aux octets 15–16 (relevé `00 46` = 7,0 °C) ; la chaudière la renvoie aux thermostats (octets 7–8 de la trame 49). Octets 7–14 constants |
| 15 | Chaudière → module | `81` | Date, mode chaudière à l'octet 13 |
| 19 | Chaudière → module | `03` | Consigne eau aux octets 15–16, mode module à l'octet 18 |
| 9 | Module → chaudière | `83` | Température eau de départ aux octets 7–8 |

La bibliothèque lit la T° extérieure dans la trame 49 (`boiler().outdoorTemperature`), présente avec ou sans module.

### Message thermostat : octet 5 = `43`

Relevé sur le forum HACF ([#775](https://forum.hacf.fr/t/pilotage-chaudiere-frisquet-eco-radio-system-visio-suite-partie-1/67032/775)). Thermostat → chaudière, type `01`. Octets 6–7 = longueur du texte ASCII qui suit :

```text
80 08 00 B4 01 43 00 1C "SATELLITE Z1\0CHANGER PILES \0"   -> piles faibles
80 08 00 04 01 43 00 00                                     -> message effacé
```

La chaudière répond `08 80 .. .. 81 43`. Une trame vide est aussi envoyée au redémarrage du thermostat. La bibliothèque met `zone(n)->batteryLow` à `true` pour un texte non vide et à `false` pour une trame vide ; `FrisquetVisioHA` publie `PileZoneN` (binary_sensor `battery`). Valeur `false` après un redémarrage de la carte, jusqu'au message suivant.

## Modes radio de zone

Octet 20 de la trame thermostat, repris à l'octet 24/34/44 de la réponse chaudière. Relevés de l'installation d'origine, confirmés par le [forum HACF](https://forum.hacf.fr/t/pilotage-chaudiere-frisquet-eco-radio-system-visio-suite-partie-1/67032/42) :

| Code | Mode | Libellé `ModeZoneN` |
|---|---|---|
| `00` | Nuit (réduit) permanent | `Nuit` |
| `01` | Jour (confort) permanent | `Jour` |
| `02` | Dérogation nuit | `Dérogation nuit` |
| `03` | Dérogation jour | `Dérogation jour` |
| `04` | Programme auto, période nuit | `Auto nuit` |
| `05` | Programme auto, période jour | `Auto jour` |
| `10` | Hors-gel | `Hors gel` |
| `14` | Hors-gel, programme auto | `Auto hors gel` |

Lecture en bits : `0x01` jour/confort, `0x02` dérogation, `0x04` programme auto, `0x10` hors-gel. Frisquet Connect utiliserait d'autres codes (`06`–`09`), non gérés.

Ces codes radio ne sont pas les modes de l'interface Home Assistant. La bibliothèque reprend le comportement du firmware d'origine : en Heat, elle envoie `01` (jour permanent) si le mode physique reçu est `00`/`01`, sinon `05` (auto jour). En Off, elle envoie respectivement `10` ou `14`.

## Modes de commande Home Assistant

L'exemple Home Assistant crée une entité `climate` par zone configurée :

| Mode HA | Fonction |
|---|---|
| **Auto** | Thermostat physique maître ; pas d'injection pour cette zone. La consigne physique reçue est affichée dans HA. |
| **Heat** | La carte envoie la consigne Heat de HA après l'échange réel. |
| **Off** | La carte demande le hors-gel avec la consigne Off de HA ; ce n'est pas un arrêt électrique. |

Les consignes Heat et Off sont distinctes. La bibliothèque radio seule les garde en RAM ; `FrisquetVisioHA` (exemples 02 et 04) les enregistre en NVS. Plage acceptée : 5–24 °C, pas de 0,5 °C ; le curseur du climate HA est borné à 16–24 °C comme le firmware d'origine. Un changement attend le prochain échange du thermostat, souvent environ 10 minutes mais sans délai garanti ; son afficheur ne montre pas la consigne HA. Au retour en Auto, la carte n'injecte plus et le thermostat reprend la main à son échange suivant.

## Séquence d'injection

```text
Thermostat Z1  -> chaudière : TrNum B8, consigne physique
Chaudière      -> Z1        : TrNum B8, même TrID
Heltec (Z1)    -> chaudière : TrNum BC, consigne et mode demandés
Chaudière      -> Z1        : TrNum BC
... cycle suivant ...
Thermostat Z1  -> chaudière : TrNum BC
Chaudière      -> Z1        : TrNum BC
Heltec (Z1)    -> chaudière : TrNum C0
```

Le délai configurable est mesuré **depuis le traitement de la réception du thermostat**, pas depuis la réponse chaudière. La carte attend les deux conditions : délai écoulé et réponse chaudière valide correspondant à la zone, aux adresses, au `TrID` et au `TrNum`. Aucun envoi si la réponse n'a pas été captée. `150 ms` est la valeur par défaut ; `100 ms` a été essayé sur l'installation d'origine. Plage configurable : `0..1000 ms`.

La bibliothèque limite les doubles émissions d'une même base de transaction sur 60 s. Une transaction en attente expire après 10 s sans émission. Après un TX, elle attend jusqu'à 2 s une réponse correspondante avant de permettre un autre TX ; un événement signale l'expiration. Ces garde-fous de la bibliothèque sont à vérifier sur matériel. Ils ne garantissent pas l'absence de collision ni la réception de toutes les trames.

La trame de réponse peut montrer la consigne simulée, mais seule l'observation du chauffage confirme son application. Une réponse non captée ne prouve pas que la chaudière n'a pas répondu.

## Récupérer le Network ID

Méthode d'écoute d'association décrite par [OpenFrisquetVisio](https://github.com/FreedomNX/OpenFrisquetVisio), trouvé via le [fil Frisquet Connect de la communauté francophone Home Assistant](https://forum.hacf.fr/t/integration-frisquet-connect/34437). Elle reste à tester avec cet exemple et le modèle de chaudière concerné.

En mode association, la chaudière peut diffuser sur le sync word `FF FF FF FF` une trame de type `41`, avec une longueur `04` à l'octet 6 et le Network ID aux octets 7–10.

1. Copier `examples/01_Sniffer/config.example.h` vers `examples/01_Sniffer/config_local.h`.
2. Mettre `NETWORK_ID_RECOVERY` à `true`, compiler puis téléverser `sniffer_v3` ou `sniffer_v4`. Le mode écoute reste passif ; la bibliothèque refuse les modes d'émission avec le sync word broadcast.
3. Ouvrir le moniteur série à 115200 bauds.
4. Sur la chaudière, lancer une demande d'association Connect ou de remplacement du satellite Z1, sans la valider jusqu'au bout. Les menus dépendent du modèle.
5. Noter la ligne `Network ID : XX XX XX XX`, puis **annuler la procédure sur la chaudière sans confirmer le remplacement**. Vérifier que les thermostats existants restent opérationnels.
6. Dans `config_local.h`, renseigner ces quatre octets, mettre `NETWORK_ID_CONFIGURED` à `true` et `NETWORK_ID_RECOVERY` à `false`, puis recompiler et téléverser.
7. Vérifier la réception des thermostats et des réponses chaudière. Ne pas publier son Network ID ni le journal d'association.

Si aucune trame n'apparaît, vérifier l'antenne, la fréquence, les broches et la procédure propre à la chaudière. Ne pas répéter une association au hasard : une confirmation peut modifier les appairages existants.

### Méthode passive, sans association (non implémentée)

Décrite sur le [forum HACF (#730)](https://forum.hacf.fr/t/pilotage-chaudiere-frisquet-eco-radio-system-visio-suite-partie-1/67032/730) : écouter toutes les trames sans filtre de sync word. CRC désactivé, détecteur de préambule 16 bits, faux sync word `0xA8`, puis décalage du buffer reçu de 2 bits et décodage/vérification « à la main ». Le Network ID apparaît alors en tête de chaque trame. Aucune action sur la chaudière, mais il faut un sniffer dédié ; l'exemple `01_Sniffer` ne le fait pas.

## Interruptions et exécution temps réel

Le front DIO1 du SX1262 déclenche une ISR RadioLib. L'ISR interne de la bibliothèque pose un drapeau, puis appelle le hook `onRadioInterrupt()` s'il a été enregistré avant `begin()`. Le hook peut notifier une tâche FreeRTOS avec les primitives `FromISR` ; il ne doit ni lire la radio ni décoder une trame.

La tâche réveillée appelle `loop()` : lecture du paquet et statut CRC, réarmement RX, décodage des trames valides, puis callback applicatif `onEvent()`. C'est aussi `loop()` qui vérifie les délais TX et les expirations. `onEvent()` s'exécute donc hors ISR, mais un callback lent retarde toujours les traitements radio suivants.

L'[exemple FreeRTOS](../examples/03_HomeAssistant_FreeRTOS/) attend une notification avec un timeout d'un tick pour continuer à gérer ces délais sans nouvelle IRQ. Le délai configuré est une condition d'éligibilité, pas une garantie d'émission à la milliseconde exacte : il faut encore la réponse chaudière, la disponibilité radio et le temps CPU.

L'IRQ RX est détachée pendant le TX et rétablie ensuite. Une notification n'est pas une file de trames : ni le drapeau ni la notification ne garantissent la récupération de plusieurs paquets accumulés. Les pertes, collisions et latences restent à mesurer sur matériel. Voir [l'architecture et les contraintes ISR](../README.md#interruptions-radio-et-freertos).

## Diagnostic

L'événement `Receive` contient les octets reçus, RSSI et statut RadioLib ; un CRC invalide est traité si la trame est plausible (`acceptCrcMismatch = true`), sinon jamais utilisé pour déclencher un TX. `Transmit` indique le statut de la radio, pas l'application de la consigne. `TransactionExpired` et `ResponseTimeout` signalent une attente dépassée. Les éventuels défauts de réarmement RX sont renvoyés par `loop()` et arrêtent la réception jusqu'à réinitialisation.

Garder un journal horodaté des trames et surveiller thermostat, chaudière, pompe et vanne lors des premiers essais. En cas de doute, remettre la zone en Auto ou arrêter la carte ; le thermostat physique reste en place.
