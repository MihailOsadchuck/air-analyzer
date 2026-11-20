#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ESP32Encoder.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <time.h>
#include "wifi_secrets.h"

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST 4
#define TFT_BL 32

#define BLUE_GRAY   0x3209
#define WHITE       0xFFFF
#define GREEN       0x07E0
#define LIME        0xFFE0
#define ORANGE      0xFD20
#define RED         0xF800
#define GRAY        0x8410
#define CYAN        0x07FF

constexpr uint16_t CO2_MIN_PPM = 400;
constexpr uint16_t CO2_GOOD_MAX_PPM = 800;
constexpr uint16_t CO2_MODERATE_MAX_PPM = 1000;
constexpr uint16_t CO2_HIGH_MAX_PPM = 1500;
constexpr uint16_t CO2_MAX_PPM = 2000;
constexpr uint32_t DISPLAY_UPDATE_INTERVAL_MS = 10000;
constexpr uint32_t HISTORY_VERSION = 3;
constexpr size_t HISTORY_CAPACITY = 25;
constexpr time_t HISTORY_WINDOW_SECONDS = 24 * 60 * 60;
constexpr int ENCODER_COUNTS_PER_STEP = 1;
constexpr uint32_t SENSOR_POLL_INTERVAL_MS = 250;
constexpr uint32_t SYSTEM_PAGE_UPDATE_INTERVAL_MS = 1000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
constexpr uint32_t NTP_RETRY_INTERVAL_MS = 30000;

struct HourlySample {
    int64_t hourStartEpoch;
    uint16_t minimumCo2;
    int64_t minimumEpoch;
    uint16_t maximumCo2;
    int64_t maximumEpoch;
    bool minimumOverLimit;
    bool maximumOverLimit;
};

struct HourlySampleV2 {
    int64_t hourStartEpoch;
    uint16_t minimumCo2;
    int64_t minimumEpoch;
    uint16_t maximumCo2;
    int64_t maximumEpoch;
};

struct HistoryDataV2 {
    uint32_t version;
    uint8_t count;
    HourlySampleV2 samples[HISTORY_CAPACITY];
};

struct ActiveHour {
    int64_t hourStartEpoch;
    uint16_t minimumCo2;
    int64_t minimumEpoch;
    uint16_t maximumCo2;
    int64_t maximumEpoch;
    bool valid;
    bool minimumOverLimit;
    bool maximumOverLimit;
};

struct HistoryData {
    uint32_t version;
    uint8_t count;
    HourlySample samples[HISTORY_CAPACITY];
};

SensirionI2cScd4x scd4x;
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
ESP32Encoder encoder;
Preferences preferences;
HistoryData history = {};
ActiveHour activeHour = {};
uint16_t backgroundColor = BLUE_GRAY;
uint32_t lastDisplayUpdateMs = 0;
uint32_t lastClockUpdateMs = 0;
uint32_t lastSystemPageUpdateMs = 0;
uint32_t lastSensorPollMs = 0;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastNtpAttemptMs = 0;
uint32_t cpuWindowStartMs = 0;
uint8_t appLoadPercent = 0;
uint64_t cpuWindowBusyMicros = 0;
bool systemPageNeedsFullUpdate = true;
bool historyPageNeedsFullUpdate = true;
bool ntpAttempted = false;
int64_t lastEncoderCount = 0;
int encoderCountRemainder = 0;
uint8_t currentPage = 0;
bool hasSensorReading = false;
uint16_t latestCo2 = 0;
float latestTemperature = 0;
float latestHumidity = 0;
int gaugeMarkerX;
int gaugeMarkerY;
float gaugeMarkerAngle;
bool gaugeMarkerVisible = false;

struct HistoryDisplayCache {
    bool valid;
    uint16_t minimumCo2;
    int64_t minimumEpoch;
    bool minimumOverLimit;
    uint16_t maximumCo2;
    int64_t maximumEpoch;
    bool maximumOverLimit;
};

HistoryDisplayCache historyDisplayCache = {};
int8_t historyPageDisplayState = -1;

void showCurrentPage();

uint16_t limitCo2(uint16_t co2) {
    return co2 > CO2_MAX_PPM ? CO2_MAX_PPM : co2;
}

const bool digits[10][7] = {
    {1,1,1,1,1,1,0},
    {0,1,1,0,0,0,0},
    {1,1,0,1,1,0,1},
    {1,1,1,1,0,0,1},
    {0,1,1,0,0,1,1},
    {1,0,1,1,0,1,1},
    {1,0,1,1,1,1,1},
    {1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1},
    {1,1,1,1,0,1,1}
};

