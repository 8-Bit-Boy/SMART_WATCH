#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <time.h>

#include <Arduino.h>
#include <U8x8lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif

#include <esp_sleep.h>

//MPU-6050
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

Adafruit_MPU6050 mpu;
bool mpu_ok = false;

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
unsigned long elapsedTime = 0;
bool isRunning = false;
unsigned long counter = 0;

//battery pins 
const int battery_pin = 1;
float voltage  = 0.0;
float ADC_value = 0;
float ADC_max_voltage = 3.3;      //max ADC input voltage 
float op_voltage = 3.2;         //smallest safe  possible operating voltage 
float  blackout_voltage = 3.0;  //below this voltage ,  the device should turn off
int battery_flash_delay = 200;  //delay between LED flashes to indicate LOW battery 
float  ADC_range = 4095.0f;     //Esp C3 has a 12 bit ADC 
float V_divider_factor = 73.0f / 50.0f  ;
bool batteryLow = false;

//Determines the time between window refreshes 
//Smaller delay creates smoother windows
const int Refresh_rate = 2;

/*
function declarations
*********************************************************************
*/

void time_state();
tm get_date_time();
void displayError(const char *title, const char *message);
Weather getWeather();
void stop_watch_state();
void weather_state();
void timer();
String weatherText(int code);
float  monitor_battery();
void Low_battery_flash();
void read_mpu();


/*
ISR's
*********************************************************************
*/

//state_button ISR
//triggered when user decides to move from one state 
//another 
void IRAM_ATTR state_select(){
    state_counter++;

    if(state_counter > max_no_of_states)
        state_counter = 0;

    //clear flags when transitioning states 
    //for the alarm state
    isRunning  = false;
    selected  = false;
}

