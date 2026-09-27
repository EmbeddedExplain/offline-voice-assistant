// Episode 1: the Core2 asks a local LLM a preset question and shows the answer.
// Touch buttons A / B / C (the three zones under the screen) each send one question
// to the gateway's POST /ask endpoint.

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "secrets.h"

static const char* QUESTIONS[] = {
    "What is an ESP32?",
    "Tell me a fun fact about space.",
    "Why is local AI good for privacy?",
};
static const char* BUTTON_LABELS[] = {"ESP32", "Space", "Privacy"};

static const int MARGIN = 8;
static const int FOOTER_H = 24;

void drawFooter() {
    int w = M5.Display.width();
    int y = M5.Display.height() - FOOTER_H;
    M5.Display.fillRect(0, y, w, FOOTER_H, TFT_DARKGREY);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    M5.Display.setTextDatum(middle_center);
    for (int i = 0; i < 3; i++) {
        M5.Display.drawString(BUTTON_LABELS[i], w / 6 + i * w / 3, y + FOOTER_H / 2);
    }
    M5.Display.setTextDatum(top_left);
}

// Word-wrapped text in the area above the footer. Returns the y after the last line.
int drawWrapped(const String& text, int y, uint16_t color) {
    M5.Display.setTextColor(color, TFT_BLACK);
    int maxW = M5.Display.width() - 2 * MARGIN;
    int lineH = M5.Display.fontHeight() + 2;
    int bottom = M5.Display.height() - FOOTER_H;
    String line, word;

    auto flushLine = [&]() {
        if (y + lineH <= bottom) M5.Display.drawString(line, MARGIN, y);
        y += lineH;
        line = "";
    };

    for (size_t i = 0; i <= text.length(); i++) {
        char c = i < text.length() ? text[i] : ' ';
        if (c == ' ' || c == '\n') {
            String candidate = line.isEmpty() ? word : line + " " + word;
            if (M5.Display.textWidth(candidate) > maxW && !line.isEmpty()) {
                flushLine();
                line = word;
            } else {
                line = candidate;
            }
            word = "";
            if (c == '\n') flushLine();
        } else {
            word += c;
        }
    }
    if (!line.isEmpty()) flushLine();
    return y;
}

void showScreen(const String& title, const String& body, uint16_t titleColor = TFT_CYAN) {
    M5.Display.fillRect(0, 0, M5.Display.width(), M5.Display.height() - FOOTER_H, TFT_BLACK);
    M5.Display.setFont(&fonts::FreeSansBold9pt7b);
    int y = drawWrapped(title, MARGIN, titleColor);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    drawWrapped(body, y + 6, TFT_WHITE);
}

void connectWiFi() {
    showScreen("Connecting to Wi-Fi...", WIFI_SSID, TFT_YELLOW);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
    }
    showScreen("Offline assistant ready",
               "IP: " + WiFi.localIP().toString() + "\nGateway: " + GATEWAY_URL +
                   "\n\nTouch a button below to ask a question.",
               TFT_GREEN);
}

void ask(const char* question) {
    showScreen(question, "Thinking...", TFT_CYAN);
    uint32_t start = millis();

    HTTPClient http;
    http.begin(String(GATEWAY_URL) + "/ask");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(120000);  // first request may load the model into VRAM

    JsonDocument req;
    req["question"] = question;
    String body;
    serializeJson(req, body);

    int code = http.POST(body);
    String payload = http.getString();
    http.end();
    uint32_t elapsed = millis() - start;

    if (code != 200) {
        showScreen("Error", "HTTP " + String(code) + "\n" + payload, TFT_RED);
        Serial.printf("[ask] HTTP %d: %s\n", code, payload.c_str());
        return;
    }

    JsonDocument res;
    if (deserializeJson(res, payload)) {
        showScreen("Error", "Invalid JSON from gateway", TFT_RED);
        return;
    }
    String answer = res["answer"].as<String>();
    int llmMs = res["timings_ms"]["llm_ms"] | 0;

    showScreen(question, answer + "\n\n(" + String(elapsed) + " ms round trip, LLM " + String(llmMs) + " ms)");
    Serial.printf("[ask] %s -> %s (%u ms)\n", question, answer.c_str(), elapsed);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    M5.Display.setRotation(1);
    M5.Display.fillScreen(TFT_BLACK);
    drawFooter();
    connectWiFi();
}

void loop() {
    M5.update();
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

    if (M5.BtnA.wasPressed()) ask(QUESTIONS[0]);
    if (M5.BtnB.wasPressed()) ask(QUESTIONS[1]);
    if (M5.BtnC.wasPressed()) ask(QUESTIONS[2]);
    delay(10);
}
