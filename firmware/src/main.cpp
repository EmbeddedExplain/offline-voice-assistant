// Episode 1: touch buttons A / B / C (the three zones under the screen) each send one
// preset question to the gateway's POST /ask endpoint and show the answer.
// Episode 2: hold a finger on the screen, speak, release: the recording is sent to
// POST /voice, and the spoken reply is played on the speaker and shown on screen.

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "secrets.h"

// Push-to-talk audio: 16 kHz, 16-bit, mono, at most 8 s (256 KB, kept in PSRAM).
static const uint32_t SAMPLE_RATE = 16000;
static const size_t MAX_RECORD_S = 8;
static const size_t CHUNK_SAMPLES = SAMPLE_RATE / 10;  // record 100 ms at a time
static const size_t MAX_SAMPLES = SAMPLE_RATE * MAX_RECORD_S;
static const size_t WAV_HEADER_BYTES = 44;
static const size_t MIN_SAMPLES = SAMPLE_RATE / 2;  // shorter than 0.5 s = accidental tap

// HTTPClient::setTimeout() takes a uint16_t here: 65 s is the maximum. A cold first request
// (model loaded into VRAM) takes ~45 s, so keep the model warm with OLLAMA_KEEP_ALIVE.
static const uint16_t HTTP_TIMEOUT_MS = 65000;

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
    WiFi.setSleep(false);  // modem sleep (on by default) slows uploads to ~55 KB/s
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
    }
    showScreen("Offline assistant ready",
               "IP: " + WiFi.localIP().toString() + "\nGateway: " + GATEWAY_URL +
                   "\n\nTouch a button below to ask a question,\nor hold the screen and speak.",
               TFT_GREEN);
}

void ask(const char* question) {
    showScreen(question, "Thinking...", TFT_CYAN);
    uint32_t start = millis();

    HTTPClient http;
    http.begin(String(GATEWAY_URL) + "/ask");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);

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

// ---------------------------------------------------------------------------
// Episode 2: voice
// ---------------------------------------------------------------------------

static uint8_t* recWav = nullptr;  // WAV header + samples, allocated once in PSRAM

static void putLE16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void putLE32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

// Standard 44-byte PCM WAV header, so the gateway receives a normal .wav file.
void writeWavHeader(uint8_t* h, size_t samples) {
    uint32_t dataBytes = samples * 2;
    memcpy(h, "RIFF", 4);      putLE32(h + 4, 36 + dataBytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    putLE32(h + 16, 16);       putLE16(h + 20, 1);  // PCM chunk size, format = PCM
    putLE16(h + 22, 1);        putLE32(h + 24, SAMPLE_RATE);  // mono, sample rate
    putLE32(h + 28, SAMPLE_RATE * 2);  putLE16(h + 32, 2);    // byte rate, block align
    putLE16(h + 34, 16);       // bits per sample
    memcpy(h + 36, "data", 4); putLE32(h + 40, dataBytes);
}

// The gateway percent-encodes text headers (HTTP headers must be ASCII).
String urlDecode(const String& s) {
    String out;
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] == '%' && i + 2 < s.length()) {
            out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Records while the screen is touched. Returns the number of samples captured.
size_t recordWhileTouched() {
    int16_t* samples = (int16_t*)(recWav + WAV_HEADER_BYTES);
    M5.Speaker.end();  // on the Core2 the mic and speaker share the I2S bus
    M5.Mic.begin();
    showScreen("Listening...", "Release to send (max " + String(MAX_RECORD_S) + " s)", TFT_RED);

    size_t n = 0;
    while (n + CHUNK_SAMPLES <= MAX_SAMPLES) {
        M5.Mic.record(samples + n, CHUNK_SAMPLES, SAMPLE_RATE);  // queued; blocks when the queue is full
        n += CHUNK_SAMPLES;
        M5.update();
        if (!M5.Touch.getDetail().isPressed()) break;
    }
    while (M5.Mic.isRecording()) delay(1);
    M5.Mic.end();
    M5.Speaker.begin();
    return n;
}

void askByVoice() {
    size_t samples = recordWhileTouched();
    if (samples < MIN_SAMPLES) {
        showScreen("Too short", "Hold the screen while you speak.", TFT_YELLOW);
        return;
    }
    size_t wavLen = WAV_HEADER_BYTES + samples * 2;
    writeWavHeader(recWav, samples);
    showScreen("Thinking...", String(samples / (float)SAMPLE_RATE, 1) + " s of audio sent", TFT_CYAN);
    uint32_t start = millis();

    HTTPClient http;
    http.begin(String(GATEWAY_URL) + "/voice");
    http.addHeader("Content-Type", "audio/wav");
    http.setTimeout(HTTP_TIMEOUT_MS);
    const char* keys[] = {"X-Transcript", "X-Answer", "X-Timings"};
    http.collectHeaders(keys, 3);

    int code = http.POST(recWav, wavLen);
    if (code != 200) {
        String payload = http.getString();
        http.end();
        showScreen("Error", "HTTP " + String(code) + "\n" + payload, TFT_RED);
        Serial.printf("[voice] HTTP %d: %s\n", code, payload.c_str());
        return;
    }

    // Read the WAV reply into PSRAM.
    int len = http.getSize();
    uint8_t* reply = len > 0 ? (uint8_t*)ps_malloc(len) : nullptr;
    if (!reply) {
        http.end();
        showScreen("Error", "Reply too large or missing (" + String(len) + " bytes)", TFT_RED);
        return;
    }
    WiFiClient* stream = http.getStreamPtr();
    int got = 0;
    uint32_t deadline = millis() + 30000;
    while (got < len && millis() < deadline) {
        int avail = stream->available();
        if (avail > 0) got += stream->readBytes(reply + got, min(avail, len - got));
        else delay(1);
    }
    String transcript = urlDecode(http.header("X-Transcript"));
    String answer = urlDecode(http.header("X-Answer"));
    JsonDocument timings;
    deserializeJson(timings, http.header("X-Timings"));
    http.end();
    uint32_t elapsed = millis() - start;

    showScreen("\"" + transcript + "\"",
               answer + "\n\n(" + String(elapsed) + " ms round trip: STT " + String(timings["stt_ms"] | 0) +
                   ", LLM " + String(timings["llm_ms"] | 0) + ", TTS " + String(timings["tts_ms"] | 0) + ")");
    Serial.printf("[voice] \"%s\" -> %s (%u ms, %d bytes of audio)\n", transcript.c_str(), answer.c_str(), elapsed, got);

    if (got == len) {
        M5.Speaker.playWav(reply, len);
        while (M5.Speaker.isPlaying()) delay(10);
    }
    free(reply);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    M5.Display.setRotation(1);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Speaker.setVolume(200);
    recWav = (uint8_t*)ps_malloc(WAV_HEADER_BYTES + MAX_SAMPLES * 2);
    drawFooter();
    connectWiFi();
    if (!recWav) showScreen("No PSRAM", "Voice disabled, buttons still work.", TFT_RED);
}

void loop() {
    M5.update();
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

    if (M5.BtnA.wasPressed()) ask(QUESTIONS[0]);
    if (M5.BtnB.wasPressed()) ask(QUESTIONS[1]);
    if (M5.BtnC.wasPressed()) ask(QUESTIONS[2]);

    // A touch on the screen itself (above the footer) starts push-to-talk.
    auto t = M5.Touch.getDetail();
    if (recWav && t.wasPressed() && t.y < M5.Display.height() - FOOTER_H) askByVoice();
    delay(10);
}
