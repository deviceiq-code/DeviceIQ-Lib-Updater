#include <DevIQ_Update.h>

using namespace DeviceIQ_Update;

Client updater;

const char* ROOT_CA = R"PEM(
-----BEGIN CERTIFICATE-----
... your root CA ...
-----END CERTIFICATE-----
)PEM";

void setup() {
    // WiFi.begin(...); wait for conection...
    Config cfg;
    cfg.model = "deviceiq-esp32";
    cfg.currentVersion = "1.2.0";
    cfg.manifestUrl = "https://your.cdn.com/firmware/manifest.json";
    cfg.rootCA_PEM = ROOT_CA;
    cfg.enableLanOta = true;
    cfg.lanHostname = "deviceiq-test";
    cfg.lanPassword = "password";
    cfg.checkIntervalMs = 6UL*60UL*60UL*1000UL;

    updater.onEvent([](Event e){ Serial.printf("[Update] Event=%d\n", (int)e); });
    updater.onProgress([](size_t w, size_t t){
        if (t > 0) Serial.printf("[Update] %u/%u (%u%%)\n", (unsigned)w, (unsigned)t, (unsigned)(100.0*w/(t)));
    });
    updater.onError([](Error er, const String& d){
        Serial.printf("[Update] ERROR=%d detail=%s\n", (int)er, d.c_str());
    });

    updater.begin(cfg);
    updater.CheckUpdateNow();
}

void loop() {
    updater.Control();
    // ... your app
    // For forced update::
    // updater.UpdateFromUrl("https://.../bin", "sha256hex");
}