void drawDigit(int x, int y, int value, uint16_t color) {
    const int s = 4;

    // A
    tft.fillRect(x+4, y, 14, s, digits[value][0] ? color : backgroundColor);

    // B
    tft.fillRect(x+18, y+4, s, 15, digits[value][1] ? color : backgroundColor);

    // C
    tft.fillRect(x+18, y+22, s, 15, digits[value][2] ? color : backgroundColor);

    // D
    tft.fillRect(x+4, y+38, 14, s, digits[value][3] ? color : backgroundColor);

    // E
    tft.fillRect(x, y+22, s, 15, digits[value][4] ? color : backgroundColor);

    // F
    tft.fillRect(x, y+4, s, 15, digits[value][5] ? color : backgroundColor);

    // G
    tft.fillRect(x+4, y+19, 14, s, digits[value][6] ? color : backgroundColor);
}

// Draw thick colored arc
void drawArcSegment(
    int cx, int cy, int radius,
    float startAngle, float endAngle,
    uint16_t color
) {
    for (float a = startAngle; a <= endAngle; a += 2) {
        float rad = a * PI / 180.0;

        int x = cx + cos(rad) * radius;
        int y = cy + sin(rad) * radius;

        tft.fillCircle(x, y, 3, color);
    }
}

float co2ToAngle(uint16_t co2) {
    int boundedCo2 = constrain((int)co2, (int)CO2_MIN_PPM, (int)CO2_MAX_PPM);
    return 135.0 + (boundedCo2 - (int)CO2_MIN_PPM) * 270.0 /
        (CO2_MAX_PPM - CO2_MIN_PPM);
}

void drawGaugeArcRange(float rangeStart, float rangeEnd) {
    const int cx = 106;
    const int cy = 85;
    const int radius = 62;
    const float starts[] = {
        co2ToAngle(CO2_MIN_PPM),
        co2ToAngle(CO2_GOOD_MAX_PPM) + 2,
        co2ToAngle(CO2_MODERATE_MAX_PPM) + 2,
        co2ToAngle(CO2_HIGH_MAX_PPM) + 2
    };
    const float ends[] = {
        co2ToAngle(CO2_GOOD_MAX_PPM) - 2,
        co2ToAngle(CO2_MODERATE_MAX_PPM) - 2,
        co2ToAngle(CO2_HIGH_MAX_PPM) - 2,
        co2ToAngle(CO2_MAX_PPM)
    };
    const uint16_t colors[] = {GREEN, LIME, ORANGE, RED};

    for (int i = 0; i < 4; i++) {
        float start = rangeStart > starts[i] ? rangeStart : starts[i];
        float end = rangeEnd < ends[i] ? rangeEnd : ends[i];
        if (start <= end) {
            drawArcSegment(cx, cy, radius, start, end, colors[i]);
        }
    }
}

void drawGauge() {
    drawGaugeArcRange(co2ToAngle(CO2_MIN_PPM), co2ToAngle(CO2_MAX_PPM));
}

void drawGaugeMarker(uint16_t co2) {
    float angleDegrees = co2ToAngle(co2);
    float angle = angleDegrees * PI / 180.0;
    int nextX = 106 + cos(angle) * 62;
    int nextY = 85 + sin(angle) * 62;

    if (gaugeMarkerVisible && nextX == gaugeMarkerX && nextY == gaugeMarkerY) {
        return;
    }

    if (gaugeMarkerVisible) {
        tft.fillCircle(gaugeMarkerX, gaugeMarkerY, 7, backgroundColor);
        drawGaugeArcRange(gaugeMarkerAngle - 8, gaugeMarkerAngle + 8);
    }

    gaugeMarkerX = nextX;
    gaugeMarkerY = nextY;
    gaugeMarkerAngle = angleDegrees;

    tft.fillCircle(gaugeMarkerX, gaugeMarkerY, 6, WHITE);
    tft.fillCircle(gaugeMarkerX, gaugeMarkerY, 2, backgroundColor);
    gaugeMarkerVisible = true;
}

