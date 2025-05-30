/* ***********************************************************************************************************************************************************
SOIL QUALITY SENSOR: this file includes the main code for the soil quality sensor used in Daniel Rodriguez Moya's Master Thesis. It sends data to ThingsBoard
via MQTT at a fixed frequency, measuring soil temperature and moisture using a DS18B20 and a FC-38, respectively.
*********************************************************************************************************************************************************** */

// ===========================================================================================================================================================
// INLCUSION DE LIBRERIAS
// ===========================================================================================================================================================
#include <Arduino.h>                                                                                             // Library for PlatformIO to use the Arduino environment
// Wi-Fi and MQTT libs ---------------------------------------------------------------------------------------------------------------------------------------
#include <WiFi.h>                                                                                                // Library to connect to Wi-Fi
#include <WiFiClientSecure.h>                                                                                    // Library to add TLS certificates to MQTT connection
#include <PubSubClient.h>                                                                                        // Library to connect to a MQTT broker
// ArduinoOTA libs -------------------------------------------------------------------------------------------------------------------------------------------
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
// I2C libs --------------------------------------------------------------------------------------------------------------------------------------------------
#include <Wire.h>
#include <axp20x.h>                                                                                              // Library for the PMU AXP192
// ESP32 libs ------------------------------------------------------------------------------------------------------------------------------------------------
#include <esp_sleep.h>                                                                                           // Library to send the ESP32 to sleep
#include "rom/rtc.h"                                                                                             // Library to use the ESP32 RTC memory, where I can store variable whose values survive deep sleep
// LIBRERIAS END =============================================================================================================================================

// ===========================================================================================================================================================
// MACROS (de ser necesarias)
// ===========================================================================================================================================================
// T-Beam macros ---------------------------------------------------------------------------------------------------------------------------------------------
#define LED_PIN 4
#define BUTTON_PIN GPIO_NUM_38                                                                                   // RTC pin to interrupt deep sleep
#define SDA_PIN 21
#define SCL_PIN 22
#define PMU_IRQ_PIN 35                                                                                           // PEK (PWR) button interrupt pin on T-Beam
// Serial Monitor macros -------------------------------------------------------------------------------------------------------------------------------------
#define ENABLE_SERIAL true

#if ENABLE_SERIAL                                                                                                // If set to true, the macros invoke the Serial functions
  #define Debug(x)    Serial.print(x)
  #define Debugln(x)  Serial.println(x)
  #define Debugf(...) Serial.printf(__VA_ARGS__)                                                                 // This only works with ESP32 processors, do not use it for ATMega-based boards
#else                                                                                                            // If set to false, the macros do not invoke anything and resources are saved
  #define Debug(x)
  #define Debugln(x)
  #define Debugf(...)
#endif
// MACROS END ================================================================================================================================================

// ===========================================================================================================================================================
// CONSTRUCTORES DE OBJETOS DE CLASE DE LIBRERIA, VARIABLES GLOBALES, CONSTANTES...
// ===========================================================================================================================================================
static WiFiClientSecure secureClient;                                                                            // Object of the Wi-Fi library
static PubSubClient mqttClient(secureClient);                                                                    // Object of the MQTT library
static AXP20X_Class axp;
// CONSTRUCTORES END =========================================================================================================================================

// ===========================================================================================================================================================
// GLOBAL VARIABLES
// ===========================================================================================================================================================
// Constants -------------------------------------------------------------------------------------------------------------------------------------------------
static const char* ssid = "";
static const char* password = "";
// static const char* ssid = "";
// static const char* password = "";
static const char* mqtt_server = "srv-iot.diatel.upm.es";                                                        // UPM MQTT broker
static const int mqtt_port = 8883;                                                                               // MQTT broker port
static const char* mqttTopicPub = "v1/devices/me/telemetry";
static const char* mqttTopicSub = "v1/devices/me/attributes";
static const char* access_token = "";                                                        // Unique ThingsBoard device token

static const char* root_ca = R"EOF(-----BEGIN CERTIFICATE-----

-----END CERTIFICATE-----)EOF";                                                                                  // Certificate for MQTT over TLS on Thingsboard

