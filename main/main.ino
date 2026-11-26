#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <NimBLEDevice.h>   
#include <time.h>

#include <Arduino.h>
#include <U8x8lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif

#include <esp_sleep.h>

//MPU-6050
#include <Wire.h>
#include <Adafruit_Sensor.h>

#include <Adafruit_ADXL345_U.h>     // replaces Adafruit_MPU6050.h

Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
bool mpu_ok = false;
bool mpuFresh = false;          // true when there is a reading not yet sent
unsigned long lastMpuRead = 0;

//your networks name and password
const char* ssid = "";
const char* password = "";

//defining SPI pins for OLED display
U8X8_SH1106_128X64_NONAME_4W_SW_SPI u8x8(
    /* clock=*/2,
    /* data=*/3,
    /* cs=*/6,
    /* dc=*/5,
    /* reset=*/4
);

const char error_name[] = "Wifi"; 
const char error_definition[] = "Failed"; 

//holds weather data
struct Weather
{
    float temperature;
    int humidity;
    float pressure;
    int weatherCode;
};
Weather weather = {0.0,0,0.0,-1};
//pins attached to push buttons
const int state_button = 10;
const int select_button = 9;

//counter to keep track of states
volatile int state_counter = 0;
//bool to keep track of menu selections
volatile bool selected = false;
//the number of states 
//start counting from 0
const int max_no_of_states = 2;         // = no. of states - 1

//struct to hold time data;
struct tm timeinfo;

// Stopwatch variables
unsigned long startTime = 0;
enum StopwatchState { SW_IDLE, SW_RUNNING, SW_STOPPED };
StopwatchState swState = SW_IDLE;

volatile bool swReset = false;      // set when you change screens
unsigned long counter = 0;

//battery setup
//battery pin
const int battery_pin = 1;
float ADC_max_voltage = 3.3;      //max ADC input voltage 
float  ADC_range = 4095.0f;     //Esp C3 has a 12 bit ADC 
float V_divider_factor = 73.0f / 50.0f  ;

#define ENABLE_BATT_LOCKOUT 0             // set to 0 when testing on USB with no battery attached, 1 enables it again

const float LOW_BATT_CUTOFF   = 3.2f;     // below this: lock out
const float RECOVER_VOLTAGE   = 3.6f;     // above this for a while: restart
const int   LOW_BATT_COUNT    = 3;        // consecutive low readings before lockout
const int   RECOVER_COUNT     = 5;        // consecutive good readings (about 10 s) before restart
unsigned long lastBattCheck   = 0;
int lowReadings = 0;

//weather setup
const unsigned long WEATHER_INTERVAL = 60UL * 60UL * 1000UL;   // 1 hour
const unsigned long WEATHER_RETRY    = 60UL * 1000UL;          // retry after 1 min on failure

unsigned long lastWeatherFetch = 0;
unsigned long weatherWait = 0;       // how long to wait before the next fetch
bool weatherFetchedOnce = false;
bool weatherOK = false;

char shown[8][17];            // what is currently on each of the 8 rows
int  last_drawn_state = -1;
int  lastSecondDrawn  = -1;

//Determines the time between window refreshes 
//Smaller delay creates smoother windows
const int Refresh_rate = 2;

//BLE defintions 
//transmitting on watch 
#define SERVICE_UUID "12345678-1234-5678-1234-56789abcdef0"
#define MPU_UUID     "12345678-1234-5678-1234-56789abcdef2"
#define BATT_UUID    "12345678-1234-5678-1234-56789abcdef3"

NimBLEServer*         pServer      = nullptr;
NimBLECharacteristic* pMpuChar  = nullptr;
NimBLECharacteristic* pBattChar = nullptr;

//Time service for syncing time between phone and watch 
//ID for time service 
#define TIME_UUID "12345678-1234-5678-1234-56789abcdef5"

//points to characteristic 
NimBLECharacteristic* pTimeChar = nullptr;

volatile bool timeNew = false;       // set by the callback, handled in loop()
volatile uint32_t timeReceived = 0;  // Unix timestamp (UTC seconds), ie no. of secnonds since UTC 0
bool timeSynced = false;             // true once the phone has set the clock