void drawInterface() {
    tft.fillScreen(backgroundColor);

   tft.drawRoundRect(5, 5, 310, 160, 9, GREEN);
    tft.drawFastVLine(178, 17, 136, GRAY);

    // CO2 title
    tft.setTextColor(WHITE);
    tft.setTextSize(2);
    tft.setCursor(93, 35);  // CO2
    tft.print("CO2");

    // Gauge
    drawGauge();
    tft.setTextColor(GRAY);
    tft.setTextSize(1);
    tft.setCursor(53, 140);
    tft.print(CO2_MIN_PPM);
    tft.setCursor(125, 140);
    tft.print(CO2_MAX_PPM);

    // PPM
    tft.setTextSize(1);
    tft.setCursor(99, 121); // PPM
    tft.print("PPM");

    // Temperature section
    tft.setTextSize(1);
    tft.setTextColor(GRAY);
    tft.setCursor(185, 34);
    tft.print("TEMP");

    tft.drawRoundRect(192, 50, 7, 22, 3, WHITE);
    tft.drawCircle(195, 70, 6, WHITE);
    tft.drawFastVLine(195, 57, 14, WHITE);
    tft.fillCircle(195, 70, 3, WHITE);

    // Humidity section
    tft.setCursor(185, 100);
    tft.print("HUMIDITY");

    tft.fillCircle(195, 131, 7, WHITE);
    tft.fillTriangle(195, 114, 188, 131, 202, 131, WHITE);
    tft.fillCircle(195, 131, 4, backgroundColor);
    tft.fillTriangle(195, 119, 191, 131, 199, 131, backgroundColor);
}

void updateDisplay(
    uint16_t co2,
    float temperature,
    float humidity,
    bool force = false
) {
    drawGaugeMarker(co2);

    // CO2 value
    int value = limitCo2(co2);
    bool overLimit = co2 > CO2_MAX_PPM;

    char buffer[5];
    snprintf(buffer, sizeof(buffer), "%04d", value);

    static char previousCo2Digits[5] = "";
    static bool previousCo2OverLimit = false;
    bool co2LimitStateChanged = overLimit != previousCo2OverLimit;

    for (int i = 0; i < 4; i++) {
        if (force || co2LimitStateChanged || buffer[i] != previousCo2Digits[i]) {
            drawDigit(52 + i * 25, 66, buffer[i] - '0', overLimit ? RED : WHITE);
            previousCo2Digits[i] = buffer[i];
        }
    }
    if (force || co2LimitStateChanged) {
        previousCo2OverLimit = overLimit;
    }

    // Temperature
    static int previousTemperatureTenths = -32768;
    int temperatureTenths = (int)round(temperature * 10.0f);
    if (force || temperatureTenths != previousTemperatureTenths) {
        char temperatureText[10];
        snprintf(temperatureText, sizeof(temperatureText), "%.1fC", temperature);
        tft.setTextColor(CYAN, backgroundColor);
        tft.setTextSize(2);
        tft.setCursor(209, 57);
        tft.print(temperatureText);
        tft.print("  ");
        previousTemperatureTenths = temperatureTenths;
    }

    // Humidity
    static int previousHumidityTenths = -32768;
    int humidityTenths = (int)round(humidity * 10.0f);
    if (force || humidityTenths != previousHumidityTenths) {
        char humidityText[10];
        snprintf(humidityText, sizeof(humidityText), "%.1f%%", humidity);
        tft.setTextColor(WHITE, backgroundColor);
        tft.setTextSize(2);
        tft.setCursor(209, 123);
        tft.print(humidityText);
        tft.print("  ");
        previousHumidityTenths = humidityTenths;
    }

}

bool isTimeValid() {
    return time(nullptr) >= 1704067200;
}

void startWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    Serial.println("Wi-Fi: connecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiAttemptMs = millis();
}

void maintainWifiAndTime() {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
            Serial.println("Wi-Fi: retrying connection...");
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            lastWifiAttemptMs = now;
        }
        return;
    }

    if (!isTimeValid() &&
        (!ntpAttempted || now - lastNtpAttemptMs >= NTP_RETRY_INTERVAL_MS)) {
        Serial.println("NTP: requesting Kyiv time synchronization...");
        configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4",
            "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        lastNtpAttemptMs = now;
        ntpAttempted = true;
    }
}

void loadHistory() {
    history = {};
    history.version = HISTORY_VERSION;
    if (!preferences.begin("air-history", true)) {
        return;
    }

    size_t storedLength = preferences.getBytesLength("hourly");
    if (storedLength == sizeof(history)) {
        preferences.getBytes("hourly", &history, sizeof(history));
        if (history.version != HISTORY_VERSION || history.count > HISTORY_CAPACITY) {
            history = {};
            history.version = HISTORY_VERSION;
        } else {
            for (uint8_t i = 0; i < history.count; i++) {
                history.samples[i].minimumCo2 = limitCo2(history.samples[i].minimumCo2);
                history.samples[i].maximumCo2 = limitCo2(history.samples[i].maximumCo2);
            }
        }
    } else if (storedLength == sizeof(HistoryDataV2)) {
        HistoryDataV2 previousHistory = {};
        preferences.getBytes("hourly", &previousHistory, sizeof(previousHistory));
        if (previousHistory.version == 2 && previousHistory.count <= HISTORY_CAPACITY) {
            history.count = previousHistory.count;
            for (uint8_t i = 0; i < history.count; i++) {
                const HourlySampleV2& previousSample = previousHistory.samples[i];
                history.samples[i] = {
                    previousSample.hourStartEpoch,
                    limitCo2(previousSample.minimumCo2),
                    previousSample.minimumEpoch,
                    limitCo2(previousSample.maximumCo2),
                    previousSample.maximumEpoch,
                    previousSample.minimumCo2 > CO2_MAX_PPM,
                    previousSample.maximumCo2 > CO2_MAX_PPM
                };
            }
        }
    }
    preferences.end();
}

