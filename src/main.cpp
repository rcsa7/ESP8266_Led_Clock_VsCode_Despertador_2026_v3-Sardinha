#include <Arduino.h>
/**
 * Relógio de Matriz de LED com ESP8266 (Wemos D1 Mini) - v4 Despertador 2026
 *
 * BASEADO EM: Temp_Escritorio_Despertador_VsCode v3
 *
 * NOVIDADES v4 - Sistema de Despertador Completo:
 *   - Alarme configurável: horário e dias da semana
 *   - Soneca configurável (padrão 9 min)
 *   - Som escalante: aumenta intensidade ao longo do tempo
 *   - Auto-desligar após tempo máximo sem resposta
 *   - Display rola "! ACORDA !" durante o alarme
 *   - Botão D1 / GPIO5: toque curto = Soneca / Dispensar
 *   - Botão D4 / GPIO2: segura 5s = Reset WiFi
 *   - Interface web /alarm: status em tempo real, botões SONECA e DISPENSAR
 *   - API /status JSON: estado do alarme para atualização sem reload
 *   - Todas as configurações (incl. alarme) salvas em LittleFS
 *        - CLK -> D5 (SCK)
 *        - CS ->  D6
 *        - DIN -> D7 (MOSI)
 *        - VCC -> 5V+
 *        - GND -> GND-
 *        - BUZZER PIN D8
 *        - TRIGGER PIN D4
 *        - SNOOZE PIN D1
 * 
 * 
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Max72xxPanel.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#define CS_PIN               D6
#define BUZZER_PIN           D8
#define TRIGGER_PIN          D4
#define SNOOZE_PIN           D1

#define HORIZONTAL_DISPLAYS  4
#define VERTICAL_DISPLAYS    1
#define DEFAULT_SCROLL_SPEED 75
#define CHAR_WIDTH           6
#define STATIC_TIME_X_POS    2
#define RESET_HOLD_TIME      5000UL
#define DATE_DISPLAY_INTERVAL 8000UL

const int CITY_ID = 3468615;
const char* UNITS = "metric";
const unsigned long FETCH_INTERVAL = 600000UL;
const unsigned long EVENT_INTERVAL = 30000UL;
const unsigned long EVENT_DURATION = 5000UL;
const char* CONFIG_FILE = "/config.json";

enum MainState { DISPLAYING_TIME, DISPLAYING_SCROLL_TEXT, DISPLAYING_EVENT };
enum DisplayEffect { STATIC, SCROLL_LEFT, SCROLL_RIGHT, SCROLL_UP, SCROLL_DOWN };
enum AlarmState { ALARM_IDLE, ALARM_RINGING, ALARM_SNOOZED };

const uint8_t smiley[] PROGMEM = {0b00111100,0b01000010,0b10100101,0b10000001,0b10100101,0b10011001,0b01000010,0b00111100};
const uint8_t creature[] PROGMEM = {0b00111100,0b01111110,0b11111111,0b11100111,0b11111111,0b01111110,0b00111100,0b00000000};
const uint8_t pacman_open[] PROGMEM = {0b00111100,0b01111110,0b11100011,0b11000011,0b11000011,0b11100011,0b01111110,0b00111100};
const uint8_t duck[] PROGMEM = {0b00000000,0b00110000,0b01111000,0b00111110,0b00111110,0b00111000,0b00010000,0b00000000};
const uint8_t boat[] PROGMEM = {0b00010000,0b00111000,0b01111100,0b11111110,0b01010101,0b10101010,0b01010101,0b00000000};
const uint8_t rocket[] PROGMEM = {0b00011000,0b00111100,0b00111100,0b01111110,0b01111110,0b11111111,0b00100100,0b01011010};
const uint8_t heart[] PROGMEM = {0b01100110,0b11111111,0b11111111,0b11111111,0b01111110,0b00111100,0b00011000,0b00000000};
const uint8_t ghost[] PROGMEM = {0b00111100,0b01111110,0b11011011,0b11111111,0b11111111,0b11111111,0b10101010,0b10101010};
const uint8_t eye_blink[] PROGMEM = {0b00000000,0b00000000,0b11111111,0b01111110,0b01111110,0b11111111,0b00000000,0b00000000};
const uint8_t eye_open[] PROGMEM = {0b00111100,0b01000010,0b10100101,0b10000001,0b10000001,0b10100101,0b01000010,0b00111100};
const uint8_t eye_left[] PROGMEM = {0b00000000,0b01111110,0b11100011,0b11100101,0b11111001,0b11100011,0b01111110,0b00000000};
const uint8_t eye_right[] PROGMEM = {0b00000000,0b01111110,0b11000111,0b10100111,0b10011111,0b11000111,0b01111110,0b00000000};
const uint8_t eye_surprise[] PROGMEM = {0b00000000,0b01111110,0b11000011,0b10011001,0b10011001,0b11000011,0b01111110,0b00000000};

const uint8_t* specialAnimations[] = {smiley, creature, pacman_open, duck, boat, rocket, heart, ghost, eye_blink, eye_open, eye_left, eye_right, eye_surprise};
const int numSpecialAnimations = sizeof(specialAnimations) / sizeof(specialAnimations[0]);

Max72xxPanel matrix = Max72xxPanel(CS_PIN, HORIZONTAL_DISPLAYS, VERTICAL_DISPLAYS);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -10800, 60000);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

const char* version = "Despertador_RobertoCarlos v4";

String weatherApiKey = "";
int cityId = CITY_ID;

float currentTemperature = 0.0;
bool weatherFetched = false;
unsigned long lastWeatherFetch = 0;

int wakeHour = 5, wakeMinute = 0;
int sleepHour = 23, sleepMinute = 0;
bool isDisplayOn = true;
bool sleepModeEnabled = true;

MainState mainState = DISPLAYING_TIME;
unsigned long lastEventTime = 0;
int currentAnimationIndex = 0;
String scrollText = "";
int scrollXPosition = 0;
int scrollTextWidth = 0;
DisplayEffect currentEffect = STATIC;
uint8_t currentIntensity = 3;
int scrollSpeedDelay = DEFAULT_SCROLL_SPEED;
bool randomEffectMode = false;

bool isBuzzerPlaying = false;
unsigned long buzzerStartTime = 0;
unsigned long buzzerDuration = 0;
int buzzerDurationMs = 100;
bool silentAnimationMode = false;
bool isMessageAlert = false;

AlarmState alarmState = ALARM_IDLE;
bool alarmEnabled = false;
int alarmHour = 6;
int alarmMinute = 30;
bool alarmDays[7] = {false, true, true, true, true, true, false};
int snoozeMinutes = 9;
int alarmMaxMinutes = 10;
int alarmToneProfile = 0;
int alarmVolumePercent = 80;
bool alarmTriggeredToday = false;
unsigned long alarmRingStart = 0;
unsigned long snoozeStartTime = 0;
bool alarmAudioActive = false;
int alarmScrollPos = 0;
unsigned long alarmDisplayLastUpdate = 0;
String teenName = "";
bool showTeenNameOnAlarm = false;
volatile bool snoozeButtonIrqEvent = false;
volatile unsigned long snoozeButtonLastIrqMs = 0;

void handleScrollingText();
void displayTimeWithEffect();
void handleEvent();
void setupWebServer();
void playStartupSound();
void playThemeChangeSound();
void handleBuzzer();
void checkSleepSchedule();
void playSound(int frequency);
void playSoundDuration(int frequency, unsigned long durationMs);
void handleWeatherUpdate();
void handleRoot();
void handleSet();
void handleConfig();
void handleSaveConfig();
void handleAlarmPage();
void handleSaveAlarm();
void handleSnooze();
void handleDismiss();
void handleStatus();
void checkAlarm();
void triggerAlarm();
void snoozeAlarm();
void dismissAlarm();
void handleAlarmSound();
void handleAlarmDisplay();
void checkSnoozeButton();
void checkLongPressForReset();
bool loadConfig();
bool saveConfig();
String getOption(DisplayEffect effect, const char* name);
String formatTime(int hour, int minute);
String getDateString();
String getShortDateString();
void startScrollingText(String text);
String getAlarmDisplayMessage();
String htmlEscape(const String& input);
void IRAM_ATTR onSnoozeButtonFalling();

const int ALARM_TONE_PROFILES[4][3] = {
    {880, 1047, 1319},
    {523, 659, 784},
    {740, 988, 1175},
    {988, 1319, 1568}
};

void IRAM_ATTR onSnoozeButtonFalling() {
    unsigned long nowMs = millis();
    if (nowMs - snoozeButtonLastIrqMs >= 60) {
        snoozeButtonLastIrqMs = nowMs;
        snoozeButtonIrqEvent = true;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.printf("\n\nIniciando %s\n", version);
    randomSeed(analogRead(A0));

    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(TRIGGER_PIN, INPUT_PULLUP);
    pinMode(SNOOZE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SNOOZE_PIN), onSnoozeButtonFalling, FALLING);
    analogWriteRange(1023);
    analogWrite(BUZZER_PIN, 0);

    matrix.setIntensity(currentIntensity);
    for (int i = 0; i < HORIZONTAL_DISPLAYS; i++) {
        matrix.setRotation(i, 1);
    }

    if (!LittleFS.begin()) {
        Serial.println("Falha LittleFS. Formatando...");
        LittleFS.format();
        LittleFS.begin();
    }
    loadConfig();

    playStartupSound();

    startScrollingText("HELLO");
    while (mainState == DISPLAYING_SCROLL_TEXT) { handleScrollingText(); yield(); }

    startScrollingText(version);
    while (mainState == DISPLAYING_SCROLL_TEXT) { handleScrollingText(); yield(); }

    WiFiManager wifiManager;
    wifiManager.setAPCallback([](WiFiManager*) { startScrollingText("SETUP"); });
    wifiManager.setConfigPortalTimeout(300);
    if (!wifiManager.autoConnect("RelogioLED-AP", "password")) {
        delay(3000);
        ESP.restart();
    }

    Serial.print("WiFi conectado! IP: ");
    Serial.println(WiFi.localIP());
    startScrollingText("IP: " + WiFi.localIP().toString());
    while (mainState == DISPLAYING_SCROLL_TEXT) { handleScrollingText(); yield(); }

    timeClient.begin();
    setupWebServer();
    httpUpdater.setup(&server, "/update", "admin", "admin");
    server.begin();

    Serial.println("Setup concluido. Relogio em operacao.");
    lastEventTime = millis();
}

void loop() {
    handleBuzzer();
    handleAlarmSound();
    server.handleClient();
    timeClient.update();
    checkSleepSchedule();
    checkSnoozeButton();
    checkLongPressForReset();
    checkAlarm();

    if (!isDisplayOn) {
        return;
    }

    if (millis() - lastWeatherFetch > FETCH_INTERVAL || !weatherFetched) {
        handleWeatherUpdate();
        lastWeatherFetch = millis();
    }

    if (alarmState == ALARM_RINGING) {
        handleAlarmDisplay();
        return;
    }

    if (mainState == DISPLAYING_TIME && millis() - lastEventTime > EVENT_INTERVAL) {
        mainState = DISPLAYING_EVENT;
        lastEventTime = millis();
    }

    switch (mainState) {
        case DISPLAYING_SCROLL_TEXT:
            handleScrollingText();
            break;
        case DISPLAYING_EVENT:
            handleEvent();
            break;
        case DISPLAYING_TIME:
        default:
            displayTimeWithEffect();
            break;
    }
}

bool loadConfig() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("Config nao encontrada. Usando padroes.");
        return false;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        return false;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("Erro config: %s\n", err.c_str());
        return false;
    }

    weatherApiKey = doc["apiKey"] | String("");
    cityId = doc["cityId"] | CITY_ID;
    alarmEnabled = doc["alarmEnabled"] | false;
    alarmHour = doc["alarmHour"] | 6;
    alarmMinute = doc["alarmMinute"] | 30;
    snoozeMinutes = doc["snoozeMinutes"] | 9;
    alarmMaxMinutes = doc["alarmMaxMin"] | 10;
    alarmToneProfile = constrain((int)(doc["alarmToneProfile"] | 0), 0, 3);
    alarmVolumePercent = constrain((int)(doc["alarmVolumePercent"] | 80), 0, 100);
    teenName = doc["teenName"] | String("");
    showTeenNameOnAlarm = doc["showTeenNameOnAlarm"] | false;

    String days = doc["alarmDays"] | String("0111110");
    for (int i = 0; i < 7; i++) {
        alarmDays[i] = (i < (int)days.length()) ? (days[i] == '1') : false;
    }

    Serial.println("Config carregada.");
    return true;
}

bool saveConfig() {
    DynamicJsonDocument doc(2048);
    doc["apiKey"] = weatherApiKey;
    doc["cityId"] = cityId;
    doc["alarmEnabled"] = alarmEnabled;
    doc["alarmHour"] = alarmHour;
    doc["alarmMinute"] = alarmMinute;
    doc["snoozeMinutes"] = snoozeMinutes;
    doc["alarmMaxMin"] = alarmMaxMinutes;
    doc["alarmToneProfile"] = alarmToneProfile;
    doc["alarmVolumePercent"] = alarmVolumePercent;
    doc["teenName"] = teenName;
    doc["showTeenNameOnAlarm"] = showTeenNameOnAlarm;

    String days = "";
    for (int i = 0; i < 7; i++) {
        days += alarmDays[i] ? "1" : "0";
    }
    doc["alarmDays"] = days;

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        return false;
    }
    bool ok = (serializeJson(doc, f) > 0);
    f.close();
    Serial.println(ok ? "Config salva." : "Erro ao salvar config.");
    return ok;
}

void triggerAlarm() {
    alarmState = ALARM_RINGING;
    alarmRingStart = millis();
    alarmAudioActive = true;
    alarmTriggeredToday = true;
    alarmScrollPos = matrix.width();
    alarmDisplayLastUpdate = 0;
    if (!isDisplayOn) {
        isDisplayOn = true;
        matrix.shutdown(false);
        matrix.setIntensity(currentIntensity);
    }
    Serial.printf("ALARME DISPARADO! %02d:%02d\n", alarmHour, alarmMinute);
}

void snoozeAlarm() {
    alarmState = ALARM_SNOOZED;
    snoozeStartTime = millis();
    alarmAudioActive = false;
    noTone(BUZZER_PIN);
    analogWrite(BUZZER_PIN, 0);
    isBuzzerPlaying = false;
    Serial.printf("SONECA ativada por %d minutos.\n", snoozeMinutes);
    startScrollingText("ZZZ " + String(snoozeMinutes) + "min");
}

void dismissAlarm() {
    alarmState = ALARM_IDLE;
    alarmAudioActive = false;
    noTone(BUZZER_PIN);
    analogWrite(BUZZER_PIN, 0);
    isBuzzerPlaying = false;
    Serial.println("Alarme dispensado.");
    startScrollingText("BOM DIA!");
}

void checkAlarm() {
    if (!alarmEnabled) {
        return;
    }

    static int lastDay = -1;
    int curDay = timeClient.getDay();
    int curHour = timeClient.getHours();
    int curMinute = timeClient.getMinutes();

    if (lastDay != curDay) {
        lastDay = curDay;
        alarmTriggeredToday = false;
        Serial.println("Novo dia: flag alarme resetada.");
    }

    if (alarmState == ALARM_IDLE && !alarmTriggeredToday) {
        if (alarmDays[curDay] && curHour == alarmHour && curMinute == alarmMinute) {
            triggerAlarm();
            return;
        }
    }

    if (alarmState == ALARM_SNOOZED) {
        if (millis() - snoozeStartTime >= (unsigned long)snoozeMinutes * 60000UL) {
            triggerAlarm();
            return;
        }
    }

    if (alarmState == ALARM_RINGING) {
        if (millis() - alarmRingStart >= (unsigned long)alarmMaxMinutes * 60000UL) {
            Serial.println("Alarme: tempo maximo. Auto-dispensando.");
            dismissAlarm();
        }
    }
}

void handleAlarmSound() {
    if (alarmState != ALARM_RINGING) {
        return;
    }

    unsigned long elapsed = millis() - alarmRingStart;
    static unsigned long lastToggle = 0;
    static bool beepOn = false;
    unsigned long now = millis();

    unsigned long onTime;
    unsigned long offTime;
    int phaseIdx;
    if (elapsed < 120000UL) {
        phaseIdx = 0;
        onTime = 180;
        offTime = 1820;
    } else if (elapsed < 300000UL) {
        phaseIdx = 1;
        onTime = 250;
        offTime = 750;
    } else {
        phaseIdx = 2;
        onTime = 300;
        offTime = 300;
    }

    unsigned long wait = beepOn ? onTime : offTime;
    if (now - lastToggle >= wait) {
        lastToggle = now;
        beepOn = !beepOn;
        if (beepOn) {
            int freq = ALARM_TONE_PROFILES[alarmToneProfile][phaseIdx];
            int duty = map(alarmVolumePercent, 0, 100, 0, 512);
            analogWriteFreq(freq);
            analogWrite(BUZZER_PIN, duty);
        } else {
            analogWrite(BUZZER_PIN, 0);
        }
    }
}

void handleAlarmDisplay() {
    String msg = getAlarmDisplayMessage();
    int msgWidth = msg.length() * CHAR_WIDTH;

    unsigned long now = millis();
    if (now - alarmDisplayLastUpdate > 55) {
        alarmDisplayLastUpdate = now;
        alarmScrollPos--;
        if (alarmScrollPos < -msgWidth) {
            alarmScrollPos = matrix.width();
        }
        matrix.fillScreen(LOW);
        matrix.setCursor(alarmScrollPos, 0);
        matrix.print(msg);
        matrix.write();
    }
}

void checkSnoozeButton() {
    bool irqEvent = false;
    noInterrupts();
    if (snoozeButtonIrqEvent) {
        snoozeButtonIrqEvent = false;
        irqEvent = true;
    }
    interrupts();

    if (irqEvent) {
        if (alarmState == ALARM_RINGING || alarmState == ALARM_SNOOZED) {
            dismissAlarm();
            Serial.println("Botao D1: SILENCIAR/DISPENSAR");
        }
        return;
    }
}

void checkLongPressForReset() {
    static unsigned long pressStartTime = 0;
    static bool pressing = false;

    if (digitalRead(TRIGGER_PIN) == LOW) {
        if (!pressing) {
            pressing = true;
            pressStartTime = millis();
            Serial.println("Botao D4: iniciando contagem para reset...");
        }
        unsigned long holdTime = millis() - pressStartTime;
        int remaining = (int)ceil((float)(RESET_HOLD_TIME - holdTime) / 1000.0f);

        if (holdTime >= RESET_HOLD_TIME) {
            matrix.fillScreen(LOW);
            matrix.setCursor(0, 0);
            matrix.print("RESET");
            matrix.write();
            WiFiManager wifiManager;
            wifiManager.resetSettings();
            LittleFS.format();
            delay(500);
            ESP.restart();
        } else if (holdTime > 500 && remaining > 0) {
            matrix.fillScreen(LOW);
            matrix.setCursor(0, 0);
            matrix.print("RST " + String(remaining) + "s");
            matrix.write();
        }
    } else {
        if (pressing) {
            pressing = false;
            Serial.println("Botao D4 solto. Reset cancelado.");
            matrix.fillScreen(LOW);
            matrix.write();
        }
    }
}

void checkSleepSchedule() {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 10000) {
        return;
    }
    lastCheck = millis();

    if (alarmState != ALARM_IDLE) {
        return;
    }

    if (!sleepModeEnabled) {
        if (!isDisplayOn) {
            isDisplayOn = true;
            matrix.shutdown(false);
            matrix.setIntensity(currentIntensity);
        }
        return;
    }

    int cur = timeClient.getHours() * 60 + timeClient.getMinutes();
    int wake = wakeHour * 60 + wakeMinute;
    int sleep = sleepHour * 60 + sleepMinute;
    bool shouldBeOn = (sleep > wake) ? (cur >= wake && cur < sleep) : (cur >= wake || cur < sleep);

    if (shouldBeOn && !isDisplayOn) {
        isDisplayOn = true;
        matrix.shutdown(false);
        matrix.setIntensity(currentIntensity);
        Serial.println("Display LIGADO pelo agendamento.");
    } else if (!shouldBeOn && isDisplayOn) {
        isDisplayOn = false;
        matrix.fillScreen(LOW);
        matrix.write();
        matrix.shutdown(true);
        Serial.println("Display DESLIGADO pelo agendamento.");
    }
}

void handleBuzzer() {
    if (isBuzzerPlaying && (millis() - buzzerStartTime > buzzerDuration)) {
        noTone(BUZZER_PIN);
        isBuzzerPlaying = false;
        isMessageAlert = false;
    }
}

void playSound(int frequency) {
    if (alarmAudioActive) {
        return;
    }
    if (isBuzzerPlaying) {
        return;
    }
    if (silentAnimationMode && !isMessageAlert) {
        return;
    }
    tone(BUZZER_PIN, frequency);
    buzzerStartTime = millis();
    buzzerDuration = buzzerDurationMs;
    isBuzzerPlaying = true;
}

void playSoundDuration(int frequency, unsigned long durationMs) {
    if (alarmAudioActive) {
        return;
    }
    if (isBuzzerPlaying) {
        return;
    }
    isMessageAlert = true;
    tone(BUZZER_PIN, frequency);
    buzzerStartTime = millis();
    buzzerDuration = durationMs;
    isBuzzerPlaying = true;
}

void playStartupSound() {
    tone(BUZZER_PIN, 523, 100); delay(120);
    tone(BUZZER_PIN, 659, 100); delay(120);
    tone(BUZZER_PIN, 784, 100); delay(120);
    noTone(BUZZER_PIN);
}

void playThemeChangeSound() {
    playSound(988);
}

void startScrollingText(String text) {
    mainState = DISPLAYING_SCROLL_TEXT;
    scrollText = text;
    scrollTextWidth = text.length() * CHAR_WIDTH;
    scrollXPosition = matrix.width();
}

void handleScrollingText() {
    static unsigned long lastScrollTime = 0;
    if (millis() - lastScrollTime > (unsigned long)scrollSpeedDelay) {
        lastScrollTime = millis();
        scrollXPosition--;
        matrix.fillScreen(LOW);
        matrix.setCursor(scrollXPosition, 0);
        matrix.print(scrollText);
        matrix.write();
        if (scrollXPosition < -scrollTextWidth) {
            mainState = DISPLAYING_TIME;
            matrix.fillScreen(LOW);
            matrix.write();
        }
    }
}

void handleEvent() {
    static unsigned long eventStartTime = 0;
    static bool isTempEvent = false;

    if (eventStartTime == 0) {
        eventStartTime = millis();
        if (weatherFetched && random(2) == 0) {
            isTempEvent = true;
            char tempStr[6];
            dtostrf(currentTemperature, 2, 0, tempStr);
            strcat(tempStr, "C");
            matrix.fillScreen(LOW);
            matrix.setCursor(5, 0);
            matrix.print(tempStr);
            matrix.write();
        } else {
            isTempEvent = false;
            currentAnimationIndex = random(numSpecialAnimations);
        }
    }

    if (millis() - eventStartTime >= EVENT_DURATION) {
        eventStartTime = 0;
        mainState = DISPLAYING_TIME;
        matrix.fillScreen(LOW);
        matrix.write();
        return;
    }

    if (!isTempEvent) {
        static unsigned long lastAnimTime = 0;
        static int animScrollX = 32;
        if (millis() - lastAnimTime > (unsigned long)scrollSpeedDelay) {
            lastAnimTime = millis();
            animScrollX--;
            if (animScrollX < -8) {
                animScrollX = matrix.width();
            }
            matrix.fillScreen(LOW);
            matrix.drawBitmap(animScrollX, 0, specialAnimations[currentAnimationIndex], 8, 8, 1);
            matrix.write();
        }
    }
}

void displayTimeWithEffect() {
    static unsigned long lastAnimTime = 0;
    static int timeScrollX = 0;
    static int timeScrollY = 0;
    static unsigned long lastTimeUpdate = 0;
    static String timeStr = "00:00";
    static unsigned long lastDateToggle = 0;
    static bool showDate = false;
    static int dateScrollX = 0;
    static String dateStr = "";
    static int dateWidth = 0;

    if (millis() - lastTimeUpdate > 1000) {
        lastTimeUpdate = millis();
        String ft = timeClient.getFormattedTime();
        timeStr = ft.substring(0, 5);
        timeStr.setCharAt(2, timeClient.getSeconds() % 2 == 0 ? ':' : ' ');
    }

    if (millis() - lastAnimTime > (unsigned long)scrollSpeedDelay) {
        lastAnimTime = millis();
        matrix.fillScreen(LOW);

        if (millis() - lastDateToggle > DATE_DISPLAY_INTERVAL) {
            showDate = !showDate;
            lastDateToggle = millis();
            if (showDate) {
                dateStr = getShortDateString();
                dateWidth = dateStr.length() * CHAR_WIDTH;
                dateScrollX = matrix.width();
            }
        }

        if (showDate) {
            matrix.setCursor(dateScrollX, 0);
            matrix.print(dateStr);
            dateScrollX--;
            if (dateScrollX < -dateWidth) {
                showDate = false;
                lastDateToggle = millis();
            }
            matrix.write();
            return;
        }

        auto changeEffect = [&]() {
            if (randomEffectMode) {
                DisplayEffect old = currentEffect;
                do {
                    currentEffect = static_cast<DisplayEffect>(random(1, 5));
                } while (currentEffect == old);
            }
        };

        switch (currentEffect) {
            case STATIC:
                matrix.setCursor(STATIC_TIME_X_POS, 0);
                matrix.print(timeStr);
                break;
            case SCROLL_LEFT:
                timeScrollX--;
                if (timeScrollX < -(int)(timeStr.length() * CHAR_WIDTH)) {
                    timeScrollX = matrix.width();
                    changeEffect();
                }
                matrix.setCursor(timeScrollX, 0);
                matrix.print(timeStr);
                break;
            case SCROLL_RIGHT:
                timeScrollX++;
                if (timeScrollX > matrix.width()) {
                    timeScrollX = -(int)(timeStr.length() * CHAR_WIDTH);
                    changeEffect();
                }
                matrix.setCursor(timeScrollX, 0);
                matrix.print(timeStr);
                break;
            case SCROLL_UP:
                timeScrollY--;
                if (timeScrollY < -8) {
                    timeScrollY = matrix.height();
                    changeEffect();
                }
                matrix.setCursor(STATIC_TIME_X_POS, timeScrollY);
                matrix.print(timeStr);
                break;
            case SCROLL_DOWN:
                timeScrollY++;
                if (timeScrollY > matrix.height()) {
                    timeScrollY = -8;
                    changeEffect();
                }
                matrix.setCursor(STATIC_TIME_X_POS, timeScrollY);
                matrix.print(timeStr);
                break;
        }
        matrix.write();
    }
}

void handleWeatherUpdate() {
    if (WiFi.status() != WL_CONNECTED) {
        weatherFetched = false;
        return;
    }
    if (weatherApiKey.length() == 0) {
        weatherFetched = false;
        return;
    }

    WiFiClient client;
    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/weather?id=" + String(cityId) + "&units=" + UNITS + "&appid=" + weatherApiKey;
    http.setTimeout(3000);
    if (!http.begin(client, url)) {
        return;
    }

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        DynamicJsonDocument doc(2048);
        if (!deserializeJson(doc, http.getStream())) {
            if (doc.containsKey("main") && doc["main"].containsKey("temp")) {
                currentTemperature = doc["main"]["temp"].as<float>();
                weatherFetched = true;
                Serial.printf("Temp: %.1f C\n", currentTemperature);
            }
        }
    } else {
        Serial.printf("Falha clima HTTP: %d\n", code);
    }
    http.end();
}

void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/set", HTTP_POST, handleSet);
    server.on("/config", HTTP_GET, handleConfig);
    server.on("/saveconfig", HTTP_POST, handleSaveConfig);
    server.on("/alarm", HTTP_GET, handleAlarmPage);
    server.on("/savealarm", HTTP_POST, handleSaveAlarm);
    server.on("/snooze", HTTP_GET, handleSnooze);
    server.on("/dismiss", HTTP_GET, handleDismiss);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/msg", HTTP_POST, []() {
        if (server.hasArg("message")) {
            String msg = server.arg("message");
            startScrollingText(msg);
            unsigned long dur = (unsigned long)(matrix.width() + msg.length() * CHAR_WIDTH) * scrollSpeedDelay;
            playSoundDuration(880, dur);
            server.sendHeader("Location", "/");
            server.send(303);
        } else {
            server.send(400, "text/plain", "Argumento 'message' ausente.");
        }
    });
}

void handleStatus() {
    String stateStr = (alarmState == ALARM_RINGING) ? "RINGING" : (alarmState == ALARM_SNOOZED) ? "SNOOZED" : "IDLE";
    String snoozeRem = "";
    if (alarmState == ALARM_SNOOZED) {
        unsigned long total = (unsigned long)snoozeMinutes * 60000UL;
        unsigned long elapsed = millis() - snoozeStartTime;
        unsigned long rem = (elapsed < total) ? (total - elapsed) : 0;
        int rm = rem / 60000;
        int rs = (rem % 60000) / 1000;
        snoozeRem = String(rm) + ":" + (rs < 10 ? "0" : "") + String(rs);
    }
    String json = "{";
    json += "\"alarmState\":\"" + stateStr + "\",";
    json += "\"alarmEnabled\":" + String(alarmEnabled ? "true" : "false") + ",";
    json += "\"alarmTime\":\"" + formatTime(alarmHour, alarmMinute) + "\",";
    json += "\"snoozeRemaining\":\"" + snoozeRem + "\",";
    json += "\"currentTime\":\"" + timeClient.getFormattedTime().substring(0, 5) + "\"}";
    server.send(200, "application/json", json);
}

void handleSnooze() {
    if (alarmState == ALARM_RINGING) {
        snoozeAlarm();
    }
    server.sendHeader("Location", "/alarm");
    server.send(303);
}

void handleDismiss() {
    if (alarmState == ALARM_RINGING || alarmState == ALARM_SNOOZED) {
        dismissAlarm();
    }
    server.sendHeader("Location", "/alarm");
    server.send(303);
}

void handleAlarmPage() {
    String bannerClass, bannerMsg, bannerBtns;
    String teenNameClean = teenName;
    teenNameClean.trim();
    String teenNameEscaped = htmlEscape(teenNameClean);
    if (alarmState == ALARM_RINGING) {
        bannerClass = "alarm-ringing";
        bannerMsg = "ALARME TOCANDO!";
        if (showTeenNameOnAlarm && teenNameEscaped.length() > 0) {
            bannerMsg += " " + teenNameEscaped;
        }
        bannerBtns = "<a href='/snooze' class='btn-alarm btn-snooze'>SONECA (" + String(snoozeMinutes) + " min)</a>"
                     "<a href='/dismiss' class='btn-alarm btn-dismiss'>DISPENSAR ALARME</a>";
    } else if (alarmState == ALARM_SNOOZED) {
        unsigned long total = (unsigned long)snoozeMinutes * 60000UL;
        unsigned long elapsed = millis() - snoozeStartTime;
        unsigned long rem = (elapsed < total) ? (total - elapsed) : 0;
        int rm = rem / 60000;
        int rs = (rem % 60000) / 1000;
        bannerClass = "alarm-snoozed";
        bannerMsg = "SONECA - faltam " + String(rm) + ":" + (rs < 10 ? "0" : "") + String(rs);
        bannerBtns = "<a href='/dismiss' class='btn-alarm btn-dismiss'>DISPENSAR AGORA</a>";
    } else {
        bannerClass = "alarm-idle";
        bannerMsg = alarmEnabled ? ("Alarme: " + formatTime(alarmHour, alarmMinute)) : "Alarme desativado";
        bannerBtns = "";
    }

    const char* dayNames[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sab"};
    String dayBoxes = "";
    for (int i = 0; i < 7; i++) {
        dayBoxes += "<label class='dl'><input type='checkbox' name='day" + String(i) + "' value='1'";
        if (alarmDays[i]) {
            dayBoxes += " checked";
        }
        dayBoxes += "><span>" + String(dayNames[i]) + "</span></label>";
    }

    String html = F(R"rawliteral(
<!DOCTYPE html><html><head>
<title>Despertador</title>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<meta charset='UTF-8'>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;padding:16px;color:#2d3748}
.hdr{text-align:center;color:white;margin-bottom:20px}
.hdr h1{font-size:1.8rem;font-weight:700;text-shadow:0 2px 4px rgba(0,0,0,.25)}
.wrap{max-width:480px;margin:0 auto}
.card{background:#fff;border-radius:12px;padding:20px;margin-bottom:14px;box-shadow:0 4px 6px rgba(0,0,0,.08)}
.ctitle{font-size:1.05rem;font-weight:600;margin-bottom:14px;display:flex;align-items:center;gap:8px}
.ctitle::before{content:'';width:4px;height:16px;background:#4299e1;border-radius:2px}
.alarm-ringing{border-radius:12px;padding:20px;margin-bottom:14px;text-align:center;background:linear-gradient(135deg,#ff4444,#cc0000);color:white;animation:pulse 1s infinite}
.alarm-snoozed{border-radius:12px;padding:20px;margin-bottom:14px;text-align:center;background:linear-gradient(135deg,#ff9800,#e65100);color:white}
.alarm-idle{border-radius:12px;padding:16px;margin-bottom:14px;text-align:center;background:#f0fff4;border:2px solid #48bb78;color:#276749;font-weight:700;font-size:1.1rem}
.alarm-ringing .bttl,.alarm-snoozed .bttl{font-size:1.4rem;font-weight:800;margin-bottom:14px}
@keyframes pulse{0%,100%{transform:scale(1)}50%{transform:scale(1.02)}}
.btn-alarm{display:block;width:100%;padding:18px;border-radius:10px;font-size:1.2rem;font-weight:800;text-decoration:none;text-align:center;margin-bottom:10px;letter-spacing:.3px}
.btn-snooze{background:#43a047;color:white}
.btn-dismiss{background:#1a1a2e;color:white}
.fg{margin-bottom:14px}
.fg label{display:block;font-size:.85rem;font-weight:600;color:#4a5568;margin-bottom:5px}
.fg input[type=time],.fg input[type=number],.fg input[type=text],.fg input[type=range],.fg select{width:100%;padding:10px;border:2px solid #e2e8f0;border-radius:8px;font-size:1rem}
.fg input:focus{outline:none;border-color:#4299e1}
.tg{display:flex;align-items:center;justify-content:space-between;background:#f7fafc;padding:12px;border-radius:8px;margin-bottom:14px}
.sw{position:relative;width:48px;height:24px}.sw input{opacity:0;width:0;height:0}
.ss{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#cbd5e0;border-radius:24px;transition:.3s}
.ss:before{content:'';position:absolute;height:16px;width:16px;left:4px;bottom:4px;background:white;border-radius:50%;transition:.3s}
.sw input:checked+.ss{background:#48bb78}.sw input:checked+.ss:before{transform:translateX(24px)}
.days{display:grid;grid-template-columns:repeat(7,1fr);gap:4px;margin-top:6px}
.dl{display:flex;flex-direction:column;align-items:center;background:#f7fafc;border-radius:6px;padding:7px 2px;font-size:.75rem;font-weight:700;cursor:pointer;border:2px solid #e2e8f0;transition:.2s;color:#4a5568}
.dl input{margin-bottom:4px;cursor:pointer}
.dl:has(input:checked){background:#ebf8ff;border-color:#4299e1;color:#2b6cb0}
.btn{display:block;width:100%;padding:13px;border:none;border-radius:8px;font-size:.95rem;font-weight:700;cursor:pointer;text-decoration:none;text-align:center;margin-top:8px;color:white}
.btn-save{background:linear-gradient(135deg,#4299e1,#3182ce)}
.btn-back{background:linear-gradient(135deg,#718096,#4a5568)}
.footer{text-align:center;color:white;opacity:.8;font-size:.78rem;margin-top:14px}
</style>
<script>
function poll(){
  fetch('/status').then(r=>r.json()).then(d=>{
    if(d.alarmState!=='IDLE') location.reload();
  }).catch(()=>{});
}
setInterval(poll,8000);
</script>
</head><body>
<div class='hdr'><h1>⏰ Despertador</h1></div>
<div class='wrap'>
)rawliteral");

    html += "<div class='" + bannerClass + "'>";
    if (alarmState != ALARM_IDLE) {
        html += "<div class='bttl'>";
        if (alarmState == ALARM_RINGING) {
            html += "🚨 ";
        } else {
            html += "😴 ";
        }
        html += bannerMsg + "</div>";
    } else {
        html += bannerMsg;
    }
    html += bannerBtns;
    html += "</div>";

    html += R"rawliteral(
<div class='card'>
<div class='ctitle'>⚙️ Configurar Alarme</div>
<form action='/savealarm' method='POST'>
<div class='tg'>
  <span style='font-weight:700'>Alarme Ativo</span>
  <label class='sw'><input type='checkbox' name='alarmEnabled' value='1')rawliteral";
    if (alarmEnabled) {
        html += " checked";
    }
    html += R"rawliteral(><span class='ss'></span></label>
</div>
<div class='fg'>
  <label>⏰ Horário do Alarme</label>
  <input type='time' name='alarmTime' value=')rawliteral";
    html += formatTime(alarmHour, alarmMinute);
    html += R"rawliteral('>
</div>
<div class='fg'>
  <label>📅 Dias da Semana</label>
  <div class='days'>)rawliteral";
    html += dayBoxes;
    html += R"rawliteral(</div>
</div>
<div class='fg'>
  <label>😴 Soneca (minutos)</label>
  <input type='number' name='snoozeMinutes' min='1' max='30' value=')rawliteral";
    html += String(snoozeMinutes);
    html += R"rawliteral('>
</div>
<div class='fg'>
  <label>🔕 Desligar alarme após (minutos sem resposta)</label>
  <input type='number' name='alarmMaxMin' min='1' max='60' value=')rawliteral";
    html += String(alarmMaxMinutes);
    html += R"rawliteral('>
</div>
<div class='fg'>
    <label>🎵 Tom do Alarme</label>
    <select name='alarmToneProfile'>)rawliteral";
        html += "<option value='0'" + String(alarmToneProfile == 0 ? " selected" : "") + ">Padrao</option>";
        html += "<option value='1'" + String(alarmToneProfile == 1 ? " selected" : "") + ">Suave</option>";
        html += "<option value='2'" + String(alarmToneProfile == 2 ? " selected" : "") + ">Energetico</option>";
        html += "<option value='3'" + String(alarmToneProfile == 3 ? " selected" : "") + ">Agudo</option>";
        html += R"rawliteral(</select>
</div>
<div class='fg'>
    <label>🔊 Volume do Alarme: <span id='alarmVolValue'>)rawliteral";
        html += String(alarmVolumePercent);
        html += R"rawliteral(</span>%</label>
    <input type='range' name='alarmVolumePercent' min='0' max='100' value=')rawliteral";
        html += String(alarmVolumePercent);
        html += R"rawliteral(' oninput="document.getElementById('alarmVolValue').textContent=this.value">
</div>
<div class='fg'>
    <label>👤 Nome do Adolescente</label>
    <input type='text' name='teenName' maxlength='24' value=')rawliteral";
        html += teenNameEscaped;
        html += R"rawliteral(' placeholder='Ex: Joao'>
</div>
<div class='tg'>
    <span style='font-weight:700'>Mostrar nome quando alarme tocar</span>
    <label class='sw'><input type='checkbox' name='showTeenNameOnAlarm' value='1')rawliteral";
        if (showTeenNameOnAlarm) {
                html += " checked";
        }
        html += R"rawliteral(><span class='ss'></span></label>
</div>
<button type='submit' class='btn btn-save'>💾 Salvar Alarme</button>
</form>
</div>
<a href='/' class='btn btn-back'>← Voltar ao Painel Principal</a>
</div>
<div class='footer'>⏰ )rawliteral";
    html += version;
    html += " • D1 Mini ESP8266</div></body></html>";

    server.send(200, "text/html", html);
}

void handleSaveAlarm() {
    if (server.method() != HTTP_POST) {
        server.send(405);
        return;
    }

    bool previousAlarmEnabled = alarmEnabled;
    int previousAlarmHour = alarmHour;
    int previousAlarmMinute = alarmMinute;
    bool previousAlarmDays[7];
    for (int i = 0; i < 7; i++) {
        previousAlarmDays[i] = alarmDays[i];
    }

    alarmEnabled = server.hasArg("alarmEnabled");

    if (server.hasArg("alarmTime")) {
        alarmHour = server.arg("alarmTime").substring(0, 2).toInt();
        alarmMinute = server.arg("alarmTime").substring(3, 5).toInt();
    }
    for (int i = 0; i < 7; i++) {
        alarmDays[i] = server.hasArg("day" + String(i));
    }

    if (server.hasArg("snoozeMinutes")) {
        snoozeMinutes = constrain(server.arg("snoozeMinutes").toInt(), 1, 30);
    }

    if (server.hasArg("alarmMaxMin")) {
        alarmMaxMinutes = constrain(server.arg("alarmMaxMin").toInt(), 1, 60);
    }
    if (server.hasArg("alarmToneProfile")) {
        alarmToneProfile = constrain(server.arg("alarmToneProfile").toInt(), 0, 3);
    }
    if (server.hasArg("alarmVolumePercent")) {
        alarmVolumePercent = constrain(server.arg("alarmVolumePercent").toInt(), 0, 100);
    }

    if (server.hasArg("teenName")) {
        teenName = server.arg("teenName");
        teenName.trim();
        if (teenName.length() > 24) {
            teenName = teenName.substring(0, 24);
        }
    }
    showTeenNameOnAlarm = server.hasArg("showTeenNameOnAlarm");

    bool alarmScheduleChanged = (alarmEnabled != previousAlarmEnabled) ||
        (alarmHour != previousAlarmHour) ||
        (alarmMinute != previousAlarmMinute);
    if (!alarmScheduleChanged) {
        for (int i = 0; i < 7; i++) {
            if (alarmDays[i] != previousAlarmDays[i]) {
                alarmScheduleChanged = true;
                break;
            }
        }
    }
    if (alarmScheduleChanged) {
        alarmTriggeredToday = false;
        Serial.println("Config do alarme alterada: liberado novo disparo para o horario salvo.");
    }

    saveConfig();
    Serial.printf("Alarme salvo: %s %02d:%02d snooze=%dmin max=%dmin tom=%d vol=%d nome='%s' mostrarNome=%s\n",
        alarmEnabled ? "ON" : "OFF",
        alarmHour,
        alarmMinute,
        snoozeMinutes,
        alarmMaxMinutes,
        alarmToneProfile,
        alarmVolumePercent,
        teenName.c_str(),
        showTeenNameOnAlarm ? "ON" : "OFF");

    server.sendHeader("Location", "/alarm");
    server.send(303);
}

void handleRoot() {
    String alarmColor = "#48bb78";
    String alarmText = "Desativado";
    String topBanner = "";
    if (alarmEnabled) {
        alarmText = "&#9200; " + formatTime(alarmHour, alarmMinute);
        alarmColor = "#4299e1";
    }
    if (alarmState == ALARM_RINGING) {
        String teenNameClean = teenName;
        teenNameClean.trim();
        String ringTitle = "&#128680; ALARME TOCANDO!";
        if (showTeenNameOnAlarm && teenNameClean.length() > 0) {
            ringTitle += " - " + htmlEscape(teenNameClean);
        }
        alarmText = "&#128680; TOCANDO!";
        alarmColor = "#f56565";
        topBanner = "<div style='background:linear-gradient(135deg,#ff4444,#cc0000);color:white;border-radius:12px;"
                    "padding:18px;margin-bottom:16px;text-align:center;animation:pulse 1s infinite'>"
                    "<div style='font-size:1.4rem;font-weight:800;margin-bottom:12px'>" + ringTitle + "</div>"
                    "<a href='/snooze' style='display:block;padding:15px;background:#43a047;color:white;text-decoration:none;"
                    "border-radius:10px;font-size:1.1rem;font-weight:800;margin-bottom:8px'>&#128564; SONECA (" + String(snoozeMinutes) + " min)</a>"
                    "<a href='/dismiss' style='display:block;padding:15px;background:#1a1a2e;color:white;text-decoration:none;"
                    "border-radius:10px;font-size:1.1rem;font-weight:800'>&#9989; DISPENSAR</a>"
                    "</div>";
    } else if (alarmState == ALARM_SNOOZED) {
        alarmText = "&#128564; SONECA";
        alarmColor = "#ed8936";
        topBanner = "<div style='background:linear-gradient(135deg,#ff9800,#e65100);color:white;border-radius:12px;"
                    "padding:18px;margin-bottom:16px;text-align:center'>"
                    "<div style='font-size:1.3rem;font-weight:800;margin-bottom:12px'>&#128564; EM SONECA</div>"
                    "<a href='/dismiss' style='display:block;padding:15px;background:#1a1a2e;color:white;text-decoration:none;"
                    "border-radius:10px;font-size:1.1rem;font-weight:800'>&#9989; DISPENSAR AGORA</a>"
                    "</div>";
    }

    String html = R"rawliteral(<!DOCTYPE html><html><head>
<title>Relogio LED</title>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<meta charset='UTF-8'>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#f5f7fa;--card:#fff;--t:#2d3748;--s:#718096;--b:#4299e1;--g:#48bb78;--o:#ed8936;--r:#f56565;--bd:#e2e8f0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;padding:16px;color:var(--t)}
.hdr{text-align:center;color:white;margin-bottom:22px}
.hdr h1{font-size:1.9rem;font-weight:700;text-shadow:0 2px 4px rgba(0,0,0,.2)}
.hdr p{font-size:.88rem;opacity:.9}
.wrap{max-width:580px;margin:0 auto}
.card{background:var(--card);border-radius:12px;padding:20px;margin-bottom:14px;box-shadow:0 4px 6px rgba(0,0,0,.07)}
.ctitle{font-size:1.1rem;font-weight:600;color:var(--t);margin-bottom:14px;display:flex;align-items:center;gap:8px}
.ctitle::before{content:'';width:4px;height:18px;background:var(--b);border-radius:2px}
.igrid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-top:10px}
.iitem{background:var(--bg);padding:10px;border-radius:8px;text-align:center}
.ilbl{font-size:.7rem;color:var(--s);text-transform:uppercase;letter-spacing:.5px;margin-bottom:3px}
.ival{font-size:1rem;font-weight:700}
.fg{margin-bottom:14px}
.fg label{display:block;font-size:.85rem;font-weight:600;color:var(--t);margin-bottom:5px}
.fg input,.fg select{width:100%;padding:10px;border:2px solid var(--bd);border-radius:8px;font-size:.95rem}
.fg input:focus,.fg select:focus{outline:none;border-color:var(--b)}
.sc{display:flex;align-items:center;gap:10px}
.sc input[type=range]{flex:1}
.sv{min-width:40px;text-align:center;font-weight:600;color:var(--b);background:var(--bg);padding:5px 10px;border-radius:6px}
.tg{display:flex;align-items:center;justify-content:space-between;background:var(--bg);padding:12px;border-radius:8px}
.sw{position:relative;width:48px;height:24px}.sw input{opacity:0;width:0;height:0}
.ss{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#cbd5e0;border-radius:24px;transition:.3s}
.ss:before{content:'';position:absolute;height:16px;width:16px;left:4px;bottom:4px;background:white;border-radius:50%;transition:.3s}
.sw input:checked+.ss{background:var(--g)}.sw input:checked+.ss:before{transform:translateX(24px)}
.ti{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btn{display:block;width:100%;padding:13px;border:none;border-radius:8px;font-size:.95rem;font-weight:700;cursor:pointer;text-decoration:none;text-align:center;margin-top:8px;color:white;transition:.2s}
.bp{background:linear-gradient(135deg,#4299e1,#3182ce)}
.bg{background:linear-gradient(135deg,#48bb78,#38a169)}
.br{background:linear-gradient(135deg,#f56565,#c53030)}
.bw{background:linear-gradient(135deg,#ed8936,#dd6b20)}
@keyframes pulse{0%,100%{transform:scale(1)}50%{transform:scale(1.02)}}
.footer{text-align:center;color:white;opacity:.8;font-size:.8rem;margin-top:14px}
</style>
<script>
function chk(){fetch('/status').then(r=>r.json()).then(d=>{if(d.alarmState!=='IDLE')location.reload();}).catch(()=>{});}
setInterval(chk,8000);
</script>
</head><body>
<div class='hdr'><h1>&#128336; Relógio LED Matrix</h1><p>Painel de Controle • )rawliteral";
    html += version;
    html += "</p></div><div class='wrap'>";
    html += topBanner;

    html += "<div class='card'><div class='ctitle'>&#128202; Status</div><div class='igrid'>";
    html += "<div class='iitem'><div class='ilbl'>IP</div><div class='ival'>" + WiFi.localIP().toString() + "</div></div>";
    html += "<div class='iitem'><div class='ilbl'>Data</div><div class='ival'>" + getShortDateString() + "</div></div>";
    html += "<div class='iitem'><div class='ilbl'>Temperatura</div><div class='ival'>" + (weatherFetched ? (String(currentTemperature, 1) + "°C") : String("--")) + "</div></div>";
    html += "<div class='iitem'><div class='ilbl'>Alarme</div><div class='ival' style='color:" + alarmColor + "'>" + alarmText + "</div></div>";
    html += "</div></div>";

    html += R"rawliteral(<div class='card'><div class='ctitle'>&#128172; Enviar Mensagem</div>
<form action='/msg' method='POST'>
<div class='fg'><label>Texto para o display</label>
<input type='text' name='message' placeholder='Digite a mensagem...'></div>
<button type='submit' class='btn bp'>&#128228; Enviar</button>
</form></div>)rawliteral";

    html += R"rawliteral(<div class='card'><div class='ctitle'>&#9881;&#65039; Configurações de Display</div>
<form action='/set' method='POST'>
<div class='fg'><label>&#128161; Brilho</label>
<div class='sc'><input type='range' name='brightness' min='0' max='15' value=')rawliteral";
    html += String(currentIntensity);
    html += R"rawliteral(' oninput='this.nextElementSibling.value=this.value'>
<output class='sv'>)rawliteral";
    html += String(currentIntensity);
    html += R"rawliteral(</output></div></div>
<div class='fg'><label>&#9889; Velocidade da Rolagem</label>
<div class='sc'><input type='range' name='speed' min='25' max='150' value=')rawliteral";
    html += String(scrollSpeedDelay);
    html += R"rawliteral(' oninput='this.nextElementSibling.value=this.value'>
<output class='sv'>)rawliteral";
    html += String(scrollSpeedDelay);
    html += R"rawliteral(</output></div></div>
<div class='fg'><label>&#128266; Duração Buzzer (ms)</label>
<input type='number' name='buzzerDurationMs' min='20' max='500' value=')rawliteral";
    html += String(buzzerDurationMs);
    html += R"rawliteral('></div>
<div class='tg' style='margin-bottom:14px'><span style='font-weight:600'>&#127922; Modo Aleatório</span>
<label class='sw'><input type='checkbox' name='randomMode' value='true')rawliteral";
    if (randomEffectMode) {
        html += " checked";
    }
    html += R"rawliteral(><span class='ss'></span></label></div>
<div class='fg'><label>&#127917; Efeito da Hora</label>
<select name='effect')rawliteral";
    if (randomEffectMode) {
        html += " disabled";
    }
    html += ">";
    html += getOption(STATIC, "Estatico (Piscando)");
    html += getOption(SCROLL_LEFT, "Rolar Esquerda");
    html += getOption(SCROLL_RIGHT, "Rolar Direita");
    html += getOption(SCROLL_UP, "Rolar Cima");
    html += getOption(SCROLL_DOWN, "Rolar Baixo");
    html += R"rawliteral(</select></div>
<button type='submit' class='btn bp'>&#128190; Salvar Display</button>
</form></div>)rawliteral";

    html += R"rawliteral(<div class='card'><div class='ctitle'>&#127769; Modo Dormir</div>
<form action='/set' method='POST'>
<div class='tg' style='margin-bottom:14px'><span style='font-weight:600'>Ativar Agendamento</span>
<label class='sw'><input type='checkbox' name='sleepModeEnabled' value='true')rawliteral";
    if (sleepModeEnabled) {
        html += " checked";
    }
    html += R"rawliteral(><span class='ss'></span></label></div>
<div class='ti'>
<div class='fg'><label>&#9200; Ligar</label>
<input type='time' name='wakeHour' value=')rawliteral";
    html += formatTime(wakeHour, wakeMinute);
    html += R"rawliteral('></div>
<div class='fg'><label>&#128564; Desligar</label>
<input type='time' name='sleepHour' value=')rawliteral";
    html += formatTime(sleepHour, sleepMinute);
    html += R"rawliteral('></div></div>
<button type='submit' class='btn bg'>&#128190; Salvar Agendamento</button>
</form></div>)rawliteral";

    html += "<a href='/alarm' class='btn br' style='padding:15px;font-size:1rem;font-weight:800;margin-bottom:6px'>&#9200; Configurar Despertador</a>";
    html += "<a href='/config' class='btn bw'>&#128273; API OpenWeatherMap</a>";
    html += R"rawliteral(<div class='card' style='margin-top:8px'><div class='ctitle'>&#128257; Atualização OTA</div>
<a href='/update' class='btn bg' target='_blank'>&#128225; Atualizar Firmware</a>
<p style='font-size:.8rem;color:#718096;text-align:center;margin-top:10px'>Login: <b>admin</b> / Senha: <b>admin</b></p>
</div>)rawliteral";

    html += "</div><div class='footer'>Desenvolvido com &#10084;&#65039; &bull; ESP8266 &bull; Max7219 &bull; ";
    html += version;
    html += "</div></body></html>";

    server.send(200, "text/html", html);
}

void handleConfig() {
    String html = R"rawliteral(<!DOCTYPE html><html><head>
<title>API Config</title>
<meta name='viewport' content='width=device-width,initial-scale=1'><meta charset='UTF-8'>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;padding:16px;color:#2d3748}
.hdr{text-align:center;color:white;margin-bottom:22px}
.hdr h1{font-size:1.8rem;font-weight:700}
.wrap{max-width:480px;margin:0 auto}
.card{background:#fff;border-radius:12px;padding:20px;margin-bottom:14px;box-shadow:0 4px 6px rgba(0,0,0,.07)}
.ctitle{font-size:1.1rem;font-weight:600;margin-bottom:14px}
.fg{margin-bottom:14px}
.fg label{display:block;font-size:.85rem;font-weight:600;color:#4a5568;margin-bottom:5px}
.fg input{width:100%;padding:10px;border:2px solid #e2e8f0;border-radius:8px;font-size:.95rem}
.fg input:focus{outline:none;border-color:#4299e1}
.fg small{display:block;margin-top:4px;color:#718096;font-size:.8rem}
.btn{display:block;width:100%;padding:13px;border:none;border-radius:8px;font-size:.95rem;font-weight:700;cursor:pointer;text-decoration:none;text-align:center;margin-top:8px;color:white}
.bg{background:linear-gradient(135deg,#48bb78,#38a169)}.bp{background:linear-gradient(135deg,#4299e1,#3182ce)}
.alert{padding:10px;border-radius:8px;margin-bottom:12px;background:#fef3c7;color:#92400e;border:1px solid #fbbf24;font-size:.85rem}
</style></head><body>
<div class='hdr'><h1>&#128273; Configuração API</h1></div>
<div class='wrap'><div class='card'>
<div class='alert'>&#9888;&#65039; Mantenha sua API Key em segurança.</div>
<div class='ctitle'>OpenWeatherMap API</div>
<form action='/saveconfig' method='POST'>
<div class='fg'><label>&#128273; API Key</label>
<input type='text' name='apiKey' value=')rawliteral";
    html += weatherApiKey;
    html += R"rawliteral(' placeholder='Cole sua API Key'>
<small>Obtenha em <a href='https://openweathermap.org/api' target='_blank'>openweathermap.org/api</a></small>
</div>
<div class='fg'><label>&#127758; City ID</label>
<input type='number' name='cityId' value=')rawliteral";
    html += String(cityId);
    html += R"rawliteral('>
<small>Encontre em <a href='https://openweathermap.org/find' target='_blank'>openweathermap.org/find</a></small>
</div>
<button type='submit' class='btn bg'>&#128190; Salvar</button>
<a href='/' class='btn bp'>&#8592; Voltar</a>
</form></div></div></body></html>)rawliteral";
    server.send(200, "text/html", html);
}

void handleSaveConfig() {
    if (server.method() != HTTP_POST) {
        server.send(405);
        return;
    }
    if (server.hasArg("apiKey")) {
        weatherApiKey = server.arg("apiKey");
    }
    if (server.hasArg("cityId")) {
        cityId = server.arg("cityId").toInt();
    }
    if (saveConfig()) {
        weatherFetched = false;
        lastWeatherFetch = 0;
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleSet() {
    if (server.method() != HTTP_POST) {
        server.send(405);
        return;
    }

    if (server.hasArg("brightness")) {
        currentIntensity = (uint8_t)server.arg("brightness").toInt();
        if (isDisplayOn) {
            matrix.setIntensity(currentIntensity);
        }
    }
    if (server.hasArg("speed")) {
        scrollSpeedDelay = server.arg("speed").toInt();
    }
    if (server.hasArg("buzzerDurationMs")) {
        buzzerDurationMs = constrain(server.arg("buzzerDurationMs").toInt(), 20, 500);
    }

    bool newRandom = server.hasArg("randomMode");
    if (newRandom != randomEffectMode) {
        randomEffectMode = newRandom;
        playThemeChangeSound();
        if (randomEffectMode && currentEffect == STATIC) {
            currentEffect = static_cast<DisplayEffect>(random(1, 5));
        }
    }
    if (!randomEffectMode && server.hasArg("effect")) {
        DisplayEffect ne = static_cast<DisplayEffect>(server.arg("effect").toInt());
        if (ne != currentEffect) {
            currentEffect = ne;
            playThemeChangeSound();
        }
    }

    sleepModeEnabled = server.hasArg("sleepModeEnabled");
    if (server.hasArg("wakeHour")) {
        wakeHour = server.arg("wakeHour").substring(0, 2).toInt();
        wakeMinute = server.arg("wakeHour").substring(3, 5).toInt();
    }
    if (server.hasArg("sleepHour")) {
        sleepHour = server.arg("sleepHour").substring(0, 2).toInt();
        sleepMinute = server.arg("sleepHour").substring(3, 5).toInt();
    }

    checkSleepSchedule();
    server.sendHeader("Location", "/", true);
    server.send(302);
}

String getOption(DisplayEffect effect, const char* name) {
    String s = "<option value='";
    s += (int)effect;
    s += "'";
    if (effect == currentEffect) {
        s += " selected";
    }
    s += ">";
    s += name;
    s += "</option>";
    return s;
}

String getAlarmDisplayMessage() {
    String name = teenName;
    name.trim();
    if (showTeenNameOnAlarm && name.length() > 0) {
        return "! ACORDA " + name + " !";
    }
    return "! ACORDA !";
}

String htmlEscape(const String& input) {
    String out = "";
    out.reserve(input.length());
    for (size_t i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else if (c == '"') {
            out += "&quot;";
        } else if (c == '\'') {
            out += "&#39;";
        } else {
            out += c;
        }
    }
    return out;
}

String formatTime(int hour, int minute) {
    return (hour < 10 ? "0" : "") + String(hour) + ":" + (minute < 10 ? "0" : "") + String(minute);
}

String getDateString() {
    time_t epoch = timeClient.getEpochTime();
    struct tm* ptm = gmtime(&epoch);
    return (ptm->tm_mday < 10 ? "0" : "") + String(ptm->tm_mday) + "/" + (ptm->tm_mon + 1 < 10 ? "0" : "") + String(ptm->tm_mon + 1) + "/" + String(ptm->tm_year + 1900);
}

String getShortDateString() {
    time_t epoch = timeClient.getEpochTime();
    struct tm* ptm = gmtime(&epoch);
    int y = (ptm->tm_year + 1900) % 100;
    return (ptm->tm_mday < 10 ? "0" : "") + String(ptm->tm_mday) + "/" + (ptm->tm_mon + 1 < 10 ? "0" : "") + String(ptm->tm_mon + 1) + "/" + (y < 10 ? "0" : "") + String(y);
}