//weather setup , same procedure as Time but the data format recieved is different 
#define WEATHER_UUID "12345678-1234-5678-1234-56789abcdef6"

NimBLECharacteristic* pWeatherChar = nullptr;

char weatherBuf[32] = "";          // raw text from the phone
volatile bool weatherNew = false;  // set by the callback, handled in loop()

bool phoneWeatherOK = false;       // true once the phone has sent valid weather
unsigned long lastPhoneWeather = 0;
const unsigned long PHONE_WEATHER_VALID = 2UL * 60UL * 60UL * 1000UL;   // trust phone data for 2 h 

//notifications setup
#define RX_UUID "12345678-1234-5678-1234-56789abcdef4"

NimBLECharacteristic* pRxChar = nullptr;

char rxBuffer[21] = "";          // last message received (20 chars max)
volatile bool rxNew = false;     // set by the callback, handled in loop()



unsigned long lastMpuSend  = 0;
unsigned long lastBattSend = 0;

struct MpuData { float ax, ay, az; };
MpuData mpuData = {0, 0, 0};

//tells us if a device connected to the wacth 
volatile bool         bleConnected = false;
unsigned long         lastBleSend  = 0;
//counter is a variable used in testing, remove for final run
uint32_t              bleCounter   = 0;

//Gloabals for inactivity detection and screen control
volatile unsigned long lastActivity = 0;               // updated by the state button
const unsigned long SCREEN_TIMEOUT = 60UL * 1000UL;    // 1 minute
volatile bool screenOn = true;

//globals for displaying notifications in RH corner 
volatile int notifCount = 0;       // unseen notifications
int lastDrawnCount = -1;           // what the corner currently shows (-1 = force redraw)

// 8x8 lowercase gamma: each byte is one pixel column, lowest bit at the top
//chose gamma just cause , change it to whatever u like 
uint8_t gamma_tile[8] = {0x02, 0x04, 0x08, 0xF0, 0x08, 0x04, 0x02, 0x00};

//class with functions that decide what to do when a device connects or disconnects 
//overide tells the compiler that we will be changing the libraries defualt version
//of that function 
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        bleConnected = true;
        Serial.println("BLE connected");
    }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        bleConnected = false;
        Serial.printf("BLE disconnected, reason %d\n", reason);
        NimBLEDevice::getAdvertising()->start();   // become discoverable again
    }
};

//callback for syncing time 
class TimeCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& info) override {
        NimBLEAttValue val = pChar->getValue(); //gets the data sent 

        //liminting message size to prevent overflow 
        char buf[16];
        size_t n = val.size();
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        memcpy(buf, val.data(), n);
        buf[n] = '\0'; //terminating character 
        //converts the string sent to a number ,base 10, if not a number eg "hell" then it ignores it 
        unsigned long ts = strtoul(buf, nullptr, 10);

        // sanity check: between Nov 2023 and year 2100
        //1700000000 = Nov 2023
        //4102444800 = year 2100 if statement ignores anything outside this range 
        if (ts > 1700000000UL && ts < 4102444800UL)
        {
            //variable to store recevied variable and flag fro indicating somethign was recieved    
            timeReceived = ts;
            timeNew = true;
        }
    }
};

//notifications callback 
class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& info) override {
        NimBLEAttValue val = pChar->getValue();
        size_t n = val.size();
        if (n > sizeof(rxBuffer) - 1) n = sizeof(rxBuffer) - 1;

        memcpy(rxBuffer, val.data(), n);
        rxBuffer[n] = '\0';
        rxNew = true;
    }
};

//weather callbakc 
class WeatherCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& info) override {
        NimBLEAttValue val = pChar->getValue();

        size_t n = val.size();
        if (n > sizeof(weatherBuf) - 1) n = sizeof(weatherBuf) - 1;

        memcpy(weatherBuf, val.data(), n);
        weatherBuf[n] = '\0';
        weatherNew = true;
    }
};



