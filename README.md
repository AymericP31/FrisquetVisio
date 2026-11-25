# FrisquetVisio

Bibliothèque Arduino pour ESP32 et SX1262 : réception des trames radio Frisquet Visio et pilotage des consignes de chauffage par zone, sans boîtier Visio Connect.

Le thermostat physique reste nécessaire. La carte écoute son échange avec la chaudière, puis envoie une trame modifiée après la réponse de celle-ci. Une nouvelle consigne est donc appliquée au prochain échange du thermostat, souvent environ 10 minutes plus tard.

> **Projet expérimental issu de rétro-ingénierie.** Le principe d'émission a été vérifié sur le firmware d'origine avec deux zones et une chaudière Visio d'avant 2015. Cette bibliothèque extraite et ses exemples restent à valider sur matériel. Les autres chaudières et la zone 3 ne sont pas validées. Surveiller les premiers essais et garder un moyen de revenir au thermostat physique.

## Fonctions

- Écoute FSK sur 868,96 MHz, via RadioLib et un SX1262.
- Température ambiante, consigne et mode radio des thermostats ; informations chaudière et module hydraulique lorsque leurs trames sont reçues.
- Modes par zone : **Auto** suit le thermostat physique ; **Heat** impose une consigne ; **Off** demande le hors-gel.
- Attente de la réponse chaudière correspondant au thermostat, puis émission `TrNum +4`, avec délai réglable de 0 à 1000 ms par zone.
- Trames en erreur CRC traitées par défaut si leurs valeurs sont plausibles (`Config::acceptCrcMismatch = true`), ignorées si `false`.
- Partie radio `FrisquetVisio` sans Wi-Fi, MQTT ni FreeRTOS.
- Partie Home Assistant optionnelle `FrisquetVisioHA` (MQTT Discovery via ArduinoHA), compilée seulement si ArduinoHA est installée. Le Wi-Fi reste dans le sketch.

La bibliothèque accepte les adresses chaudière `0x80`, satellites `0x08` à `0x0A` et modules hydrauliques `0x25` et `0x26` (le second est déduit, jamais observé). D'autres adresses ou modèles peuvent utiliser un protocole différent.

## Matériel et dépendances