static const uint64_t SLEEP_DURATION_US = 30ULL * 1000000;                                                       // Sleep time between messages
// Variables -------------------------------------------------------------------------------------------------------------------------------------------------
static bool ledState = LOW;
static volatile bool pekPressed = false;
static RTC_DATA_ATTR uint32_t bootCount = 1;                                                                     // Boot counter must be stored in the RTC memory so it survives deep sleep, but not power-off
// GLOBAL VARIABLES END ======================================================================================================================================

// ===========================================================================================================================================================
// ISR
// ===========================================================================================================================================================
static void IRAM_ATTR handlePMUIRQ() {
  pekPressed = true;
}
// ISR END ===================================================================================================================================================

// ===========================================================================================================================================================
// FUNCTION PROTOTYPES
// ===========================================================================================================================================================
static void setupOTA();
static void reconnectToMQTT();
// FUNCTION PROTOTYPES END ===================================================================================================================================

// ===========================================================================================================================================================
// FREERTOS ELEMENTS
// ===========================================================================================================================================================
// Task handles ----------------------------------------------------------------------------------------------------------------------------------------------
static TaskHandle_t MQTTTaskHandle = NULL, PEKTaskHandle = NULL;
// Semaphore -------------------------------------------------------------------------------------------------------------------------------------------------
static SemaphoreHandle_t semaphoreSerial = NULL;
// Tasks -----------------------------------------------------------------------------------------------------------------------------------------------------
static void MQTTTask(void*);
static void PEKTask(void*);
// FREERTOS ELEMENTS END =====================================================================================================================================