/*
function protoypes 
*********************************************************************
*/

void time_state();
tm get_date_time();
void displayError(const char *title, const char *message);
bool fetch_weather();
void display_weather();
void stop_watch_state();
void weather_state();
void timer();
String weatherText(int code);
void read_mpu();
void drawPadded(uint8_t row, const char *text);
void clearShown();
void state_changed();
void ble_send();
void ble_setup();
void update_mpu();
float read_battery_voltage();
uint8_t battery_percent();
void mpu_init();
void handle_time_sync();
void handle_weather_rx();
void splash_screen();
void splash_status(const char *msg);
void screen_power_manager();
void draw_notification_indicator();
void handle_ble_rx();
void battery_guard();
void low_battery_lockout();


// writes a row padded to 16 chars, and only if the text actually changed
void drawPadded(uint8_t row, const char *text)
{
    int width = (row == 0) ? 12 : 16;    // keep cols 12-15 of row 0 free
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16s", text);

    if (strcmp(buf, shown[row]) == 0) return;   // unchanged, skip the SPI traffic
    strcpy(shown[row], buf);

    u8x8.drawString(0, row, buf);
}

void clearShown()
{
    memset(shown, 0, sizeof(shown));
}

// clears the screen once when the state changes
void state_changed()
{
    int s = state_counter;
    if (s == last_drawn_state) return;

    last_drawn_state = s;
    lastSecondDrawn  = -1;      // force the clock to redraw
    u8x8.clearDisplay();
    clearShown();
    lastDrawnCount = -1;     // the screen was wiped, so redraw the corner
}

/*
ISR's
*********************************************************************
*/

//state_button ISR
//triggered when user decides to move from one state 
//another and also triggers the screen to turn back on 
void IRAM_ATTR state_select(){
    lastActivity = millis();
    if (screenOn) notifCount = 0;

    state_counter++;
    if(state_counter > max_no_of_states)
        state_counter = 0;

    swReset  = true;                // leaving the screen resets the stopwatch
    selected = false;
}

//select button ISR 
//triggered when an menu option is selected 
void IRAM_ATTR option_select(){
    lastActivity = millis();        // a press also keeps the screen awake
    selected = true;
}

/*
***********************************************************************
*/


void setup() {
  // put your setup code here, to run once:
    //setting the BAUD rate 
    Serial.begin(115200);
    
    // Serial.println("In setup");              //debugging line 

    //ISR pin configurations
    pinMode(state_button  , INPUT_PULLUP);
    attachInterrupt(state_button   , state_select, RISING);

    pinMode(select_button  , INPUT_PULLUP);
    attachInterrupt(select_button   , option_select, RISING);
    
       u8x8.begin();
    u8x8.setPowerSave(0);

    //if the battery voltage is too low , dont even start 
    #if ENABLE_BATT_LOCKOUT
        if (read_battery_voltage() < LOW_BATT_CUTOFF)
            low_battery_lockout();               // never start the radios on a flat battery
    #endif

    splash_screen();                 // appears immediately on power-up
    splash_status("Starting...");

    //ADXL setup
    splash_status("Sensor...");
    Wire.begin(8, 7);
    Wire.setClock(100000);
    Wire.setTimeOut(50);
    mpu_init();

    //setting up BLE
    splash_status("Bluetooth...");
    ble_setup();

    WiFi.begin(ssid , password);
    configTime(2 * 3600, 0, "pool.ntp.org");
    setenv("TZ", "SAST-2", 1);
    tzset();

    splash_status("Ready");
    delay(3000);                     // hold the splash so it's readable
    lastActivity = millis();        
}
  

void loop() {
  // put your main code here, to run repeatedly:

battery_guard();
update_mpu();
ble_send();
handle_time_sync();
handle_weather_rx();
handle_ble_rx();

screen_power_manager();

if (screenOn)
{
    switch(state_counter)
    {
        case 0: time_state();       break;
        case 1: weather_state();    break;
        case 2: stop_watch_state(); break;
    }
    draw_notification_indicator();
}

delay(Refresh_rate);

}


/*  
*************************************************************************
*/