void pruneHistory(time_t now) {
    int64_t oldestEpoch = (int64_t)now - HISTORY_WINDOW_SECONDS;
    uint8_t retained = 0;
    for (uint8_t i = 0; i < history.count; i++) {
        const HourlySample& sample = history.samples[i];
        bool minimumInRange = sample.minimumEpoch >= oldestEpoch && sample.minimumEpoch <= now;
        bool maximumInRange = sample.maximumEpoch >= oldestEpoch && sample.maximumEpoch <= now;
        if (minimumInRange || maximumInRange) {
            history.samples[retained++] = history.samples[i];
        }
    }
    history.count = retained;
}

void saveHistory() {
    history.version = HISTORY_VERSION;
    for (uint8_t i = 0; i < history.count; i++) {
        history.samples[i].minimumCo2 = limitCo2(history.samples[i].minimumCo2);
        history.samples[i].maximumCo2 = limitCo2(history.samples[i].maximumCo2);
    }
    if (preferences.begin("air-history", false)) {
        preferences.putBytes("hourly", &history, sizeof(history));
        preferences.end();
    }
}

void commitActiveHour(time_t now) {
    if (!activeHour.valid) {
        return;
    }

    pruneHistory(now);
    if (history.count == HISTORY_CAPACITY) {
        for (uint8_t i = 1; i < history.count; i++) {
            history.samples[i - 1] = history.samples[i];
        }
        history.count--;
    }

    history.samples[history.count++] = {
        activeHour.hourStartEpoch,
        activeHour.minimumCo2,
        activeHour.minimumEpoch,
        activeHour.maximumCo2,
        activeHour.maximumEpoch,
        activeHour.minimumOverLimit,
        activeHour.maximumOverLimit
    };
    saveHistory();
}

void recordHourlyCo2(uint16_t co2, time_t now) {
    if (!isTimeValid()) {
        return;
    }
    bool overLimit = co2 > CO2_MAX_PPM;
    co2 = limitCo2(co2);

    int64_t hourStartEpoch = ((int64_t)now / 3600) * 3600;
    if (activeHour.valid && activeHour.hourStartEpoch != hourStartEpoch) {
        if (hourStartEpoch > activeHour.hourStartEpoch) {
            commitActiveHour(now);
        }
        activeHour.valid = false;
    }

    if (!activeHour.valid) {
        activeHour.hourStartEpoch = hourStartEpoch;
        activeHour.minimumCo2 = co2;
        activeHour.minimumEpoch = now;
        activeHour.maximumCo2 = co2;
        activeHour.maximumEpoch = now;
        activeHour.minimumOverLimit = overLimit;
        activeHour.maximumOverLimit = overLimit;
        activeHour.valid = true;
        return;
    }

    if (co2 < activeHour.minimumCo2 ||
        (co2 == activeHour.minimumCo2 && activeHour.minimumOverLimit && !overLimit)) {
        activeHour.minimumCo2 = co2;
        activeHour.minimumEpoch = now;
        activeHour.minimumOverLimit = overLimit;
    }
    if (co2 > activeHour.maximumCo2 ||
        (co2 == activeHour.maximumCo2 && overLimit && !activeHour.maximumOverLimit)) {
        activeHour.maximumCo2 = co2;
        activeHour.maximumEpoch = now;
        activeHour.maximumOverLimit = overLimit;
    }
}

void printSampleTime(int x, int y, int64_t epoch) {
    time_t timestamp = (time_t)epoch;
    struct tm localTime;
    localtime_r(&timestamp, &localTime);
    char timeText[24];
    strftime(timeText, sizeof(timeText), "%d.%m %H:%M", &localTime);
    tft.setTextColor(GRAY, backgroundColor);
    tft.setTextSize(2);
    tft.setCursor(x, y);
    tft.print(timeText);
}

void drawHistoryStat(int valueY, int timeY, uint16_t co2,
    bool overLimit, int64_t timestamp, uint16_t color) {
    tft.fillRect(40, valueY, 105, 26, backgroundColor);
    tft.setTextColor(overLimit ? RED : color, backgroundColor);
    tft.setTextSize(3);
    tft.setCursor(42, valueY);
    if (overLimit) {
        tft.print(">");
    }
    tft.print(co2);
    tft.fillRect(140, timeY - 2, 172, 22, backgroundColor);
    printSampleTime(144, timeY, timestamp);
}