//select button ISR 
//triggered when an menu option is selected 
void IRAM_ATTR option_select(){
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

    //MPU-setup 
    Wire.begin(8, 7);   // SDA = GPIO 8, SCL = GPIO 7
    mpu_ok = mpu.begin(0x68, &Wire);

    if (mpu_ok)
    {
        mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
        mpu.setGyroRange(MPU6050_RANGE_500_DEG);
        mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

    WiFi.begin(ssid , password);

    unsigned long start = millis();

    while(WiFi.status()!=WL_CONNECTED)
    {
        if(millis()-start > 15000)          //exits error code after 15s 
        {
            Serial.println("Wifi error!");
            displayError(error_name , error_definition);
            break;
        }

        delay(500);
    }

    configTime(2 * 3600, 0, "pool.ntp.org");   // UTC+2 for South Africa

}
  

void loop() {
  // put your main code here, to run repeatedly:

// Low_battery_flash();
//Debugging line
// Serial.println("In the loop");

switch(state_counter)
{
    case 0:
        time_state();
        break;

    case 1:
        weather_state();
        break;


    case 2:
        stop_watch_state();
        break;
}

delay(Refresh_rate);

}


/*  
*************************************************************************
*/

//function to get the  time over wifi and display to the OLED 
tm get_date_time()
{
    if (getLocalTime(&timeinfo))
    {
        char timeString[9];   // HH:MM:SS
        char dateString[20];  // Sat 19 Jul

        sprintf(timeString, "%02d:%02d:%02d",
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec);

        strftime(dateString, sizeof(dateString), "%a %d %b", &timeinfo);

        u8x8.clearDisplay();

        // Large time
        u8x8.setFont(u8x8_font_inr21_2x4_r);
        u8x8.drawString(1, 0, timeString);

        // Date
        u8x8.setFont(u8x8_font_chroma48medium8_r);
        u8x8.drawString(2, 6, dateString);

        u8x8.refreshDisplay();
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
Weather getWeather()
{


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
    //error check 
    if(httpCode != HTTP_CODE_OK)
    {
        displayError("Weather","No Internet");
        http.end();
        //displaying the battery percentage even when  no internet 
        float batt_percent = ((monitor_battery() - blackout_voltage) / (3.7f - blackout_voltage)) * 100.0f;
        batt_percent = constrain(batt_percent, 0.0f, 100.0f);   // clamp so it never shows over 100%

        char batt[20];
        sprintf(batt, "Bat %.0f%%", batt_percent);

        u8x8.drawString(0, 6, batt);
        return weather;
    }

    if (httpCode == 200)
    {
        String payload = http.getString();

        JsonDocument doc;

        DeserializationError err = deserializeJson(doc, payload);
    //error check the json payload
    if(err)
        {
            displayError("JSON Error", err.c_str());
            http.end();
            return weather;
        }

        weather.temperature =
            doc["current"]["temperature_2m"];

        weather.humidity =
            doc["current"]["relative_humidity_2m"];

        weather.pressure =
            doc["current"]["pressure_msl"];

        weather.weatherCode =
            doc["current"]["weather_code"];
    }

    http.end();


    char temp[20];
    char hum[20];
    char forecast[20];

    sprintf(temp, "%.1f C", weather.temperature);
    sprintf(hum, "Hum %d%%", weather.humidity);
    sprintf(forecast, "%s", weatherText(weather.weatherCode).c_str());

    u8x8.clearDisplay();
    u8x8.drawString(0,0,temp);
    u8x8.drawString(0,2,hum);
    u8x8.drawString(0,4,forecast);

    //displaying the battery percentage
    float batt_percent = ((monitor_battery() - blackout_voltage) / (3.7f - blackout_voltage)) * 100.0f;
    batt_percent = constrain(batt_percent, 0.0f, 100.0f);   // clamp so it never shows over 100%

    char batt[20];
    sprintf(batt, "Bat %.0f%%", batt_percent);

    u8x8.drawString(0, 6, batt);

    return weather;
}


void timer(){

    char time[20]; 

    if (selected == true &&  isRunning == false){
        isRunning = true;       //start the timer 
        startTime = millis();   //save the current time
        selected = false;       //clear select flag   

    }else if(selected == false &&  isRunning == true){
        //display the the time as it counts 
        counter  = millis()/1000;           //save current time(s)
        sprintf(time,"%lu s", counter);

        u8x8.clearDisplay();
        u8x8.drawString(0,0,time);

    }else if(selected == true && isRunning == true){
        elapsedTime = counter - startTime/1000;         //record how much time has passed(s)

        sprintf(time,"%lu s", elapsedTime);

        u8x8.clearDisplay();
        u8x8.drawString(0,0,time);

    }else{
        //Displays zero in case where both are false 
        u8x8.clearDisplay();
        u8x8.drawString(0,0,"0");   
    }
}

//the MPU functino below goes in the stopwatch state
//reads the MPU6050 and displays it under the stopwatch time
void read_mpu()
{
    u8x8.setFont(u8x8_font_chroma48medium8_r);

    if (!mpu_ok)
    {
        u8x8.drawString(0, 2, "MPU not found");
        return;
    }

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    char line[17];

    // Acceleration (m/s^2)
    snprintf(line, sizeof(line), "AX %6.2f", a.acceleration.x);
    u8x8.drawString(0, 2, line);
    snprintf(line, sizeof(line), "AY %6.2f", a.acceleration.y);
    u8x8.drawString(0, 3, line);
    snprintf(line, sizeof(line), "AZ %6.2f", a.acceleration.z);
    u8x8.drawString(0, 4, line);

    // Rotation (rad/s)
    snprintf(line, sizeof(line), "GX %6.2f", g.gyro.x);
    u8x8.drawString(0, 5, line);
    snprintf(line, sizeof(line), "GY %6.2f", g.gyro.y);
    u8x8.drawString(0, 6, line);
    snprintf(line, sizeof(line), "GZ %6.2f", g.gyro.z);
    u8x8.drawString(0, 7, line);
}

/*
**************************************************************************

/*
State functions
**********************************************************************
*/


//state to retreive and display the date and time
void time_state(){
get_date_time();
}

//function to get and retrieve the current weather conditions
void weather_state(){
getWeather();
}


//function to implement stopwatch 
void stop_watch_state(){
timer();
    read_mpu();
}


/*
*********************************************************************************
Power management functions functions 
*/



float monitor_battery(){
    //read voltage of the battery 
    ADC_value = analogRead(battery_pin);
    //convert to voltage 
    ::voltage = ((ADC_value/ADC_range)*(ADC_max_voltage))*V_divider_factor;

    if(voltage >= op_voltage){
        u8x8.setPowerSave(0);
        return voltage;
        
    }else if(voltage > blackout_voltage) {

        //flash LED to indicate reduced battery voltage 
        Low_battery_flash();
        ::batteryLow = true;
        return voltage;

    }else {
        u8x8.clearDisplay();
        u8x8.drawString(0, 3, "LOW BATTERY");

        delay(3000);

        u8x8.setPowerSave(1);

        // Sleep until the user charges the watch or presses a button
        // esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 1);
        // esp_deep_sleep_start();
        return voltage;
    }
}

void Low_battery_flash(){
    if(batteryLow){
    //flash the LED 
    digitalWrite(BUILTIN_LED, HIGH); 
    delay(battery_flash_delay);

    digitalWrite(BUILTIN_LED, LOW); 
    delay(battery_flash_delay);
    
    digitalWrite(BUILTIN_LED, HIGH); 
    delay(battery_flash_delay);
    
    digitalWrite(BUILTIN_LED, LOW); 
    delay(battery_flash_delay);
    
    }else {
        return;
    }
}

/*
*********************************************************************************
*/