//function to get the  time over wifi and display to the OLED 
tm get_date_time()
{
    if (getLocalTime(&timeinfo, 50) && timeinfo.tm_sec != lastSecondDrawn)
    {
        lastSecondDrawn = timeinfo.tm_sec;

        char hm[6];            // HH:MM
        char ss[3];            // SS
        char dateString[20];

        snprintf(hm, sizeof(hm), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        snprintf(ss, sizeof(ss), "%02d", timeinfo.tm_sec);
        strftime(dateString, sizeof(dateString), "%a %d %b", &timeinfo);

        // large HH:MM on rows 1-4, centred
        u8x8.setFont(u8x8_font_inr21_2x4_r);
        u8x8.drawString(3, 1, hm);

        // seconds on row 5, date on row 7, small font
        u8x8.setFont(u8x8_font_chroma48medium8_r);
        u8x8.drawString(7, 5, ss);
        u8x8.drawString(3, 7, dateString);
    }

    return timeinfo;
}

//function to dislpay error messages
void displayError(const char *title, const char *message)
{
    u8x8.clearDisplay();

    // Error title (inverse text)
    u8x8.setFont(u8x8_font_chroma48medium8_r);
    u8x8.drawString(0, 0, title);

    // Message
    // u8x8.setFont(u8x8_font_chroma48medium8_r);
    u8x8.setInverseFont(1);
    u8x8.drawString(0, 1, message);
    u8x8.setInverseFont(0);

    u8x8.refreshDisplay();

}

   //determing the weeather forecast using the weather_code
    String weatherText(int code)
{
    switch(code)
    {
        case 0: return "Clear";
        case 1: return "Mainly Clear";
        case 2: return "Partly Cloudy";
        case 3: return "Cloudy";

        case 45:
        case 48:
            return "Fog";

        case 51:
        case 53:
        case 55:
            return "Drizzle";

        case 61:
        case 63:
        case 65:
            return "Rain";

        case 71:
        case 73:
        case 75:
            return "Snow";

        case 95:
            return "Storm";

        default:
            return "Unknown";
    }
}


//function to get current weather coinditions and display them
//change the location via longitude and latitude whif you change cities or something
//only talks to the API, no drawing
bool fetch_weather()
{
    //reconnecting to network
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.reconnect();

        // give it a few seconds to come back
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 5000)
            delay(100);

        if (WiFi.status() != WL_CONNECTED)
            return false;
    }

    HTTPClient http;

    String url =
    "https://api.open-meteo.com/v1/forecast?"
    "latitude=-25.7545&"
    "longitude=28.2314&"
    "current=temperature_2m,"
    "relative_humidity_2m,"
    "pressure_msl,"
    "weather_code";

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK)
    {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload))
        return false;

    weather.temperature = doc["current"]["temperature_2m"];
    weather.humidity    = doc["current"]["relative_humidity_2m"];
    weather.pressure    = doc["current"]["pressure_msl"];
    weather.weatherCode = doc["current"]["weather_code"];

    return true;
}

//draws the stored weather data plus battery, no network
void display_weather()
{
    u8x8.setFont(u8x8_font_chroma48medium8_r);

    if (!weatherOK)
    {
        drawPadded(0, "No weather");
        drawPadded(2, "data yet");
    }
    else
    {
        char line[17];

        snprintf(line, sizeof(line), "%.1f C", weather.temperature);
        drawPadded(0, line);

        snprintf(line, sizeof(line), "Hum %d%%", weather.humidity);
        drawPadded(2, line);

        drawPadded(4, weatherText(weather.weatherCode).c_str());
    }
    //display the batteries percentage in the weather screen , --looks good so might leave it here ?--
    char batt[17];
    snprintf(batt, sizeof(batt), "Bat %d%%", battery_percent());
    drawPadded(6, batt);
}