void drawHistoryPage() {
    if (historyPageNeedsFullUpdate) {
        tft.fillScreen(backgroundColor);
        tft.drawRoundRect(5, 5, 310, 160, 9, GREEN);
        tft.setTextColor(WHITE, backgroundColor);
        tft.setTextSize(2);
        tft.setCursor(91, 23);
        tft.print("CO2 / 24H");

        tft.setTextColor(GRAY, backgroundColor);
        tft.setTextSize(1);
        tft.setCursor(42, 43);
        tft.print("MINIMUM");
        tft.setCursor(42, 98);
        tft.print("MAXIMUM");
        tft.setCursor(42, 87);
        tft.print("ppm");
        tft.setCursor(42, 142);
        tft.print("ppm");

        historyPageNeedsFullUpdate = false;
        historyDisplayCache.valid = false;
        historyPageDisplayState = -1;
    }

    bool currentTimeValid = isTimeValid();
    int64_t referenceEpoch = currentTimeValid ? (int64_t)time(nullptr) : 0;
    if (!currentTimeValid) {
        for (uint8_t i = 0; i < history.count; i++) {
            const HourlySample& sample = history.samples[i];
            if (sample.minimumEpoch > referenceEpoch) {
                referenceEpoch = sample.minimumEpoch;
            }
            if (sample.maximumEpoch > referenceEpoch) {
                referenceEpoch = sample.maximumEpoch;
            }
        }
        if (activeHour.valid) {
            if (activeHour.minimumEpoch > referenceEpoch) {
                referenceEpoch = activeHour.minimumEpoch;
            }
            if (activeHour.maximumEpoch > referenceEpoch) {
                referenceEpoch = activeHour.maximumEpoch;
            }
        }
    }
    uint16_t minimumCo2 = 0;
    uint16_t maximumCo2 = 0;
    int64_t minimumEpoch = 0;
    int64_t maximumEpoch = 0;
    bool minimumOverLimit = false;
    bool maximumOverLimit = false;
    bool foundSample = false;
    int64_t oldestEpoch = referenceEpoch - HISTORY_WINDOW_SECONDS;
    auto considerExtreme = [&](uint16_t co2, int64_t epoch, bool overLimit) {
        if (epoch < oldestEpoch || epoch > referenceEpoch) {
            return;
        }
        if (!foundSample || co2 < minimumCo2 ||
            (co2 == minimumCo2 && minimumOverLimit && !overLimit)) {
            minimumCo2 = co2;
            minimumEpoch = epoch;
            minimumOverLimit = overLimit;
        }
        if (!foundSample || co2 > maximumCo2 ||
            (co2 == maximumCo2 && overLimit && !maximumOverLimit)) {
            maximumCo2 = co2;
            maximumEpoch = epoch;
            maximumOverLimit = overLimit;
        }
        foundSample = true;
    };

    for (uint8_t i = 0; i < history.count; i++) {
        const HourlySample& sample = history.samples[i];
        considerExtreme(sample.minimumCo2, sample.minimumEpoch, sample.minimumOverLimit);
        considerExtreme(sample.maximumCo2, sample.maximumEpoch, sample.maximumOverLimit);
    }
    if (activeHour.valid) {
        considerExtreme(activeHour.minimumCo2, activeHour.minimumEpoch,
            activeHour.minimumOverLimit);
        considerExtreme(activeHour.maximumCo2, activeHour.maximumEpoch,
            activeHour.maximumOverLimit);
    }

    if (!foundSample) {
        if (historyPageDisplayState != 1) {
            if (historyPageDisplayState == 2) {
                tft.fillRect(40, 61, 105, 26, backgroundColor);
                tft.fillRect(40, 116, 105, 26, backgroundColor);
                tft.fillRect(140, 66, 172, 22, backgroundColor);
                tft.fillRect(140, 121, 172, 22, backgroundColor);
            }
            tft.fillRect(75, 78, 180, 18, backgroundColor);
            tft.setTextColor(GRAY, backgroundColor);
            tft.setTextSize(1);
            tft.setCursor(92, 82);
            tft.print("NO SAMPLES YET");
            historyPageDisplayState = 1;
        }
        return;
    }

    bool redrawMinimum = historyPageDisplayState != 2 || !historyDisplayCache.valid ||
        minimumCo2 != historyDisplayCache.minimumCo2 ||
        minimumOverLimit != historyDisplayCache.minimumOverLimit ||
        minimumEpoch != historyDisplayCache.minimumEpoch;
    bool redrawMaximum = historyPageDisplayState != 2 || !historyDisplayCache.valid ||
        maximumCo2 != historyDisplayCache.maximumCo2 ||
        maximumOverLimit != historyDisplayCache.maximumOverLimit ||
        maximumEpoch != historyDisplayCache.maximumEpoch;

    if (historyPageDisplayState != 2) {
        tft.fillRect(75, 78, 180, 18, backgroundColor);
    }
    if (redrawMinimum) {
        drawHistoryStat(61, 68, minimumCo2, minimumOverLimit, minimumEpoch, CYAN);
    }
    if (redrawMaximum) {
        drawHistoryStat(116, 123, maximumCo2, maximumOverLimit, maximumEpoch, RED);
    }

    historyDisplayCache.minimumCo2 = minimumCo2;
    historyDisplayCache.minimumEpoch = minimumEpoch;
    historyDisplayCache.minimumOverLimit = minimumOverLimit;
    historyDisplayCache.maximumCo2 = maximumCo2;
    historyDisplayCache.maximumEpoch = maximumEpoch;
    historyDisplayCache.maximumOverLimit = maximumOverLimit;
    historyDisplayCache.valid = true;
    historyPageDisplayState = 2;
}

