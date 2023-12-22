#include "wifi_config.h"
// #define WIFI_SSID "your-ssid"
// #define WIFI_PASSWD "your-password"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#define PANEL_WIDTH 64
#define PANEL_HEIGHT 32

#define PANEL_PIN_A 18
#define PANEL_PIN_R1 23
#define PANEL_PIN_G1 22
#define PANEL_PIN_B1 21
#define PANEL_PIN_R2 0
#define PANEL_PIN_G2 2
#define PANEL_PIN_CLK 14
#define PANEL_PIN_OE 25

#define N_REQUESTED 4

#define SCROLL_INTERVAL 20

int32_t scroll_offset = 0;
int32_t last_scroll_time = 0;

#define UPDATE_INTERVAL 30000

int32_t last_update_time = 0;

#define PAGE_SWITCH_INTERVAL 7500

int32_t last_page_switch_time = 0;
int32_t current_page = N_REQUESTED - 1;

#define MAX_JSON_SIZE (N_REQUESTED * 10000)

MatrixPanel_I2S_DMA display;
 
static void init_display(void) {
    HUB75_I2S_CFG mx_config(PANEL_WIDTH, PANEL_HEIGHT, 1);

    mx_config.gpio.a = PANEL_PIN_A;
    mx_config.gpio.r1 = PANEL_PIN_R1;
    mx_config.gpio.g1 = PANEL_PIN_G1;
    mx_config.gpio.b1 = PANEL_PIN_B1;
    mx_config.gpio.r2 = PANEL_PIN_R2;
    mx_config.gpio.g2 = PANEL_PIN_G2;
    mx_config.gpio.clk = PANEL_PIN_CLK;
    mx_config.gpio.oe = PANEL_PIN_OE;

    display = MatrixPanel_I2S_DMA(mx_config);
    display.begin();
    display.setBrightness(255);
    display.clearScreen();
    display.setLatBlanking(2);
}

const uint16_t black = display.color565(0, 0, 0);
const uint16_t white = display.color565(255, 255, 255);
const uint16_t green = display.color565(50, 200, 50);
const uint16_t red = display.color565(255, 100, 100);
const uint16_t yellow = display.color565(246, 211, 45);
const uint16_t s4_color = display.color565(160, 21, 60);
const uint16_t grey = display.color565(150, 150, 150);
const uint16_t dark_grey = display.color565(50, 50, 50);
const uint16_t orange = display.color565(222, 123, 46);

const char departure_dbg_print_fmt[] PROGMEM = "In %d (+%d) min.\t(Gleis %s)\t%s\tto %s\n";

char info_text[100] = "";

static void replace_umlauts(char* str, size_t length) {
    String s(str);
    s.replace("Ä", "Ae");
    s.replace("Ö", "Oe");
    s.replace("Ü", "Ue");
    s.replace("ä", "ae");
    s.replace("ö", "oe");
    s.replace("ü", "ue");
    strncpy(str, s.c_str(), length);
}

static void shorten_direction(char* str, size_t length) {
    String s(str);
    s.replace("Ä", "A");
    s.replace("Ö", "O");
    s.replace("Ü", "U");
    s.replace("ä", "ae");
    s.replace("ö", "oe");
    s.replace("ü", "ue");
    s.replace("Karlsruhe", "KA"); // "Karlsruhe" -> "KA"
    s.replace("Hauptbahnhof", "Hbf");
    strncpy(str, s.c_str(), length);
}

struct KVVDeparture {
    int16_t countdown;
    int16_t delay;
    char platform[5];
    char number[10];
    char direction[30];

    void parse(JsonObject dep) {
        countdown = atoi(dep["countdown"]);
        strncpy(platform, dep["platform"], sizeof(platform) - 1);

        auto line = dep["servingLine"];
        strncpy(number, line["number"], sizeof(number) - 1);
        strncpy(direction, line["direction"], sizeof(direction) - 1);
        shorten_direction(direction, sizeof(direction) - 1);

        if(line.containsKey("delay"))
            delay = atoi(line["delay"]);
        else
            delay = 0;

        if(dep.containsKey("lineInfos") && strlen(info_text) == 0) {
            strncpy(info_text, dep["lineInfos"]["lineInfo"]["infoText"]["subtitle"], sizeof(info_text) - 1);
            replace_umlauts(info_text, sizeof(info_text) - 1);
        }
    }

    void dbg_print() {
        Serial.printf(departure_dbg_print_fmt, countdown, delay, platform, number, direction);
    }