void timer()
{
    char buf[17];

    // leaving the screen resets the stopwatch
    if (swReset) { swReset = false; swState = SW_IDLE; selected = false; }

    if (selected)
    {
        selected = false;
        switch (swState)
        {
            case SW_IDLE:                       // press 1: start
                swState = SW_RUNNING;
                startTime = millis();
                break;
            case SW_RUNNING:                    // press 2: stop
                counter = (millis() - startTime) / 1000;
                swState = SW_STOPPED;
                break;
            case SW_STOPPED:                    // press 3: reset to zero
                swState = SW_IDLE;
                break;
        }
    }

    if (swState == SW_RUNNING)
        counter = (millis() - startTime) / 1000;

    snprintf(buf, sizeof(buf), "%lu s", swState == SW_IDLE ? 0UL : counter);
    drawPadded(0, buf);
}

//MPU initialization 
void mpu_init()
{
    mpu_ok = accel.begin(0x53);

    if (mpu_ok)
    {
        accel.setRange(ADXL345_RANGE_4_G);
        accel.setDataRate(ADXL345_DATARATE_100_HZ);
    }
}

//the MPU functino below goes in the stopwatch state
// reads the sensor into mpuData, called every loop, throttled to 10 Hz
void update_mpu()
{
    // not initialised: retry every 2 s
    if (!mpu_ok)
    {
        if (millis() - lastMpuRead < 2000) return;
        lastMpuRead = millis();
        mpu_init();
        return;
    }

    if (millis() - lastMpuRead < 100) return;
    lastMpuRead = millis();

    sensors_event_t event;
    accel.getEvent(&event);

    mpuData.ax = event.acceleration.x;
    mpuData.ay = event.acceleration.y;
    mpuData.az = event.acceleration.z;
    mpuFresh = true;
}

// only draws the stored values, no sensor access
void read_mpu()
{
    if (!mpu_ok)
    {
        drawPadded(2, "Accel not found");
        return;
    }

    char line[17];
    snprintf(line, sizeof(line), "AX %6.2f", mpuData.ax);  drawPadded(2, line);
    snprintf(line, sizeof(line), "AY %6.2f", mpuData.ay);  drawPadded(3, line);
    snprintf(line, sizeof(line), "AZ %6.2f", mpuData.az);  drawPadded(4, line);
}
/*
**************************************************************************

/*
State functions
**********************************************************************
*/


//state to retreive and display the date and time
void time_state(){
    state_changed();
    get_date_time();
}

//function to get and retrieve the current weather conditions
//prioritises the phone connection 
void weather_state(){
    state_changed();

    // phone data counts as fresh for 2 hours
    bool phoneFresh = phoneWeatherOK &&
                      (millis() - lastPhoneWeather < PHONE_WEATHER_VALID);

    // only use WiFi if the phone hasn't supplied recent data
    if (!phoneFresh &&
        (!weatherFetchedOnce || (millis() - lastWeatherFetch >= weatherWait)))
    {
        weatherFetchedOnce = true;
        lastWeatherFetch = millis();

        bool ok = fetch_weather();
        weatherOK   = ok || weatherOK;               // keep old data if the refresh fails
        weatherWait = ok ? WEATHER_INTERVAL : WEATHER_RETRY;
    }

    display_weather();
}

//function to implement stopwatch 
void stop_watch_state(){
    state_changed();
    u8x8.setFont(u8x8_font_chroma48medium8_r);
    timer();
    read_mpu();
}

/*
*********************************************************************************
Power management functions functions 
*/


