/*
 * target.h — la « carte » MeshCore de la T-Watch Ultra : radio (pilote
 * sx1262), alimentation, horloge et capteurs, vus par le code MeshCore
 * d'origine (companion_radio). Tout s'exécute dans la tâche « mesh ».
 */
#pragma once
#include <Arduino.h>
#include <Mesh.h>
#include <helpers/SensorManager.h>
#include <helpers/KeyValueStore.h>

class TWatchRadio : public mesh::Radio {
public:
    void begin() override;
    int recvRaw(uint8_t *bytes, int sz) override;
    uint32_t getEstAirtimeFor(int len_bytes) override;
    float packetScore(float snr, int packet_len) override;
    bool startSendRaw(const uint8_t *bytes, int len) override;
    bool isSendComplete() override;
    void onSendFinished() override;
    void loop() override;
    int getNoiseFloor() const override;
    bool isInRecvMode() const override;
    bool isReceiving() override;
    float getLastRSSI() const override { return last_rssi; }
    float getLastSNR() const override { return last_snr; }

    void setParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr);
    void setTxPower(int8_t dbm);
    bool setRxBoostedGainMode(bool on) { boosted = on; return true; }   /* toujours actif */
    bool getRxBoostedGainMode() const { return boosted; }
    uint32_t getPacketsRecv() const;
    uint32_t getPacketsSent() const;
    uint32_t getPacketsRecvErrors() const;

    /* appelé par la tâche « lora » */
    void onRx(const uint8_t *buf, size_t len, int16_t rssi, int8_t snr_q4);
    void start();
    void stop();
private:
    void apply();
    float last_rssi = 0, last_snr = 0;
    bool boosted = true;
    bool dirty = false, sending = false;
    uint32_t tx_mark = 0;
    float freq = 869.618f, bw = 62.5f;
    uint8_t sf = 8, cr = 8;
    int8_t power = 22;
};

class TWatchBoard : public mesh::MainBoard {
public:
    uint16_t getBattMilliVolts() override;
    float getMCUTemperature() override;
    const char *getManufacturerName() const override { return "LilyGo T-Watch Ultra"; }
    void reboot() override;
    uint8_t getStartupReason() const override { return 0; }
    void attachDynamicPrefs(KeyValueStore *prefs) { }
};

/* RTC de la montre (PCF85063, mise à l'heure par l'iPhone) */
class TWatchClock : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override;
    void setCurrentTime(uint32_t t) override;
};

/* GNSS de la montre (gnss.c) : position des annonces quand l'app le permet
 * (« partager la position »), télémétrie de position, réglage « gps » de
 * l'app MeshCore (variables personnalisées) = interrupteur de l'app GPS. */
class TWatchSensors : public SensorManager {
public:
    bool begin() override { return true; }
    bool querySensors(uint8_t requester_permissions, CayenneLPP &telemetry) override;
    void loop() override;
    int getNumSettings() const override { return 1; }
    const char *getSettingName(int i) const override { return i == 0 ? "gps" : NULL; }
    const char *getSettingValue(int i) const override;
    bool setSettingValue(const char *name, const char *value) override;
    bool isGPSDetected() const override { return true; }
    /* position GNSS reçue depuis la dernière sauvegarde des préférences */
    bool takeMoved() { bool m = moved; moved = false; return m; }
private:
    int64_t next_us = 0;
    bool moved = false;
    double saved_lat = 0, saved_lon = 0;
};

extern TWatchRadio radio_driver;
extern TWatchBoard board;
extern TWatchClock rtc_clock;
extern TWatchSensors sensors;

mesh::LocalIdentity radio_new_identity();
