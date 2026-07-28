#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <time.h>

#include <Arduino.h>
#include <U8x8lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif

//your networks name and password
const char* ssid = "YourWiFi";
const char* password = "YourPassword";

//defining SPI pins for OLED display
U8X8_SH1106_128X64_NONAME_4W_SW_SPI u8x8(
    /* clock=*/2,
    /* data=*/3,
    /* cs=*/6,
    /* dc=*/5,
    /* reset=*/4
);

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
const int max_no_of_states = 3; 

//struct to hold time data;
struct tm timeinfo;

// Stopwatch variables
unsigned long startTime = 0;
unsigned long elapsedTime = 0;
bool isRunning = false;



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
    //ISR pin configurations
    pinMode(state_button  , INPUT_PULLUP);
    attachInterrupt(state_button   , state_select, RISING);

    pinMode(select_button  , INPUT_PULLUP);
    attachInterrupt(select_button   , option_select, RISING);

    //setting the BAUD rate 
    Serial.begin(115200);

    
    u8x8.begin();
    u8x8.setPowerSave(0);


    //date and time configuration
    WiFi.begin(ssid, password);

    unsigned long start = millis();

    while(WiFi.status()!=WL_CONNECTED)
    {
        if(millis()-start > 15000)          //exits error code after 15s 
        {
            displayError("WiFi","Failed");
            break;
        }

        delay(500);
    }

    configTime(2 * 3600, 0, "pool.ntp.org");   // UTC+2 for South Africa

}
  

void loop() {
  // put your main code here, to run repeatedly:
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

delay(200);

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
    u8x8.setInverseFont(1);
    u8x8.drawString(0, 0, title);
    u8x8.setInverseFont(0);

    // Message
    u8x8.setFont(u8x8_font_chroma48medium8_r);
    u8x8.drawString(0, 2, message);

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

    return weather;
}



void timer(){

        char stop_time[20]; 

    if (selected == true &&  isRunning == false){
        isRunning = true;       //start the timer 
        startTime = millis();   //save the current time
        selected = false;       

    }else if(selected == false &&  isRunning == true){
        //display the the time  
        sprintf(stop_time,"%lu s", millis()/1000);

        u8x8.clearDisplay();
        u8x8.drawString(0,0,stop_time);

    }else if(selected == true && isRunning == true){
        elapsedTime = millis() - startTime;         //record how much time has passed

        sprintf(stop_time,"%lu s", elapsedTime/1000);

        u8x8.clearDisplay();
        u8x8.drawString(0,0,stop_time);

    }else{

        u8x8.clearDisplay();
        u8x8.drawString(0,0,"0");
    }
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
}


/*
*********************************************************************************
*/

