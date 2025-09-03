#include "DevIQ_Update.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#ifdef ARDUINO_ARCH_ESP32
    #include <ArduinoOTA.h>
#endif

using namespace DeviceIQ_Update;

static String sha256HexOf(const uint8_t* hash, size_t n) {
    const char* hx="0123456789abcdef"; String s; s.reserve(n*2);
    
    for (size_t i=0;i<n;i++) {
        s += hx[(hash[i]>>4)&0xF]; s += hx[hash[i]&0xF];
    }
    return s;
}

bool UpdateClient::begin(const UpdateConfig& cfg) {
    _cfg = cfg;
    _emit(Event::Init);

    #ifdef ARDUINO_ARCH_ESP32
    if (_cfg.enableLanOta) _setupLanOta();
    #endif
    
    return true;
}

void UpdateClient::Control() {
    #ifdef ARDUINO_ARCH_ESP32
        if (_cfg.enableLanOta) { ArduinoOTA.handle(); }
    #endif
    
    if (_cfg.checkIntervalMs && (millis() - _lastCheck > _cfg.checkIntervalMs)) {
        _lastCheck = millis();
        CheckUpdateNow();
    }
}

bool UpdateClient::CheckUpdateNow() {
    if (WiFi.status() != WL_CONNECTED) {
        _emitError(Error::Wifi, "No WiFi");
        return false;
    }

    _emit(Event::Checking);
    Manifest m;
    
    if (!_loadManifest(m)) return false;

    if (!IsNewer(m.version, _cfg.currentVersion)) {
        _emit(Event::NoUpdate);
        return false;
    }
    
    _emit(Event::NewVersion);
    return _downloadAndApply(m);
}

bool UpdateClient::UpdateFromURL(const String& url, const String& expectedSha) {
    Manifest m;
    m.url = url;
    m.sha256 = expectedSha;
    m.version = "0.0.0";
    
    return _downloadAndApply(m);
}

bool UpdateClient::_loadManifest(Manifest& out) {
    HTTPClient http;
    std::unique_ptr<WiFiClient> cli;
    
    if (_cfg.allowInsecure) {
        cli.reset(new WiFiClient());
    } else {
        auto s = new WiFiClientSecure();
        s->setCACert(_cfg.rootCA_PEM);
        cli.reset(s);
    }
    
    if (!http.begin(*cli, _cfg.manifestUrl)) {
        _emitError(Error::ManifestDownload, "begin()");
        return false;
    }
    
    http.setTimeout(_cfg.httpTimeoutMs);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        _emitError(Error::ManifestDownload, String("HTTP ") + code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();
    
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        _emitError(Error::ManifestParse, "JSON");
        return false;
    }

    out.model = doc["model"] | "";
    out.version = doc["version"] | "";
    out.minVersion = doc["min_version"] | "0.0.0";
    out.url = doc["url"] | "";
    out.sha256 = doc["sha256"] | "";
    
    if (!out.model.equalsIgnoreCase(_cfg.model)) {
        _emitError(Error::ModelMismatch, out.model);
        return false;
    }
    return true;
}

bool UpdateClient::_downloadAndApply(const Manifest& m) {
    HTTPClient http; std::unique_ptr<WiFiClient> cli;

    if (_cfg.allowInsecure) {
        cli.reset(new WiFiClient());
    } else {
        auto s = new WiFiClientSecure();
        s->setCACert(_cfg.rootCA_PEM);
        cli.reset(s);
    }
    
    if (!http.begin(*cli, m.url)) {
        _emitError(Error::HttpBegin, "begin()");
        return false;
    }
  
    http.setTimeout(_cfg.httpTimeoutMs);
    
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        _emitError(Error::HttpCode, String(code));
        http.end();
        return false;
    }
    
    int total = http.getSize();

    WiFiClient& stream = http.getStream();

    if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
        _emitError(Error::UpdateBegin, String(Update.getError())); http.end(); return false;
    }
    _emit(Event::Downloading);

    // SHA-256 on-the-fly
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_starts(&ctx);

    std::unique_ptr<uint8_t[]> buf(new uint8_t[_cfg.streamBufSize]);

    size_t written = 0;
    while (http.connected() && (total > 0 || total == -1)) {
        size_t avail = stream.available();
        if (avail) {
            int r = stream.readBytes(buf.get(), std::min((int)_cfg.streamBufSize, (int)avail));
            if (r <= 0) break;
            mbedtls_md_update(&ctx, buf.get(), r);
            if (Update.write(buf.get(), r) != (size_t)r) {
                mbedtls_md_free(&ctx);
                Update.abort();
                http.end();
                _emitError(Error::UpdateWrite, String(Update.getError()));
                return false;
            }

            written += r;
            if (_onProgress) _onProgress(written, total);
            if (total > 0) total -= r;
        }
        delay(1);
    }

    uint8_t hash[32]; mbedtls_md_finish(&ctx, hash); mbedtls_md_free(&ctx);
    if (m.sha256.length() == 64) {
        String got = sha256HexOf(hash, 32);
        if (!got.equalsIgnoreCase(m.sha256)) {
            Update.abort(); http.end(); _emitError(Error::ShaMismatch, got); return false;
        }
    }

    _emit(Event::Verifying);
    if (!Update.end()) {
        http.end();
        _emitError(Error::UpdateEnd, String(Update.getError()));
        return false;
    }
    if (!Update.isFinished()) {
        http.end();
        _emitError(Error::NotFinished, "Update not finished");
        return false;
    }

    http.end();
    _emit(Event::Applying);
    if (_cfg.autoReboot) {
        _emit(Event::Rebooting);
        delay(400);
        ESP.restart();
    }

    return true;
}

void UpdateClient::_setupLanOta() {
    #ifdef ARDUINO_ARCH_ESP32
        ArduinoOTA.setHostname(_cfg.lanHostname.c_str());
        if (_cfg.lanPassword.length()) ArduinoOTA.setPassword(_cfg.lanPassword.c_str());

        // Sinaliza que o serviço OTA em LAN está ativo
        ArduinoOTA.onStart([this]{ _emit(Event::Applying); });
        ArduinoOTA.onEnd([this]{
            _emit(Event::Applying);
            if (_cfg.autoReboot) { _emit(Event::Rebooting); delay(200); ESP.restart(); }
        });
        ArduinoOTA.onProgress([this](unsigned p, unsigned t) {
            if (_onProgress) _onProgress(p, t);
        });
        ArduinoOTA.onError([this](ota_error_t e){
            String d;
            switch (e) {
                case OTA_AUTH_ERROR: {
                    d = "AUTH";
                } break;
                case OTA_BEGIN_ERROR: {
                    d = "BEGIN";
                } break;
                case OTA_CONNECT_ERROR: {
                    d = "CONNECT";
                } break;
                case OTA_RECEIVE_ERROR: { 
                    d = "RECEIVE";
                } break;
                case OTA_END_ERROR: {
                    d = "END";
                } break;
                default: {
                    d = String("code ") + (int)e;
                } break;
            }
            _emitError(Error::LanOta, "ArduinoOTA "+d);
        });

        ArduinoOTA.begin();
        _emit(Event::LanReady);
    #endif
}