// ===========================================================================================================================================================
// THREADS
// ===========================================================================================================================================================
// MQTT thread -----------------------------------------------------------------------------------------------------------------------------------------------
static void MQTTTask(void *pvParameters){
  while(true) {
    ArduinoOTA.handle();                                                                                           // If a new version is available, download and install it

    if(!mqttClient.connected()){                                                                                   // If no connection
      reconnectToMQTT();                                                                                           // Call reconnect function
    }
    mqttClient.loop();                                                                                             // Main MQTT function. It must run at the highest frequency and never be blocked

    if(WiFi.status() != WL_CONNECTED){
      // Connect to Wi-Fi during the execution of the thread ---------------------------------------------------------------------------------------------------
      if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
        Debug(F("Connecting to WIFI SSID "));
        Debugln(ssid);
        xSemaphoreGive(semaphoreSerial);
      }

      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      vTaskDelay(pdMS_TO_TICKS(100));
      WiFi.begin(ssid, password);

      while(WiFi.status() != WL_CONNECTED){
        vTaskDelay(pdMS_TO_TICKS(500));
        if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
          Debug(".");
          xSemaphoreGive(semaphoreSerial);
        }
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
      }

      if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
        Debugln(F(""));

        Debug(F("WiFi connected, IP address: "));
        Debugln(WiFi.localIP());
        xSemaphoreGive(semaphoreSerial);
      }

      if(ledState){
        digitalWrite(LED_PIN, LOW);
      }
      // Connect to Wi-Fi during the execution of the thread END -----------------------------------------------------------------------------------------------
    }else{                                                                                                         // Check WiFi connection status
      // MQTT Pub ----------------------------------------------------------------------------------------------------------------------------------------------
      char dataStr[256];                                                                                           // A string is created to save a JSON containing the variables and values to be published with a size of 256 characters
      float soilTemp = random(1000, 4500) / 100.0f;
      float soilMoist = random(0, 10000) / 100.0f;
      float batVolt = (axp.getBattVoltage()) / 1000.0f;

      sprintf(dataStr, "{\"bootCnt\":%lu,\"soilTemperature\":%4.2f,\"soilMoisture\":%5.2f,\"batVoltage\":%4.3f}",
              (unsigned long)bootCount, soilTemp, soilMoist, batVolt);                                             // 'sprintf' C++ function is used to introduce the values of the sensor variables with the optimal formatting
      
      if(mqttClient.publish(mqttTopicPub, dataStr)){                                                               // The string is published on ThingsBoard topic
        if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
          Debugln(dataStr);                                                                                        // Display the string in the serial monitor
          Debugln(F("Going to sleep until next TX..."));
          xSemaphoreGive(semaphoreSerial);
        }
        bootCount++;

        esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);                                                          // Schedule deep sleep for the specified duration (30 seconds)
        esp_deep_sleep_start();
      }else{
        if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
          Debugln(F("Failed to publish data"));
          xSemaphoreGive(semaphoreSerial);
        }
      }
      // MQTT Pub END ----------------------------------------------------------------------------------------------------------------------------------------
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// PEK THREAD ------------------------------------------------------------------------------------------------------------------------------------------------
static void PEKTask(void *pvParameters){
  while(true) {
    if(pekPressed){                                                                                                // Check for PEK press ISR flag
      pekPressed = false;
      axp.readIRQ();                                                                                               // The task checks the type of IRQ

      if(axp.isPEKLongtPressIRQ()){                                                                                // If the IRQ is long-press type, the device is switched off
        if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
          Debugln(F("Long press detected: Shutting down..."));
          xSemaphoreGive(semaphoreSerial);
        }
        vTaskDelay(pdMS_TO_TICKS(100));                                                                            // Delay to get to see the print
        axp.shutdown();
      }

      axp.clearIRQ();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
// THREADS END ===============================================================================================================================================

// ===========================================================================================================================================================
// SETUP FUNCTION
// ===========================================================================================================================================================
void setup() {
  #if ENABLE_SERIAL
    Serial.begin(115200);
  #endif

  Debugln(F("Soil Quality Sensor Beta"));

  // AXP192 setup --------------------------------------------------------------------------------------------------------------------------------------------
  Wire.begin(SDA_PIN, SCL_PIN);                                                                                  // Initialize I2C bus
  
  if(axp.begin(Wire, AXP192_SLAVE_ADDRESS) != 0){                                                                // "AXP192_SLAVE_ADDRESS" should be "0x34"
    Debugln(F("AXP192 not detected!"));
    while(1);
  }else{
    Debugln(F("AXP192 detected"));
  }

  axp.setPowerOutPut(AXP192_LDO2, AXP202_OFF);                                                                   // Turn off LoRa
  axp.setPowerOutPut(AXP192_LDO3, AXP202_OFF);                                                                   // Disable GPS power
  Debugln(F("GPS and LoRa powered off"));

  axp.adc1Enable(AXP202_BATT_VOL_ADC1, true);                                                                    // Enable ADC for battery voltage

  pinMode(PMU_IRQ_PIN, INPUT);                                                                                   // Set up PEK button IRQ pin

  axp.clearIRQ();                                                                                                // Clear any existing IRQs
  axp.enableIRQ(AXP202_PEK_LONGPRESS_IRQ, true);                                                                 // Enable PEK IRQ for long press
  attachInterrupt(digitalPinToInterrupt(PMU_IRQ_PIN), handlePMUIRQ, FALLING);                                    // Enable the interruption to notify the ESP32 to give access to execute the code to power off the device
  // AXP192 setup END ----------------------------------------------------------------------------------------------------------------------------------------
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, ledState);

  esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);                                                                   // Enable deep sleep interrupt using builtin button

  // Connect to Wi-Fi during setup ---------------------------------------------------------------------------------------------------------------------------
  Debug(F("Connecting to WIFI SSID "));
  Debugln(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid, password);

  while(WiFi.status() != WL_CONNECTED){
    delay(500);
    Debug(".");
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);

    if(digitalRead(PMU_IRQ_PIN) == LOW){                                                                         // Here we need to implement the PEK press check as the thread is not running yet
      axp.readIRQ();

      if(axp.isPEKLongtPressIRQ()){
        Debugln(F("Long press detected: Shutting down..."));
        delay(100);                                                                                              // Delay to get to see the message completely
        axp.shutdown();
      }

      axp.clearIRQ();
    }
  }

  Debugln(F(""));

  Debug(F("WiFi connected, IP address: "));
  Debugln(WiFi.localIP());

  if(ledState){
    digitalWrite(LED_PIN, LOW);
  }
  // Connect to Wi-Fi during setup END -----------------------------------------------------------------------------------------------------------------------
  
  setupOTA();                                                                                                    // Function that contains all the OTA parameters setup

  secureClient.setCACert(root_ca);                                                                               // Initialization of the ciphered connection
  mqttClient.setServer(mqtt_server, mqtt_port);                                                                  // Function of the MQTT library to establish connection with the broker

  // FreeRTOS setup ------------------------------------------------------------------------------------------------------------------------------------------
  // Create the semaphore
  semaphoreSerial = xSemaphoreCreateMutex();

  // Initialize Tasks
  xTaskCreatePinnedToCore(
    MQTTTask,                                                                                                    /* Function to implement the task */
    "MQTTTask",                                                                                                  /* Name of the task */
    10000,                                                                                                       /* Stack size in bytes */
    NULL,                                                                                                        /* Task input parameter */
    1,                                                                                                           /* Priority of the task */
    &MQTTTaskHandle,                                                                                             /* Task handle. */
    1                                                                                                            /* Core where the task should run */
  );

  xTaskCreatePinnedToCore(
    PEKTask,                                                                                                     /* Function to implement the task */
    "PEKTask",                                                                                                   /* Name of the task */
    5000,                                                                                                        /* Stack size in bytes */
    NULL,                                                                                                        /* Task input parameter */
    1,                                                                                                           /* Priority of the task */
    &PEKTaskHandle,                                                                                              /* Task handle. */
    0                                                                                                            /* Core where the task should run */
  );
  // FreeRTOS setup END --------------------------------------------------------------------------------------------------------------------------------------
}
// SETUP FUNCTION END ========================================================================================================================================