void drawClockPage() {
    tft.fillScreen(backgroundColor);
    tft.drawRoundRect(5, 5, 310, 160, 9, GREEN);
}

void updateClockPage() {
    char clockText[9];
    if (!isTimeValid()) {
        snprintf(clockText, sizeof(clockText), "--:--:--");
    } else {
        time_t now = time(nullptr);
        struct tm localTime;
        localtime_r(&now, &localTime);
        strftime(clockText, sizeof(clockText), "%H:%M:%S", &localTime);
    }

    tft.setTextColor(GREEN, backgroundColor);
    tft.setTextSize(5);
    tft.setCursor(40, 65);
    tft.print(clockText);
}

void drawSystemPage() {
    tft.fillScreen(backgroundColor);
    systemPageNeedsFullUpdate = true;
    tft.drawRoundRect(5, 5, 310, 160, 9, GREEN);
    tft.setTextColor(WHITE, backgroundColor);
    tft.setTextSize(2);
    tft.setCursor(127, 25);
    tft.print("SYSTEM");

    tft.setTextSize(1);
    tft.setTextColor(GRAY, backgroundColor);
    tft.setCursor(57, 51);
    tft.print("APP LOAD");
    tft.drawRoundRect(57, 65, 206, 14, 3, GRAY);

    tft.setCursor(57, 91);
    tft.print("WI-FI");
    tft.setCursor(57, 105);
    tft.print("SIGNAL");
    tft.drawRoundRect(57, 119, 206, 14, 3, GRAY);
    tft.setCursor(57, 140);
    tft.print("IP");
}