    void show() {
        display.setCursor(0, 0);
        display.setTextColor(white);

        if(countdown == 0)
            display.print("sofort");
        else
            display.print(countdown);
        
        switch(delay) {
            case 0:
                display.setTextColor(green);
                display.print("(+0)");
                break;
            case -9999:
                display.setTextColor(red);
                display.print('*');
                break;
            default:
                display.setTextColor(yellow);
                display.printf("(+%u)", delay);
                break;
        }

        display.setTextColor(white);
        if(countdown)
            display.print(" min");

        display.setCursor(0, 8);
        
        if(strcmp(number, "S4") == 0)
            display.fillRect(0, 8, 12, 8, s4_color);
        else
            display.fillRect(0, 8, 6 * strlen(number), 8, dark_grey);

        display.printf("%s ", number);

        display.setTextColor(grey);
        if(strlen(direction) <= 10) {
            display.setCursor(0, 16);
            display.print(direction);
        }
        else {
            display.setTextWrap(true);
            display.print(direction);
            display.setTextWrap(false);
        }
    }
};

const char request_fmt[] PROGMEM = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST?outputFormat=JSON&coordOutputFormat=WGS84[dd.ddddd]&depType=stopEvents&locationServerActive=1&mode=direct&name_dm=%u&type_dm=stop&useOnlyStops=1&useRealtime=1&limit=%u";

char request_url[250];
KVVDeparture departure_list[N_REQUESTED] = {};
bool redraw_departures = false;

StaticJsonDocument<MAX_JSON_SIZE> doc;

static bool update_departures(void) {
    uint32_t station_id = 7001530;

    memset(request_url, 0, sizeof(request_url));
    snprintf(request_url, sizeof(request_url) - 1, request_fmt, station_id, N_REQUESTED);
    
    HTTPClient http;
    http.begin(request_url);

    int response_code = http.GET();
    
    if(response_code <= 0) {
        Serial.print("Error code: ");
        Serial.println(response_code);
        return false;
    }

    Serial.print("Response code: ");
    Serial.println(response_code);

    String json = http.getString();
    auto departureIdx = json.lastIndexOf("departureList");
    json.remove(0, departureIdx - 2);
    json[0] = '{';
    // Serial.println(json);
    
    auto error = deserializeJson(doc, json);
    if(error) {
        Serial.print("Deserialization error: ");
        Serial.println(error.f_str());
        return false;
    }

    memset(info_text, 0, sizeof(info_text));
    auto departures = doc["departureList"];
    for(uint8_t i = 0; i < N_REQUESTED; i++) {
        departure_list[i].parse(departures[i]);
    }

    http.end();

    Serial.println("parsed.");

    redraw_departures = true;
    last_update_time = millis();
    return true;
}

static void display_page_indicator(void) {
    for(int i = 0; i < N_REQUESTED; i++) {
        int width = PANEL_WIDTH / N_REQUESTED / 3;
        int pos = i * PANEL_WIDTH / N_REQUESTED + width;

        display.writeLine(pos, PANEL_HEIGHT - 1, pos + width, PANEL_HEIGHT - 1, i == current_page ? white : dark_grey);
       // display.writePixel(i * PANEL_WIDTH / N_REQUESTED, PANEL_HEIGHT - 1, i == current_page ? white : dark_grey); 

    }
}

TaskHandle_t update_task_handle;
static void update_task(void* _param) {
    for(;;) {
        if(millis() - last_page_switch_time > PAGE_SWITCH_INTERVAL) {
            last_page_switch_time = millis();
            current_page = (current_page + 1) % N_REQUESTED;
            redraw_departures = true;
        }
        if(redraw_departures) {
            redraw_departures = false;
            display.clearScreen();
            departure_list[current_page].show();
            last_scroll_time = 0;
        }
        if(millis() - last_scroll_time > SCROLL_INTERVAL) {
            last_scroll_time = millis();
            int32_t text_len = strlen(info_text);
            if(text_len > 0) {
                display.setCursor(scroll_offset--, 24);
                display.fillRect(0, PANEL_HEIGHT - 8, PANEL_WIDTH, 8, black);

                display_page_indicator();

                display.setTextColor(orange);
                display.print(info_text);

                if(scroll_offset < text_len * -6)
                    scroll_offset = PANEL_WIDTH;
            }
        }
    }
}

void setup() {
    Serial.begin(115200);

    init_display();

    display.setTextColor(white);
    display.clearScreen();
    display.setTextWrap(false);

    display.setCursor(0, 0);
    display.write("connecting...");

    WiFi.begin(WIFI_SSID, WIFI_PASSWD);
    WiFi.mode(WIFI_MODE_STA);

    Serial.print("connecting [");

    while(WiFi.status() != WL_CONNECTED) {
        delay(10);
        Serial.print('.');
    }
    Serial.println(']');
    Serial.println(WiFi.localIP().toString());

    display.clearScreen();
    display.setCursor(0, 0);
    display.write("updating...");

    while(!update_departures());

    xTaskCreatePinnedToCore(update_task, "update_task", 10000, NULL, 1, &update_task_handle, 1);

    for(auto& dep : departure_list) {
        dep.dbg_print();
    }
    Serial.printf("Info: %s\n", info_text);
    
    Serial.println("done.");
}

void loop() {
    if(millis() - last_update_time > UPDATE_INTERVAL) {
        last_update_time = millis();
        Serial.println("Updating...");
        //while(!update_departures());
        update_departures();
    }
}