// ===========================================================================================================================================================
// LOOP FUNCTION
// ===========================================================================================================================================================
void loop() {
  delay(10000);                                                                                                  // Empty loop as FreeRTOS is doing the tasks' job
}
// LOOP FUNCTION END =========================================================================================================================================

// ===========================================================================================================================================================
// AUXILIARY FUNCTIONS
// ===========================================================================================================================================================
// RECONNECT TO MQTT -----------------------------------------------------------------------------------------------------------------------------------------
static void reconnectToMQTT() {
  while(!mqttClient.connected()){                                                                                // Loop until we're reconnected
    if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
      Debug(F("Attempting MQTT connection..."));
      xSemaphoreGive(semaphoreSerial);
    }

    if(mqttClient.connect("soil_quaity_sensor", access_token, NULL)){                                            // Attempt to connect
      if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
        Debugln(F("connected"));
        xSemaphoreGive(semaphoreSerial);
      }
    }else{
      if(xSemaphoreTake(semaphoreSerial, portMAX_DELAY)){
        Debug(F("failed, rc="));
        Debug(mqttClient.state());
        Debugln(F(" try again in 5 seconds"));
        xSemaphoreGive(semaphoreSerial);
      }

      vTaskDelay(pdMS_TO_TICKS(5000));                                                                           // Wait 5 seconds before retrying
    }
  }
}
// RECONNECT TO MQTT END -------------------------------------------------------------------------------------------------------------------------------------

// SETUP OTA -------------------------------------------------------------------------------------------------------------------------------------------------
static void setupOTA(){
  ArduinoOTA.setHostname("soil-quality-sensor");                                                                 // Set custom OTA hostname
  ArduinoOTA.setPassword("pw0123");                                                                              // No authentication by default
  
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else                                                                                                       // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Debugln(String(F("Start updating ")) + type);
    })
    .onEnd([]() {
      Debugln(F("\nEnd"));
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Debugf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Debugf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Debugln(F("Auth Failed"));
      else if (error == OTA_BEGIN_ERROR) Debugln(F("Begin Failed"));
      else if (error == OTA_CONNECT_ERROR) Debugln(F("Connect Failed"));
      else if (error == OTA_RECEIVE_ERROR) Debugln(F("Receive Failed"));
      else if (error == OTA_END_ERROR) Debugln(F("End Failed"));
    });

  ArduinoOTA.begin();

  Debugln(F("OTA service started!"));
}
// SETUP OTA END ---------------------------------------------------------------------------------------------------------------------------------------------
// AUXILIARY FUNCTIONS END ===================================================================================================================================
