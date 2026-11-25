# Référence API

Référence des classes publiques de la bibliothèque. Fonctionnement radio : [radio-visio.md](radio-visio.md). Mise en route : [README](../README.md).

| Classe | En-tête | Dépendances | Rôle |
|---|---|---|---|
| [`FrisquetVisio`](#frisquetvisio) | `FrisquetVisio.h` | RadioLib | Radio seule : écoute, décodage, émission simulée |
| [`FrisquetVisioHA`](#frisquetvisioha) | `FrisquetVisioHA.h` | ArduinoHA, FreeRTOS | Passerelle Home Assistant (MQTT Discovery) |

---

## FrisquetVisio

```cpp
#include <FrisquetVisio.h>

class FrisquetVisio
```

Écoute le réseau radio Frisquet Visio (FSK 868,96 MHz) avec un SX1262, décode les trames thermostats, chaudière et modules hydrauliques, et pilote une zone en renvoyant la trame du thermostat modifiée (`TrNum = (TrNum & 0xFC) + 4`, consigne, mode) après la réponse de la chaudière.

Aucune dépendance Wi-Fi, MQTT ou Home Assistant.

**Contrainte de thread :** toutes les méthodes publiques doivent être appelées depuis **une seule tâche**, ou sous un mutex commun. Seule l'interruption radio interne tourne ailleurs.

### Résumé

**Types imbriqués**

| Type | Description |
|---|---|
| `enum class` [`Mode`](#mode) | Mode demandé pour une zone : `Auto`, `Heat`, `Off` |
| `enum class` [`EventType`](#eventtype) | Type d'événement transmis à `onEvent()` |
| `struct` [`Config`](#config) | Paramètres passés à `begin()` |
| `struct` [`Zone`](#zone) | État d'une zone (thermostat) |
| `struct` [`Boiler`](#boiler) | État de la chaudière |
| `struct` [`Hydraulic`](#hydraulic) | État d'un module hydraulique |
| `struct` [`Event`](#event) | Événement radio |
| `using EventCallback` | `void (*)(const Event&, void* context)` |
| `using InterruptCallback` | `void (*)(void* context)` — appelé **dans l'ISR** |

**Constantes**

| Type | Nom | Valeur | Description |
|---|---|---|---|
| `uint8_t` | `MaxZones` | 3 | Thermostats `0x08`, `0x09`, `0x0A` |
| `uint8_t` | `MaxHydraulicModules` | 2 | Modules `0x25`, `0x26` |
| `uint16_t` | `MaxTxDelayMs` | 1000 | Délai TX maximal accepté |
| `int16_t` | `InvalidConfig` | -32000 | Configuration refusée par `begin()` |
| `int16_t` | `NotReady` | -32001 | `loop()` appelé sans radio active |
| `int16_t` | `RadioInUse` | -32002 | Radio déjà démarrée (cette instance ou une autre) |

Les autres codes négatifs sont ceux de RadioLib (`RADIOLIB_ERR_*`).

**Constructeurs**

| | |
|---|---|
| [`FrisquetVisio(SX1262& radio)`](#frisquetvisiosx1262-radio) | Associe la bibliothèque à un module SX1262 |

**Méthodes publiques**

| Retour | Méthode |
|---|---|
| `int16_t` | [`begin(const Config& config)`](#begin) |
| `int16_t` | [`loop()`](#loop) |
| `bool` | [`isReady() const`](#isready) |
| `void` | [`onEvent(EventCallback callback, void* context = nullptr)`](#onevent) |
| `bool` | [`onRadioInterrupt(InterruptCallback callback, void* context = nullptr)`](#onradiointerrupt) |
| `bool` | [`setMode(uint8_t zone, Mode mode)`](#setmode) |
| `bool` | [`setTargetTemperature(uint8_t zone, Mode mode, float temperature)`](#settargettemperature) |
| `bool` | [`setTxDelay(uint8_t zone, uint16_t delayMs)`](#settxdelay) |
| `const Zone*` | [`zone(uint8_t index) const`](#zone-1) |
| `const Boiler&` | [`boiler() const`](#boiler-1) |
| `const Hydraulic*` | [`hydraulic(uint8_t index) const`](#hydraulic-1) |
| `const Config&` | [`config() const`](#config-1) |

### Types imbriqués

#### Mode

```cpp
enum class Mode : uint8_t { Auto, Heat, Off };
```

| Valeur | Comportement |
|---|---|
| `Auto` | Aucune émission : le thermostat physique décide. Valeur au démarrage. |
| `Heat` | La carte impose `Zone::heatSetpoint` (défaut 20 °C). |
| `Off` | La carte impose le hors-gel `Zone::frostSetpoint` (défaut 5 °C). Ce n'est pas un arrêt électrique. |

#### EventType

```cpp
enum class EventType : uint8_t { Receive, Transmit, TransactionExpired, ResponseTimeout };
```

| Valeur | Émis quand | `Event::status` |
|---|---|---|
| `Receive` | Une trame est reçue, CRC compris | Code RadioLib de lecture (`RADIOLIB_ERR_CRC_MISMATCH` possible) |
| `Transmit` | Une trame simulée vient d'être émise | Résultat de l'émission ; ne prouve pas que la consigne est appliquée |
| `TransactionExpired` | Trame thermostat reçue mais pas de réponse chaudière en 10 s : rien n'est émis | `RADIOLIB_ERR_NONE` |
| `ResponseTimeout` | Trame simulée émise mais pas de réponse chaudière en 2 s | `RADIOLIB_ERR_NONE` |

#### Config

```cpp
struct Config;
```

| Champ | Type | Défaut | Description |
|---|---|---|---|
| `networkId` | `uint8_t[4]` | `{0,0,0,0}` | Network ID de l'installation, utilisé comme mot de synchronisation. `FF FF FF FF` = mode récupération : seule `Mode::Auto` est acceptée. Voir [radio-visio.md](radio-visio.md). |
| `zoneCount` | `uint8_t` | 3 | Nombre de thermostats gérés, 1 à 3 |
| `frequencyMHz` | `float` | 868.96 | Fréquence radio |
| `rxBandwidthKHz` | `float` | 187.2 | Bande passante RX |
| `outputPowerDbm` | `int8_t` | 10 | Puissance d'émission |
| `boostedGain` | `bool` | `true` | Gain RX renforcé du SX1262 |
| `txDelayMs` | `uint16_t[3]` | `{150,150,150}` | Délai minimal (0–1000 ms) entre la trame thermostat et l'émission simulée. L'émission attend **en plus** la réponse chaudière. |
| `acceptCrcMismatch` | `bool` | `true` | `true` : trame en erreur CRC traitée comme une trame correcte. `false` : seulement signalée. |

#### Zone

```cpp
struct Zone;
```

| Champ | Type | Source |
|---|---|---|
| `satelliteValid` | `bool` | Au moins une trame thermostat reçue |
| `boilerValid` | `bool` | Au moins une réponse chaudière reçue |
| `ambientTemperature` | `float` | Thermostat, octets 15–16 |
| `satelliteSetpoint` | `float` | Thermostat, octets 17–18 |
| `boilerAmbientTemperature` | `float` | Chaudière, octets 19–20 (+10 par zone) |
| `boilerSetpoint` | `float` | Chaudière, octets 21–22 (+10 par zone) : consigne retenue |
| `satelliteRadioMode` | `uint8_t` | Octet 20 de la trame thermostat |
| `boilerRadioMode` | `uint8_t` | Mode de la zone vu par la chaudière |
| `mode` | `Mode` | Mode demandé à la bibliothèque |
| `heatSetpoint` | `float` | Consigne émise en `Heat` |
| `frostSetpoint` | `float` | Consigne émise en `Off` |
| `txCount` | `uint32_t` | Trames simulées émises avec succès |
| `lastTxConfirmed` | `bool` | La chaudière a répondu à la dernière émission |

Les températures valent `NAN` tant qu'aucune trame n'a été reçue.

#### Boiler

| Champ | Type | Description |
|---|---|---|
| `dataValid` | `bool` | Trame chaudière → thermostat reçue |
| `modeValid` | `bool` | Mode chaudière reçu |
| `outdoorTemperature` | `float` | Température extérieure |
| `mode` | `uint8_t` | 0 arrêt, 8 chauffage, 9 anomalie… |

#### Hydraulic

Index 0 = module `0x25`, index 1 = module `0x26` (adresse déduite, jamais observée).

| Champ | Type | Description |
|---|---|---|
| `setpointValid` | `bool` | Trame chaudière → module reçue |
| `temperatureValid` | `bool` | Trame module → chaudière reçue |
| `waterSetpoint` | `float` | Consigne de départ d'eau demandée par la chaudière |
| `waterTemperature` | `float` | Départ d'eau mesuré par le module |
| `mode` | `uint8_t` | 0 arrêt, 1 baisse, 2 stable, 3 hausse, 42 sécurité |

#### Event

| Champ | Type | Description |
|---|---|---|
| `type` | `EventType` | Nature de l'événement |
| `data` | `const uint8_t*` | Octets RX/TX ; **valides seulement pendant le callback**, à copier sinon. `nullptr` pour les délais. |
| `length` | `size_t` | Longueur de `data` |
| `status` | `int16_t` | Code RadioLib |
| `rssi` | `float` | RSSI en dBm ; `NAN` hors réception |
| `zone` | `int8_t` | Index de zone, -1 si non concerné |

### Constructeurs

#### FrisquetVisio(SX1262& radio)

```cpp
explicit FrisquetVisio(SX1262& radio);
```

Associe la bibliothèque au module radio. N'accède pas au matériel ; voir `begin()`. Non copiable. Le destructeur détache l'interruption si cette instance est active.

| Paramètre | |
|---|---|
| `radio` | Module SX1262 RadioLib, doit vivre plus longtemps que l'objet |

```cpp
SX1262 radio = new Module(8, 14, 12, 13);   // NSS, DIO1, RST, BUSY (Heltec V3/V4)
FrisquetVisio visio(radio);
```

### Méthodes publiques

#### begin

```cpp
int16_t begin(const Config& config);
```

Configure le SX1262 et lance l'écoute. `SPI.begin()` doit déjà être appelé. Une seule instance active à la fois.

| Retour | Cas |
|---|---|
| `RADIOLIB_ERR_NONE` | Radio en écoute |
| `InvalidConfig` | `zoneCount` hors 1–3, fréquence ou bande non finie, `txDelayMs` > 1000, ou mode ≠ `Auto` avec Network ID `FF FF FF FF` |
| `RadioInUse` | Déjà démarrée, ou autre instance active |
| autre négatif | Erreur RadioLib pendant l'initialisation |

#### loop

```cpp
int16_t loop();
```

À appeler le plus souvent possible, toujours depuis la même tâche. À chaque appel :

1. lit la trame signalée par l'interruption, réarme la réception, décode, appelle `onEvent(Receive)` ;
2. gère les délais (`ResponseTimeout`, `TransactionExpired`) ;
3. émet **au plus une** trame simulée si la réponse chaudière est reçue et `txDelayMs` écoulé.

L'émission dépend du temps, pas d'une interruption : `loop()` doit tourner même sans réception (voir [FreeRTOS](#exemple-freertos)).

| Retour | Cas |
|---|---|
| `RADIOLIB_ERR_NONE` | Rien à signaler |
| `RADIOLIB_ERR_CRC_MISMATCH` | Trame reçue en erreur CRC (traitée si `acceptCrcMismatch`) |
| `NotReady` | `begin()` non réussi ou réception arrêtée |
| autre négatif | Erreur RadioLib (lecture, réarmement RX ou émission) |

Si le réarmement RX échoue, `isReady()` passe à `false` : réinitialiser la radio.

#### isReady

```cpp
bool isReady() const;
```

`true` si la radio est en écoute.

#### onEvent

```cpp
void onEvent(EventCallback callback, void* context = nullptr);
```

Enregistre le callback d'événements. Appelé **dans la tâche qui exécute `loop()`**, jamais dans l'ISR : Serial, file FreeRTOS, etc. autorisés, mais rester court pour ne pas retarder la radio. `nullptr` désactive.

```cpp
void onRadio(const FrisquetVisio::Event& e, void*) {
    if (e.type == FrisquetVisio::EventType::Receive)
        Serial.printf("RX %u octets, RSSI %.1f\n", (unsigned)e.length, e.rssi);
}
visio.onEvent(onRadio);
```

#### onRadioInterrupt

```cpp
bool onRadioInterrupt(InterruptCallback callback, void* context = nullptr);
```

Hook facultatif appelé **depuis l'interruption radio**, typiquement pour réveiller une tâche FreeRTOS. À enregistrer **avant** `begin()`.

Le callback doit être `IRAM_ATTR` et très court : pas de Serial, SPI, MQTT, mutex, allocation ni appel à la bibliothèque.

| Retour | |
|---|---|
| `true` | Hook enregistré |
| `false` | Radio déjà active : refusé |

#### setMode

```cpp
bool setMode(uint8_t zone, Mode mode);
```

Change le mode demandé pour une zone. Appliqué au prochain échange du thermostat (souvent ~10 min). Le retour en `Auto` annule une émission en attente.

| Retour | |
|---|---|
| `true` | Accepté |
| `false` | Zone hors `zoneCount`, mode invalide, ou mode ≠ `Auto` en récupération (`FF FF FF FF`) |

#### setTargetTemperature

```cpp
bool setTargetTemperature(uint8_t zone, Mode mode, float temperature);
```

Règle la consigne `Heat` ou `Off` d'une zone, sans changer le mode. Plage 5–24 °C, arrondie à 0,5 °C.

| Paramètre | |
|---|---|
| `zone` | 0 à `zoneCount - 1` |
| `mode` | `Mode::Heat` → `heatSetpoint`, `Mode::Off` → `frostSetpoint` ; `Auto` refusé |
| `temperature` | °C |

Retour `false` si un paramètre est hors plage.

#### setTxDelay

```cpp
bool setTxDelay(uint8_t zone, uint16_t delayMs);
```

Change le délai TX d'une zone en RAM (0–`MaxTxDelayMs`). Retour `false` si hors plage.

#### zone

```cpp
const Zone* zone(uint8_t index) const;
```

État de la zone `index`, ou `nullptr` si `index >= zoneCount`.

#### boiler

```cpp
const Boiler& boiler() const;
```

État de la chaudière.

#### hydraulic

```cpp
const Hydraulic* hydraulic(uint8_t index) const;
```

État du module `index` (0 ou 1), ou `nullptr` si hors plage.

#### config

```cpp
const Config& config() const;
```

Configuration courante, `txDelayMs` modifié par `setTxDelay()` compris.

### Exemple minimal

```cpp
SX1262 radio = new Module(8, 14, 12, 13);
FrisquetVisio visio(radio);

void setup() {
    SPI.begin(9, 11, 10, 8);
    FrisquetVisio::Config config;
    const uint8_t id[4] = {0x12, 0x34, 0x56, 0x78};   // votre Network ID
    memcpy(config.networkId, id, 4);
    config.zoneCount = 1;
    visio.begin(config);
    visio.setTargetTemperature(0, FrisquetVisio::Mode::Heat, 19.5f);
    visio.setMode(0, FrisquetVisio::Mode::Heat);
}

void loop() { visio.loop(); }
```

### Exemple FreeRTOS

```cpp
void IRAM_ATTR wake(void* task) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(task), &woken);
    if (woken) portYIELD_FROM_ISR();
}

void radioTask(void*) {
    visio.onRadioInterrupt(wake, xTaskGetCurrentTaskHandle());   // avant begin()
    visio.begin(config);
    for (;;) {
        visio.loop();
        ulTaskNotifyTake(pdTRUE, 1);   // 1 tick, jamais portMAX_DELAY : TX différé
    }
}
```

Exemple complet : [03_HomeAssistant_FreeRTOS](../examples/03_HomeAssistant_FreeRTOS/main.cpp).

---

## FrisquetVisioHA

```cpp
#include <FrisquetVisioHA.h>

class FrisquetVisioHA
```

Passerelle Home Assistant par MQTT Discovery (ArduinoHA). Compilée seulement si ArduinoHA (`dawidchyrzynski/home-assistant-integration`) est installée. Le Wi-Fi, `HADevice` et `HAMqtt` restent dans le sketch.

**Une seule instance** par programme : les callbacks ArduinoHA sont statiques.

### Entités créées

Mêmes identifiants que le firmware d'origine.

| Identifiant | Type HA | Nombre | Contenu |
|---|---|---|---|
| `Zone_N` | climate | par zone | Modes Auto/Heat/Off, consigne (curseur 16–24 °C), température = ambiante renvoyée par la chaudière |
| `ModeZoneN` | sensor texte | par zone | Mode radio du thermostat (Heure pleine, Hors gel…) |
| `Text` | sensor °C | 1 | Température extérieure |
| `ModeChaudiere` | sensor texte | 1 | Mode chaudière |
| `TramType` | sensor texte | 1 | Dernière trame reçue/émise, RSSI, CRC, absence de réponse |
| `TspEauN` | sensor °C | par module | Consigne de départ d'eau |
| `TmesEauN` | sensor °C | par module | Départ d'eau mesuré |
| `ModeHydN` | sensor texte | par module | Mode du module hydraulique |

Consignes `Heat` et `Off` sauvegardées en NVS (`Preferences` « frisquet », clé « modesp »).

### Résumé

| Retour | Méthode |
|---|---|
| `static constexpr uint8_t` | [`entityCount(uint8_t zones, uint8_t hydraulicModules)`](#entitycount) |
| | [`FrisquetVisioHA(FrisquetVisio& visio)`](#frisquetvisiohafrisquetvisio-visio) |
| `bool` | [`begin(uint8_t zoneCount = 1, uint8_t hydraulicModuleCount = 1, SemaphoreHandle_t visioMutex = nullptr, bool tramBytes = false)`](#begin-1) |
| `void` | [`loop()`](#loop-1) |
| `static void` | [`onRadioEvent(const FrisquetVisio::Event& event, void* self)`](#onradioevent) |

### Règles d'appel

1. Construire `HAMqtt` **avant** `visioHA.begin()`.
2. Appeler `visioHA.begin()` **avant** `mqtt.begin()`.
3. Appeler `mqtt.loop()` puis `visioHA.loop()` dans la **même** tâche.
4. Brancher `TramType` : `visio.onEvent(FrisquetVisioHA::onRadioEvent, &visioHA)`.
5. Si `visio.loop()` tourne dans une autre tâche, passer le mutex qui protège `visio`.

### Méthodes

#### entityCount

```cpp
static constexpr uint8_t entityCount(uint8_t zones, uint8_t hydraulicModules);
```

Nombre d'entités créées (`3 + 2 × zones + 3 × modules`), à passer à `HAMqtt`.

```cpp
HAMqtt mqtt(wifiClient, device, FrisquetVisioHA::entityCount(2, 1));
```

#### FrisquetVisioHA(FrisquetVisio& visio)

```cpp
explicit FrisquetVisioHA(FrisquetVisio& visio);
```

Associe la passerelle à la radio. Ne crée aucune entité. Non copiable.

#### begin

```cpp
bool begin(uint8_t zoneCount = 1, uint8_t hydraulicModuleCount = 1,
           SemaphoreHandle_t visioMutex = nullptr, bool tramBytes = false);
```

Crée les entités HA et relit les consignes NVS.

| Paramètre | |
|---|---|
| `zoneCount` | 1 à 3, normalement égal à `Config::zoneCount` |
| `hydraulicModuleCount` | 0 à 2 |
| `visioMutex` | Mutex partagé avec la tâche qui appelle `visio.loop()`, ou `nullptr` si même tâche |
| `tramBytes` | `true` : `TramType` contient aussi les octets en hexadécimal (debug) |

Retour `false` : deuxième instance, paramètre hors plage, ou file mémoire impossible à créer.

#### loop

```cpp
void loop();
```

Transmet les commandes HA à la radio, publie les états modifiés et la file `TramType`. Accède à `visio` uniquement sous `visioMutex`, et jamais pendant un appel MQTT : une reconnexion réseau ne bloque pas la radio.

#### onRadioEvent

```cpp
static void onRadioEvent(const FrisquetVisio::Event& event, void* self);
```

Callback à passer à `visio.onEvent()`, avec `self = &visioHA`. Appelé dans la tâche radio : copie l'événement dans une file de 8 sans attendre. File pleine : seul `TramType` perd des messages, la radio n'est pas affectée.

Pour garder votre propre callback, appelez-le depuis celui-ci :

```cpp
void onRadio(const FrisquetVisio::Event& e, void* ctx) {
    FrisquetVisioHA::onRadioEvent(e, ctx);
    // votre traitement
}
visio.onEvent(onRadio, &visioHA);
```