//function for seting up the bluetooth connection to another device 
void ble_setup()
{
    //this sets the name that will appear in scans , change if appropriate 
    NimBLEDevice::init("ESP32-Watch");

    pServer = NimBLEDevice::createServer(); //creating the server (establishing watch as server)
    pServer->setCallbacks(new ServerCallbacks()); //allowing connect / disconect events to reach our code 

    //making the service with the associated UUID
    NimBLEService* pService = pServer->createService(SERVICE_UUID);

    //creating the MPU service, its read and notify only to the phone 
    pMpuChar = pService->createCharacteristic(
        MPU_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    //creating a battery service 
    pBattChar = pService->createCharacteristic(
        BATT_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    //time service is writeable for both types of device write normal and without acnkowlegdment  
    //
    pTimeChar = pService->createCharacteristic(
        TIME_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pTimeChar->setCallbacks(new TimeCallbacks());

    //weather service 
        pWeatherChar = pService->createCharacteristic(
        WEATHER_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pWeatherChar->setCallbacks(new WeatherCallbacks());

    //notifications  service 
    pRxChar = pService->createCharacteristic(
        RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxChar->setCallbacks(new RxCallbacks());

    // initial value so a READ before the first notify isn't empty
    uint8_t b = battery_percent();
    pBattChar->setValue(&b, 1);

    //starting the service 
    pService->start();

    //broadcasting the watches Bluetooth , this is how the phone can find 
    //it at any time, UUID allows it to keep track of the watch at any point too 
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->start();
}

//sends data over bluetooth 
void ble_send()
{
    if (!bleConnected) return;

    // MPU: 6 x int16 = 12 bytes, at most every 100 ms and only with fresh data
    if (mpuFresh && millis() - lastMpuSend >= 100)
    {
        lastMpuSend = millis();
        mpuFresh = false;

        //each peice of MPU data is stored as a 16 bit integer 
        int16_t packet[3] = {
            (int16_t)(mpuData.ax * 100),
            (int16_t)(mpuData.ay * 100),
            (int16_t)(mpuData.az * 100)
        };

        pMpuChar->setValue((uint8_t*)packet, sizeof(packet));
        pMpuChar->notify();
    }

    // Battery: 1 byte percent, every 5 s
    if (millis() - lastBattSend >= 5000)
    {
        lastBattSend = millis();

        uint8_t pct = battery_percent();
        pBattChar->setValue(&pct, 1);  //only 1 byte for the battery percentage 
        pBattChar->notify();
    }
}


//handles time received 
void handle_time_sync()
{
    if (!timeNew) return;
    timeNew = false;

    //stucture to hold seconds and micro-seconds but we stick to seconds 
    struct timeval tv;
    tv.tv_sec  = timeReceived;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr); //sets the time for the board based on the received data 

    //has the phone set the time before-flag 
    timeSynced = true;
    lastSecondDrawn = -1;      // force the clock to redraw immediately
    Serial.printf("Time set from phone: %lu\n", (unsigned long)timeReceived);
}

//receives wether data 
void handle_weather_rx()
{
    if (!weatherNew) return;
    weatherNew = false;

    // parse "temp,humidity,code"
    char* p;
    float t = strtof(weatherBuf, &p);
    if (*p != ',') { Serial.println("Weather: bad format"); return; }

    int h = strtol(p + 1, &p, 10);
    if (*p != ',') { Serial.println("Weather: bad format"); return; }

    int c = strtol(p + 1, &p, 10);

    // sanity checks
    if (t < -60 || t > 60 || h < 0 || h > 100 || c < 0 || c > 99)
    {
        Serial.println("Weather: values out of range");
        return;
    }

    weather.temperature = t;
    weather.humidity    = h;
    weather.weatherCode = c;

    weatherOK        = true;
    phoneWeatherOK   = true;
    lastPhoneWeather = millis();

    Serial.printf("Weather from phone: %.1f C, %d%%, code %d\n", t, h, c);
}

//receives notifications and increment 
void handle_ble_rx()
{
    if (!rxNew) return;
    rxNew = false;

    notifCount++;
    lastActivity = millis(); //alllows for notification to wake screen , does cost battery life a bit though so up to u 
    Serial.printf("BLE RX: %s (unseen: %d)\n", rxBuffer, (int)notifCount); //printing no of notifications to serial 
}

//adding start sreen and progress text along screens so we know where 
//we are when the watch is turned on 
// shows the watch name, then a status line that setup() updates
void splash_screen()
{
    u8x8.clearDisplay();
    u8x8.setFont(u8x8_font_chroma48medium8_r);

    u8x8.draw2x2String(3, 1, "Gracy");   // 5 chars = 10 tiles, centred at col 3
    u8x8.draw2x2String(3, 3, "Tereze");
}

// overwrites the bottom row with a status message
void splash_status(const char *msg)
{
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16s", msg);   // pad so old text is wiped
    u8x8.setFont(u8x8_font_chroma48medium8_r);
    u8x8.drawString(0, 7, buf);
}

//controls when the screen should be on and off based on its time on  
void screen_power_manager()
{
    //has a minute passed yet , if no then the screen should be on 
    bool shouldBeOn = (millis() - lastActivity < SCREEN_TIMEOUT);

    if (!shouldBeOn && screenOn) //if the screen should be off 
    {
        u8x8.setPowerSave(1);       // display off
        screenOn = false;           
    }
    else if (shouldBeOn && !screenOn)
    {
        u8x8.setPowerSave(0);       // display on
        screenOn = true;
        last_drawn_state = -1;      // force a full clear and redraw
    }
}

//displays the notification symbol and the number of notifications 
//gets capped at 99 notifications 
void draw_notification_indicator()
{
    int n = notifCount;
    if (n == lastDrawnCount) return;      // nothing changed, skip the SPI traffic
    lastDrawnCount = n;

    u8x8.setFont(u8x8_font_chroma48medium8_r);
    u8x8.drawString(12, 0, "    ");       // wipe cols 12-15

    if (n > 0)
    {
        char txt[5];
        snprintf(txt, sizeof(txt), "+%d", n > 99 ? 99 : n);

        int col = 16 - (int)strlen(txt) - 1;     // right-aligned: gamma + text
        u8x8.drawTile(col, 0, 1, gamma_tile);
        u8x8.drawString(col + 1, 0, txt);
    }
}

//improved reading using the millivolts versoin of analogRead
float read_battery_voltage()
{
    long sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(battery_pin);
    float pin_volts = (sum / 16.0f) / 1000.0f;
    return pin_volts * V_divider_factor;       // 73/50: back to the real battery voltage
}


//gives the battery percentage , using 3.3V as 0% and 3.7 as 100%
uint8_t battery_percent()
{
    float p = ((read_battery_voltage() - LOW_BATT_CUTOFF) / (3.7f - LOW_BATT_CUTOFF)) * 100.0f;
    return (uint8_t)constrain(p, 0.0f, 100.0f);
}


//disables the watches functions BLE and wifi until watch is charged 
void low_battery_lockout()
{
    // everything that draws power goes off
    NimBLEDevice::deinit(true);  //BLE off
    WiFi.disconnect(true);   //wifi goes off 
    WiFi.mode(WIFI_OFF);
    detachInterrupt(state_button);  //remove button interrupts 
    detachInterrupt(select_button);
    Wire.end();                     //no SPI
    setCpuFrequencyMhz(80);         //slow the system clock down all to reduce I-draw 

    // the only things left running: the message and the LED
    u8x8.setPowerSave(0);
    u8x8.clearDisplay();
    u8x8.setFont(u8x8_font_chroma48medium8_r);
    u8x8.drawString(2, 3, "LOW BATTERY");
    u8x8.drawString(1, 5, "Please charge");

    pinMode(LED_BUILTIN, OUTPUT);
    int goodReadings = 0;

    while (true)
    {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(200);
        digitalWrite(LED_BUILTIN, LOW);
        delay(1800);                          // one short flash every 2 s

        // has it been charged? needs a sustained good reading, not one spike
        if (read_battery_voltage() > RECOVER_VOLTAGE)
        {
            if (++goodReadings >= RECOVER_COUNT)
                ESP.restart();                // reboots straight into normal operation
        }
        else
        {
            goodReadings = 0;
        }
    }
}


//decides when to lock the device based on the battery voltage 
void battery_guard()
{
#if ENABLE_BATT_LOCKOUT
    if (millis() - lastBattCheck < 1000) return;      // once a second
    lastBattCheck = millis();

    if (read_battery_voltage() < LOW_BATT_CUTOFF)
    {
        if (++lowReadings >= LOW_BATT_COUNT)
            low_battery_lockout();
    }
    else
    {
        lowReadings = 0;
    }
#endif
}

/*
*********************************************************************************
*/