void updateSystemPage() {
    const int barX = 60;
    const int barMaxWidth = 200;
    int loadWidth = barMaxWidth * appLoadPercent / 100;
    uint16_t loadColor = appLoadPercent >= 80 ? RED : (appLoadPercent >= 50 ? ORANGE : GREEN);
    static int previousLoadPercent = -1;
    static int previousLoadWidth = -1;
    static uint16_t previousLoadColor = 0;

    if (systemPageNeedsFullUpdate || appLoadPercent != previousLoadPercent) {
        char loadText[8];
        snprintf(loadText, sizeof(loadText), "%3u%%", appLoadPercent);
        tft.fillRect(240, 48, 34, 13, backgroundColor);
        tft.setTextColor(CYAN, backgroundColor);
        tft.setTextSize(1);
        tft.setCursor(248, 51);
        tft.print(loadText);
    }
    if (systemPageNeedsFullUpdate || loadColor != previousLoadColor) {
        tft.fillRect(barX, 68, barMaxWidth, 8, backgroundColor);
        if (loadWidth > 0) {
            tft.fillRoundRect(barX, 68, loadWidth, 8, 3, loadColor);
        }
    } else if (loadWidth > previousLoadWidth) {
        tft.fillRect(barX + previousLoadWidth, 68, loadWidth - previousLoadWidth, 8, loadColor);
    } else if (loadWidth < previousLoadWidth) {
        tft.fillRect(barX + loadWidth, 68, previousLoadWidth - loadWidth, 8, backgroundColor);
    }
    previousLoadPercent = appLoadPercent;
    previousLoadWidth = loadWidth;
    previousLoadColor = loadColor;

    bool connected = WiFi.status() == WL_CONNECTED;
    static int previousConnectionState = -1;
    if (systemPageNeedsFullUpdate || connected != (previousConnectionState == 1)) {
        tft.fillRect(105, 88, 112, 13, backgroundColor);
        tft.setTextColor(connected ? GREEN : RED, backgroundColor);
        tft.setTextSize(1);
        tft.setCursor(110, 91);
        tft.print(connected ? "CONNECTED" : "OFFLINE");
        previousConnectionState = connected ? 1 : 0;
    }

    int signalPercent = 0;
    int rssi = -127;
    if (connected) {
        rssi = WiFi.RSSI();
        signalPercent = constrain(map(rssi, -100, -50, 0, 100), 0, 100);
    }
    static int previousRssi = -1000;
    if (systemPageNeedsFullUpdate || rssi != previousRssi) {
        char signalText[16];
        if (connected) {
            snprintf(signalText, sizeof(signalText), "%d dBm", rssi);
        } else {
            snprintf(signalText, sizeof(signalText), "-- dBm");
        }
        tft.fillRect(215, 102, 58, 13, backgroundColor);
        tft.setTextColor(WHITE, backgroundColor);
        tft.setTextSize(1);
        tft.setCursor(220, 105);
        tft.print(signalText);
        previousRssi = rssi;
    }

    int signalWidth = barMaxWidth * signalPercent / 100;
    uint16_t signalColor = signalPercent >= 60 ? GREEN : (signalPercent >= 30 ? ORANGE : RED);
    static int previousSignalWidth = -1;
    static uint16_t previousSignalColor = 0;
    if (systemPageNeedsFullUpdate || signalColor != previousSignalColor) {
        tft.fillRect(barX, 122, barMaxWidth, 8, backgroundColor);
        if (signalWidth > 0) {
            tft.fillRoundRect(barX, 122, signalWidth, 8, 3, signalColor);
        }
    } else if (signalWidth > previousSignalWidth) {
        tft.fillRect(barX + previousSignalWidth, 122,
            signalWidth - previousSignalWidth, 8, signalColor);
    } else if (signalWidth < previousSignalWidth) {
        tft.fillRect(barX + signalWidth, 122,
            previousSignalWidth - signalWidth, 8, backgroundColor);
    }
    previousSignalWidth = signalWidth;
    previousSignalColor = signalColor;

    static char previousIpText[16] = "";
    char ipText[16];
    if (connected) {
        snprintf(ipText, sizeof(ipText), "%s", WiFi.localIP().toString().c_str());
    } else {
        snprintf(ipText, sizeof(ipText), "NOT CONNECTED");
    }
    if (systemPageNeedsFullUpdate || strcmp(ipText, previousIpText) != 0) {
        tft.fillRect(73, 137, 200, 13, backgroundColor);
        tft.setTextColor(connected ? GRAY : RED, backgroundColor);
        tft.setTextSize(1);
        tft.setCursor(73, 140);
        tft.print(ipText);
        snprintf(previousIpText, sizeof(previousIpText), "%s", ipText);
    }
    systemPageNeedsFullUpdate = false;
}

void showCurrentPage() {
    gaugeMarkerVisible = false;
    if (currentPage != 1) {
        historyPageNeedsFullUpdate = true;
    }
    if (currentPage == 0) {
        drawInterface();
        if (hasSensorReading) {
            updateDisplay(latestCo2, latestTemperature, latestHumidity, true);
        }
        lastDisplayUpdateMs = millis();
    } else if (currentPage == 1) {
        drawHistoryPage();
    } else if (currentPage == 2) {
        drawClockPage();
        updateClockPage();
        lastClockUpdateMs = millis();
    } else {
        drawSystemPage();
        updateSystemPage();
        lastSystemPageUpdateMs = millis();
    }
}

void updateEncoderPage() {
    int64_t encoderCount = encoder.getCount();
    encoderCountRemainder += (int)(encoderCount - lastEncoderCount);
    lastEncoderCount = encoderCount;

    bool pageChanged = false;
    while (encoderCountRemainder >= ENCODER_COUNTS_PER_STEP) {
        currentPage = (currentPage + 1) % 4;
        encoderCountRemainder -= ENCODER_COUNTS_PER_STEP;
        pageChanged = true;
    }
    while (encoderCountRemainder <= -ENCODER_COUNTS_PER_STEP) {
        currentPage = (currentPage + 3) % 4;
        encoderCountRemainder += ENCODER_COUNTS_PER_STEP;
        pageChanged = true;
    }
    if (pageChanged) {
        showCurrentPage();
    }
}