- Heltec WiFi LoRa 32 V3 ou V4.3, ESP32-S3 avec SX1262 ; antenne 868 MHz adaptée.
- Arduino/PlatformIO et [RadioLib](https://github.com/jgromes/RadioLib).
- Home Assistant uniquement : Wi-Fi, broker MQTT et [ArduinoHA](https://github.com/dawidchyrzynski/arduino-home-assistant). Ce projet **n'est pas une intégration HACS**.
- Network ID de quatre octets propre à l'installation : voir [récupération du Network ID](docs/radio-visio.md#récupérer-le-network-id).

Les exemples utilisent NSS 8, SCK 9, MOSI 10, MISO 11, RESET 12, BUSY 13 et DIO1 14. Adapter ces broches pour un autre montage. L'amplificateur externe éventuel de la V4.3 n'est pas activé par ces exemples.

## Premiers essais

Depuis la racine du dépôt, avec PlatformIO :

1. Copier `examples/01_Sniffer/config.example.h` vers `examples/01_Sniffer/config_local.h`.
2. Renseigner le Network ID et mettre `NETWORK_ID_CONFIGURED` à `true`. Pour le récupérer, utiliser d'abord le mode d'écoute d'association décrit dans la [documentation radio](docs/radio-visio.md#récupérer-le-network-id).
3. Compiler l'exemple passif :

   ```text
   pio run -e sniffer_v3
   ```

   Pour une Heltec V4.3, utiliser `sniffer_v4`. Après téléversement sur la carte, ouvrir le moniteur série à 115200 bauds et vérifier les trames. Cet exemple n'émet pas.

4. Pour Home Assistant, copier `examples/02_HomeAssistant/config.example.h` vers `examples/02_HomeAssistant/config_local.h`, puis renseigner Wi-Fi, MQTT et Network ID.
5. Compiler avec `pio run -e home_assistant_v3` ou `pio run -e home_assistant_v4`, puis téléverser volontairement sur la carte. Vérifier les valeurs en mode Auto avant d'essayer Heat ou Off.

`config_local.h` et `.pio/` sont ignorés par Git. Ne pas publier de Network ID, mot de passe Wi-Fi, identifiant MQTT ou journal contenant des données privées.

## Choisir un exemple

| Exemple | Objectif | Environnements PlatformIO |
|---|---|---|
| [01_Sniffer](examples/01_Sniffer/) | Écoute passive, trames brutes, récupération du Network ID ; boucle Arduino classique. | `sniffer_v3`, `sniffer_v4` |
| [02_HomeAssistant](examples/02_HomeAssistant/) | Le plus simple avec HA : `FrisquetVisioHA`, 1 zone, 1 module, valeurs par défaut ; tout dans la boucle Arduino. Pour comprendre le principe. | `home_assistant_v3`, `home_assistant_v4` |
| [03_HomeAssistant_FreeRTOS](examples/03_HomeAssistant_FreeRTOS/) | Organisation du firmware d'origine : tâche radio (prio 4, réveillée par l'ISR) + tâche HA/MQTT (prio 3), boucle Arduino supprimée ; zones, modules, délais, CRC et TramType configurables. Chronologie TX commentée. | `home_assistant_freertos_v3`, `home_assistant_freertos_v4` |
| [04_Telnet_OLED](examples/04_Telnet_OLED/) | Diagnostic du firmware d'origine avec `FrisquetVisioStats` : trace Telnet de chaque trame RX/TX, commandes `radio`, `radio-zone1..3`, `radio-chaudiere`, `radio-hydrau`, `radio-heltec`, `radio-init`, `radio-reset` ; page radio OLED (bouton PRG). Écoute seule. | `telnet_oled_v3` |

Pour chaque exemple, copier son `config.example.h` en `config_local.h`. Les exemples 01 et 02 ne changent que le Network ID : délai **150 ms**, démarrage **Auto**, Heat **20 °C** et Off **5 °C** restent ceux de la bibliothèque. Chaque exemple affiche la ligne `config.acceptCrcMismatch` (défaut `true` : trames CRC traitées normalement) : à vous de choisir.

Dans l'exemple 02, une reconnexion Wi-Fi/MQTT peut bloquer la boucle quelques secondes et faire manquer des trames. L'exemple 03 évite ce problème : radio et HA sont dans deux tâches FreeRTOS, la radio est prioritaire.

## Home Assistant : `FrisquetVisioHA`

Mêmes entités que le firmware d'origine : `Zone_N` (climate Auto/Heat/Off), `ModeZoneN`, `Text`, `ModeChaudiere`, `TspEauN`, `TmesEauN`, `ModeHydN`, `TramType`. Les consignes Heat et Off sont sauvegardées en NVS (`frisquet` / `modesp`).

Températures du climate `Zone_N`, comme l'original : **température courante** = ambiante renvoyée par la chaudière (réponse 49 octets, octets 19–20 + 10×zone) ; **consigne** = celle du thermostat physique (trame thermostat, octets 17–18) en Auto, celle de HA en Heat/Off. La bibliothèque radio expose aussi l'ambiante envoyée par le thermostat (`Zone::ambientTemperature`).

`TramType` affiche chaque trame reçue ou émise pour vérifier la radio : `8: Zone 1`, `81: Réponse Chaudière Reçue Zone 1`, `251`/`253`/`255`/`257` module hydraulique, suffixe `(CRC non valide)`, `Envoie consigne Zone 1 : 20.0°C TrNum BC`, absence de réponse chaudière. `begin(..., tramBytes = true)` ajoute les octets en hexadécimal (équivalent de `ConfTram`).

```cpp
HAMqtt mqtt(wifiClient, device, FrisquetVisioHA::entityCount(ZONES, MODULES));
FrisquetVisioHA visioHA(visio);
// setup() : après HAMqtt, avant mqtt.begin()
visio.onEvent(FrisquetVisioHA::onRadioEvent, &visioHA);  // alimente TramType
visioHA.begin(ZONES, MODULES, mutex);  // mutex = nullptr si visio.loop() est dans la même tâche
mqtt.setBufferSize(1024);
// boucle, même tâche : mqtt.loop(); visioHA.loop();
```

`visioHA.loop()` prend le mutex quelques microsecondes pour appliquer les commandes HA et copier l'état radio, puis publie **sans** le mutex : une reconnexion MQTT ne bloque jamais la tâche radio. `onRadioEvent` ne fait qu'une copie dans une file FreeRTOS (8 événements, non bloquant).

Ajouter `dawidchyrzynski/home-assistant-integration@^2.1.0` aux `lib_deps` du projet. Sans ArduinoHA, `FrisquetVisioHA.cpp` compile vide.

## Diagnostic : `FrisquetVisioStats`

Compteurs RX par appareil (thermostats, chaudière, module hydraulique, non attribuées), émissions simulées par zone (TX, erreurs, confirmations chaudière, sans réponse, expirées), erreurs CRC. Appeler `stats.record(event, millis())` dans le callback `onEvent`. `formatTrace()` produit une ligne de trace, `report(device, ...)` le texte des commandes Telnet `radio-*`, `oled()` six lignes pour un écran 128×64. Objet POD : une autre tâche le copie sous le mutex de `visio` puis formate hors mutex. Les confirmations utilisent l'événement `EventType::TxConfirmed` (réponse chaudière à la trame simulée, notifié juste avant le `Receive` de la même trame). Voir l'exemple 04.

## Utiliser la bibliothèque

Référence complète des classes, constructeurs et méthodes : [docs/api.md](docs/api.md).

Ajouter ce dépôt comme bibliothèque à un projet PlatformIO, ou le placer dans le répertoire `lib/` du projet. `library.json` déclare RadioLib comme dépendance. Pour Arduino IDE, `library.properties` décrit la bibliothèque et sa dépendance.

```cpp
#include <SPI.h>
#include <FrisquetVisio.h>

SX1262 radio = new Module(8, 14, 12, 13);
FrisquetVisio visio(radio);

void setup() {
    Serial.begin(115200);
    SPI.begin(9, 11, 10, 8);

    FrisquetVisio::Config config;
    config.networkId[0] = 0x00; // Remplacer les 4 octets par le Network ID local.
    config.networkId[1] = 0x00;
    config.networkId[2] = 0x00;
    config.networkId[3] = 0x00;
    const int16_t status = visio.begin(config);
    if (status != RADIOLIB_ERR_NONE) Serial.printf("Erreur radio : %d\n", status);
}

void loop() {
    if (!visio.isReady()) {
        delay(1000);
        return;
    }
    const int16_t status = visio.loop();
    if (status != RADIOLIB_ERR_NONE) Serial.printf("Erreur radio : %d\n", status);
    delay(1);
}
```

Au démarrage, toutes les zones sont en **Auto** : aucune injection. L'index de zone commence à `0`. Après vérification de l'écoute, une application peut demander une consigne puis activer l'émission :

```cpp
const bool consigneOk = visio.setTargetTemperature(0, FrisquetVisio::Mode::Heat, 21.5f);
const bool modeOk = consigneOk && visio.setMode(0, FrisquetVisio::Mode::Heat);
if (!modeOk) Serial.println("Commande zone 1 refusee");
```

`setTargetTemperature` accepte **5 à 24 °C**, arrondis à **0,5 °C** (curseur du climate HA : 16 à 24 °C, comme le firmware d'origine). Utiliser `Mode::Off` pour régler sa consigne hors-gel distincte, puis `setMode(..., Mode::Auto)` pour arrêter les injections de la zone. `setTxDelay(zone, ms)` modifie le délai en RAM.

`zone(index)`, `boiler()` et `hydraulic(0..1)` donnent les dernières valeurs et leurs indicateurs de validité. `onEvent()` reçoit les trames RX/TX, leur statut RadioLib et les délais dépassés ; les octets transmis au callback ne restent valides que pendant cet appel. Une émission réussie n'est pas une preuve d'application : `Zone::lastTxConfirmed` indique seulement si une réponse radio correspondante a été reçue.

Appeler `loop()` fréquemment. Toute l'API, sauf l'interruption radio interne et son hook, doit être utilisée depuis **une seule tâche**, ou protégée par l'application. L'exemple 02 garde tout dans la boucle Arduino ; l'exemple 03 partage `visio` entre deux tâches avec un mutex.

## Interruptions radio et FreeRTOS

```text
SX1262 DIO1 -> ISR interne -> drapeau "paquet reçu"
                          -> hook optionnel -> notification FreeRTOS
Tâche radio -> loop() -> lecture/CRC -> réarmement RX -> décodage -> onEvent()
```

Sans hook, l'ISR pose seulement le drapeau et l'application doit appeler `loop()` régulièrement, comme dans le sniffer. Aucun SPI, décodage, MQTT ou affichage n'est effectué par l'ISR interne.

`onRadioInterrupt(callback, context)` ajoute un réveil applicatif, **sans dépendance FreeRTOS dans la bibliothèque**. L'enregistrer avant `begin()` ; la méthode renvoie `false` si cette instance a déjà activé sa radio. Le callback et son contexte doivent rester valides tant que la radio est active.

Les exemples FreeRTOS utilisent un callback `IRAM_ATTR` appelant uniquement `vTaskNotifyGiveFromISR()` et, si nécessaire, `portYIELD_FROM_ISR()`. Ne pas appeler `loop()`, `setMode()`, Serial, SPI, MQTT, un mutex ou une allocation mémoire depuis ce callback. `onEvent()`, à l'inverse, est appelé **dans la tâche qui exécute `loop()`**, jamais par l'ISR interne.

La tâche attend avec `ulTaskNotifyTake(pdTRUE, 1)` : une réception la réveille immédiatement si l'ordonnanceur le permet ; le timeout d'un tick permet de vérifier les émissions différées et les expirations même sans nouvelle réception. Ne pas remplacer ce timeout par `portMAX_DELAY`, sinon un TX différé peut rester bloqué. Un tick est l'attente demandée, pas une garantie de latence maximale.

### Émission TX : qui fait quoi

L'ISR ne fait **jamais** l'émission. C'est `loop()`, dans la tâche radio, qui émet :

```text
t=0       thermostat -> chaudière   ISR -> réveil -> loop() mémorise la trame (TrNum=B8)
t≈50 ms   chaudière  -> thermostat  ISR -> réveil -> loop() note « réponse reçue »
t=150 ms  (aucune interruption)     timeout 1 tick -> loop() : délai écoulé -> transmit()
                                    trame TrNum=BC + consigne, ~10 ms, ISR RX détachée
t≈200 ms  chaudière  -> thermostat  ISR -> réveil -> loop() : lastTxConfirmed = true
```

À 150 ms, rien ne réveille la tâche sauf le timeout : d'où l'interdiction de `portMAX_DELAY`. Le délai est compté depuis la réception de la trame du thermostat.

Pendant `transmit()`, l'interruption RX est détachée pour ne pas confondre TX_DONE et réception. Elle est réinstallée et la réception réarmée ensuite, même si le TX échoue. La tâche traite les erreurs de réarmement.

La notification réveille une tâche ; elle **ne stocke pas les trames**. Le drapeau interne n'est pas une file de paquets : des réceptions trop rapprochées peuvent être fusionnées ou perdues. Garder les callbacks courts et mesurer les latences sur matériel ; FreeRTOS ne supprime ni ce risque ni le fonctionnement half-duplex.

## Limites et sécurité

- L'afficheur du thermostat physique n'affiche pas la consigne demandée par Home Assistant. La chaudière peut reprendre la consigne physique à chaque cycle avant l'injection.
- `Off` signifie **hors-gel**, pas arrêt électrique de la chaudière.
- La carte ne peut ni recevoir ni émettre simultanément : une trame peut être manquée, même avec une attente de réponse et un CRC correct.
- L'absence de réponse captée ne prouve pas l'absence de réponse émise. Si la réponse au thermostat n'est pas reçue, aucune injection n'est faite pour cet échange.
- Les valeurs reçues sont les dernières connues, sans expiration. Après redémarrage, Home Assistant peut afficher un état MQTT retenu jusqu'à la prochaine trame.
- Les exemples Home Assistant redémarrent en Auto ; des commandes MQTT retenues peuvent ensuite changer ce mode. Les consignes Heat et Off sont relues depuis la NVS.
- Avec `acceptCrcMismatch = true`, un octet corrompu mais resté dans sa plage plausible (ex. 19,0 → 19,1 °C) peut être recopié dans la trame émise.
- Dans l'exemple 03, en cas d'arrêt de la radio ou de perte MQTT, les entités sont marquées indisponibles. Après une erreur radio, diagnostiquer puis redémarrer la carte.
- Protocole non officiel, sans affiliation avec Frisquet ; compatibilité non garantie.

Documentation détaillée : [protocole et modes radio](docs/radio-visio.md), [référence API](docs/api.md).

## Tests

Les tests C++ simulent la radio, les réponses, les délais, les zones, le compteur et le hook ISR (aucune lecture avant `loop()`, réarmement après TX, configuration et durée de vie du callback) :

```text
g++ -std=c++11 -Wall -Wextra -Werror -Itest/stubs -Isrc test/radio_test.cpp src/FrisquetVisio.cpp -o radio_test
./radio_test
```

Ils ne remplacent pas des essais sur chaudière, en particulier pour le temps de réponse et les collisions radio.

## Licence

[MIT](LICENSE) — Copyright (c) 2026 Pournain Aymeric. Les dépendances conservent leurs licences respectives.