void drawStartupAnimation() {
    const int centerX = 160;
    const int centerY = 82;
    const int radius = 38;

    tft.fillScreen(backgroundColor);
    tft.drawRoundRect(12, 12, 296, 146, 10, GRAY);
    tft.setTextColor(WHITE);
    tft.setTextSize(2);
    tft.setCursor(88, 20);
    tft.print("AIR ANALYZER");
    tft.setTextColor(GRAY);
    tft.setTextSize(1);
    tft.setCursor(130, 140);
    tft.print("STARTING...");

    drawArcSegment(centerX, centerY, radius, 135, 405, GRAY);
    for (float angle = 135; angle <= 405; angle += 4) {
        if (angle > 135) {
            drawArcSegment(centerX, centerY, radius, angle - 12, angle + 4, GRAY);
        }

        float radians = angle * PI / 180.0;
        int markerX = centerX + cos(radians) * radius;
        int markerY = centerY + sin(radians) * radius;
        tft.fillCircle(markerX, markerY, 3, CYAN);
        tft.fillCircle(markerX, markerY, 1, WHITE);
        delay(10);
    }
    delay(120);
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Wait for Serial to initialize

    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    encoder.attachHalfQuad(27, 26);
    encoder.setFilter(1023);
    encoder.clearCount();
    lastEncoderCount = encoder.getCount();

    loadHistory();
    startWifi();

  Wire.begin(21, 22); // Initialize I2C with SDA on GPIO 21 and SCL on GPIO 22
  scd4x.begin(Wire, 0x62); // Initialize the SCD4x sensor with I2C address 0x62
  uint16_t error;
  error = scd4x.stopPeriodicMeasurement();
  if (error) {
    Serial.print("Error stopping periodic measurement (code ");
    Serial.print(error);
    Serial.println(").");
  } else {
    Serial.println("Periodic measurement stopped successfully.");
  }
  error = scd4x.startPeriodicMeasurement();
  if (error) {
    Serial.print("Error starting periodic measurement (code ");
    Serial.print(error);
    Serial.println(").");
  } else {
    Serial.println("Periodic measurement started successfully.");
  }
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH); // Turn on the backlight
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS); // Initialize SPI
  tft.init(170, 320); // Initialize the display with width and height
  tft.setRotation(-1); // Set the display rotation

    drawStartupAnimation();
  drawInterface(); // Draw the initial interface
    if (isTimeValid()) {
        pruneHistory(time(nullptr));
    }
    cpuWindowStartMs = millis();
    cpuWindowBusyMicros = 0;

}

void loop(){
    uint64_t loopStartMicros = esp_timer_get_time();
    maintainWifiAndTime();
    updateEncoderPage();

    static bool previousTimeValid = false;
    bool timeValid = isTimeValid();
    if (timeValid && !previousTimeValid && currentPage == 1) {
        drawHistoryPage();
    }
    previousTimeValid = timeValid;

    uint32_t now = millis();
    if (currentPage == 2 && now - lastClockUpdateMs >= 1000) {
        updateClockPage();
        lastClockUpdateMs = now;
    }
    if (now - lastSensorPollMs >= SENSOR_POLL_INTERVAL_MS) {
        lastSensorPollMs = now;
        uint16_t co2;
        float temperature;
        float humidity;
        bool dataReady = false;
        uint16_t error = scd4x.getDataReadyStatus(dataReady);
        if (error) {
            Serial.print("Error checking data ready status (code ");
            Serial.print(error);
            Serial.println(").");
        } else if (dataReady) {
            error = scd4x.readMeasurement(co2, temperature, humidity);
            if (error) {
                Serial.print("Error reading measurement (code ");
                Serial.print(error);
                Serial.println(").");
            } else {
                latestCo2 = co2;
                latestTemperature = temperature;
                latestHumidity = humidity;
                hasSensorReading = true;
                recordHourlyCo2(co2, time(nullptr));

                now = millis();
                if (currentPage == 0 &&
                    (lastDisplayUpdateMs == 0 || now - lastDisplayUpdateMs >= DISPLAY_UPDATE_INTERVAL_MS)) {
                    updateDisplay(co2, temperature, humidity);
                    lastDisplayUpdateMs = now;
                }
            }
        }
    }

    cpuWindowBusyMicros += esp_timer_get_time() - loopStartMicros;
    now = millis();
    uint32_t cpuWindowDurationMs = now - cpuWindowStartMs;
    if (cpuWindowDurationMs >= SYSTEM_PAGE_UPDATE_INTERVAL_MS) {
        uint64_t windowDurationMicros = (uint64_t)cpuWindowDurationMs * 1000;
        appLoadPercent = (uint8_t)constrain(
            (int)((cpuWindowBusyMicros * 100) / windowDurationMicros), 0, 100);
        cpuWindowBusyMicros = 0;
        cpuWindowStartMs = now;
    }
    if (currentPage == 3 && now - lastSystemPageUpdateMs >= SYSTEM_PAGE_UPDATE_INTERVAL_MS) {
        updateSystemPage();
        lastSystemPageUpdateMs = now;
    }
    delay(1);
}
