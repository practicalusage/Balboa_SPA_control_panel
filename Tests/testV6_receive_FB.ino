#include <WiFi.h>
#include <PubSubClient.h>
#include <driver/uart.h>
#include <ArduinoOTA.h>

uint32_t last_ota_time = 0;  // for OTA

//RT Using a WaveShare WS485. Some info from their code
#define tub Serial1
#define tubUART UART_NUM_1
#define RX_PIN 18
#define TX_PIN 17
#define RTS_PIN 21        // RS485 direction control, RTS RequestToSend TX or RX, required for MAX485 board.
#define PIN_5_FROM_SPA 2  //PIN to receive signal from SPA when it is talking to us on its pin 5: CAREFULL 3.3V on ESP32

//uint8_t keyboardCommand_LIGHT[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x09, 0xF6, 0x05 };  //LIGHT toggle
//Keyboard Commands
//They seem to all start with 0xFB 0x06. The last byte is the checksum. Could be calculated but I just spyed it...
//WARNING. replace 0x64, 0x35, 0x16, 0x00  with your keyboard UID ( analyse your existing keyboard FB COMMAND to find it )
// EXAMPLES
// fb 06 66 66 66 66 02 fd 50
// fb 06 03 45 0e 00 09 f6 f6
uint8_t keyboardCommand_UP[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x01, 0xFE, 0x95 };
uint8_t keyboardCommand_DOWN[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x02, 0xFD, 0xA3 };
uint8_t keyboardCommand_TIME[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x03, 0xFC, 0xB1 };
uint8_t keyboardCommand_CHANGE_MODE[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x04, 0xFB, 0xCF };
uint8_t keyboardCommand_JET1[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x06, 0xF9, 0xEB };
uint8_t keyboardCommand_JET2[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x07, 0xF8, 0xF9 };
uint8_t keyboardCommand_OPTION[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x08, 0xF7, 0x17 };  //RT does nothing on my SPA...
uint8_t keyboardCommand_EMPTY[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x00, 0xFF, 0x87 };   //RT received after sending a command
uint8_t keyboardCommand_LIGHT[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x09, 0xF6, 0x05 };   //LIGHT toggle
uint8_t keyboardCommand_INVERT[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x0A, 0xF5, 0x33 };  //RT INVERT the display instead of BLOWER





unsigned long timeBeetweenFrame = 290UL;  //290us
int PIN_5_ACTIVE = 0;
unsigned long timeLastPin5 = 0;
unsigned long intervalLOW = 0;
int expectedLength = 0;
const int numBytesMax = 32;     // Maximum number of characters to receive
byte inputBuffer[numBytesMax];  // An array (buffer) to store the data
//testing byte inputBuffer[] = { 0xfa, 0x14, 0x33, 0x34, 0x32, 0x43, 0x0a, 0x23, 0x09, 0x04c,
//                       0x01, 0x01, 0x01, 0x01, 0x12, 0x31, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1, 0xff };
byte inputBufferOld[numBytesMax] = {};
byte inputBufferStateOld[23] = {};   //Store previously received STATE to only process NEW STATE
byte inputBufferCommandOld[9] = {};  //Same for previous COMMAND
byte inputBufferAEOld[16] = {};      //Same for AE unknown string
bool flagNewData = true;             //did we just receive something new?
long unsigned whenPinHIGH = 0;
long unsigned howLongPinHIGH = 0;
bool SPAWaitingForCommand = false;
byte toto = 0;
unsigned long lastCmdTime = 0;
int selectCounter = 0;
bool tryWrite = false;
int writeLoop = 0;
//Buffer for command TO SPA SIMPLIFY**************
#define maxSizeCommandBufferLength 16
int iCurrentCommandBufferLength = 0;
uint8_t commandBuffer[maxSizeCommandBufferLength];  //OUTGOING command buffer only? TODO Vrify

//MQTT Section ***********
const char* ssid = "yourssid";                 // your network SSID (name of wifi network)
const char* password = "yourpassword";      // your network password
const char* mqtt_server = "mqttserverIP";  // your mqtt server ip
const int mqtt_port = 1883;                // your mqtt server port
const char* mqtt_topic = "ESP_SPA";        // topic (don't change this)
const char* mqtt_topic_command = "ESP_SPA/command";
const char* mqtt_topic_RCVD = "ESP_SPA_RCVD";  // When sending valid status buffer to MQTT
String receivedData = " ";
bool receivedMQTTFlag = false;




//WIFI and MQTT
WiFiClient client;
PubSubClient mqtt_client(client);

//MQTT
void callback(char* topic, byte* payload, unsigned int length) {
  int i;
  String strPayload;

  strPayload = "";
  for (int i = 0; i < length; i++) {
    strPayload += (char)payload[i];
  }
  printf("Message received from MQTT: %s\r\n", strPayload);
  //Do we need to process a command? Else for now we just print
  // Use strcmp() to compare char arrays (Strings cannot be compared with == in this context)
  if (strcmp(topic, mqtt_topic_command) == 0) {
    // Logic for COMMAND
    printf("We have just received a MQTT Command!");
    receivedData = strPayload;
    receivedMQTTFlag = true;
  } else if (strcmp(topic, "rooms/singola/heater") == 0) {
    // Logic for "rooms/singola/heater" topic
  } else if (strcmp(topic, "home/bedroom/lights") == 0) {
    // Logic for "home/bedroom/lights" topic
  }
}

//MQTT
void reconnect() {
  printf("MQTT Disconnected\r\n");
  // Loop until we're reconnected
  while (!mqtt_client.connected()) {
    // Create a random client ID
    String clientId = "ESP32SPA";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (mqtt_client.connect(clientId.c_str())) {
      printf("MQTT connected\r\n");
      //delay(2000); //RT Test

      // Once connected, publish an announcement...
      //mqtt_client.publish(mqtt_topic, "HELTEC TEST Connected");
      //IPAddress ip = WiFi.localIP();
      // Convert the uint8_t to an Arduino String object
      //String myString = "HELTEC TEST Connected with IP " + String(ip[3]);
      // Get the const char* from the String object using c_str()
      //const char* charPointer = myString.c_str();
      mqtt_client.publish(mqtt_topic, "ESP32_SPA On-Line");
      // ... and resubscribe
      //delay(1000);
      //mqtt_client.subscribe(mqtt_topic);
      mqtt_client.subscribe(mqtt_topic_command);
    } else {
      printf("failed, rc= %i, try again in 5 seconds\r\n", mqtt_client.state());
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void IRAM_ATTR panelSelected() {
  //msgStartTime = micros();
  SPAWaitingForCommand = true;
  //uartFlush(tubUART);
  whenPinHIGH = micros();
  selectCounter++;
  //clearDataBuffer();
  //uart_flush(tubUART);  //Requires #include <driver/uart.h>
  //tub.flush(true);
}
/**
 * @brief enable interrupt for pin5 falling level change so we can clear the rx buffer
 *  every time our panel is selected
 */
void attachPanelInterrupt() {
  //attachInterrupt(digitalPinToInterrupt(PIN_5_FROM_SPA), panelSelected, FALLING);
  // RISING if using an optocoupler (6N138)
  attachInterrupt(digitalPinToInterrupt(PIN_5_FROM_SPA), panelSelected, RISING);
}

void setup() {
  Serial.begin(115200);
  printf("Beginning setup\r\n");

  pinMode(RTS_PIN, OUTPUT);
  printf("Setting receive-transmit pin %u LOW\n", RTS_PIN);
  digitalWrite(RTS_PIN, LOW);             //Only HIGH when need to send on RS485
  pinMode(PIN_5_FROM_SPA, INPUT_PULLUP);  //PULLUP even if signal is pulled to zero by optocoupler. Ensures better response if short interrupt <1ms
  printf("Setting serial port as pins %u, %u\n", RX_PIN, TX_PIN);
  tub.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
  while (tub.available() > 0) {  // workarond for bug with hanging during Serial2.begin -
                                 // https://github.com/espressif/arduino-esp32/issues/5443
    Serial.read();
  }

  WiFi.begin(ssid, password);
  // attempt to connect to Wifi network:
  while (WiFi.status() != WL_CONNECTED) {
    printf(".");
    // wait 1 second for re-trying
    delay(1000);
  }

  IPAddress ip = WiFi.localIP();
  printf("WIFI Connected: with IP: %i \r\n", ip[3]);
  delay(1000);

  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(callback);

  prepareOTA();  //RT. See below

  attachPanelInterrupt();


  printf("End of setup\r\n");
}

void loop() {

  /* Only give timing for Valid incoming message and ignore the rest. We want to figure out the timeing
  V6 try to confirm FB from SPA after succesfull FB sent
  */
  PIN_5_ACTIVE = digitalRead(PIN_5_FROM_SPA);
  if (PIN_5_ACTIVE) {  //SPA tALKING TO US
                       //if (true) {                       //SPAWaitingForCommand = false;
    if (tub.available() > 0) {

      expectedLength = numberOfBytesToReceive();
      if (expectedLength >= 0) {
        tub.readBytes(inputBuffer, expectedLength);

        if (inputBuffer[0] == 0xFA && expectedLength != 0) {
          howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
          //printf(" FA received %u %i\r\n", howLongPinHIGH, selectCounter);
          //testing for new data only
          flagNewData = false;
          for (int i = 0; i < expectedLength; i++) {
            if (inputBuffer[i] != inputBufferStateOld[i]) {
              flagNewData = true;
            }
          }
          if (flagNewData) {
            flagNewData = false;
            for (int i = 0; i < expectedLength; i++) {
              inputBufferStateOld[i] = inputBuffer[i];
            }
            // Process the received data (e.g., print it)
            for (int i = 0; i < expectedLength; i++) {
              printf("%x", inputBuffer[i]);  // Print the byte in hexadecimal format
              //printf(" ");
            }
            printf("\r\n");
            mqtt_client.publish(mqtt_topic_RCVD, inputBuffer, expectedLength);  //send it ASAP for study
          }
          //if (millis() - lastCmdTime >= 400 ) {  //from netmindz
          if (tryWrite == true && writeLoop < 4) {  //only try writing 3 times
            writeLoop++;
            if (writeLoop > 3) {
              tryWrite = false;
            }
            lastCmdTime = millis();
            digitalWrite(RTS_PIN, HIGH);
            delayMicroseconds(20);  //from netmindz
            //delayMicroseconds(300);  //is This delay necessary???????????????
            tub.write(commandBuffer, iCurrentCommandBufferLength);
            //delay(3);  //????????? this delay seems imprtant: adjust: works with 5 ms, not with 1 or 2 ms bur OK with 3!!!!!!!!!!!!!!!
            //Arduino UNO was receiving the right command (LIGHT) for the first time.
            //delayMicroseconds(300);  //fonctionne avec les deux délais
            howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
            tub.flush(true);                          //from netmindz
            delay(1);                                 //from netmindz
            //printf("Command sent %u %i\n", howLongPinHIGH, selectCounter);
            digitalWrite(RTS_PIN, LOW);
            // for (int i = 0; i < expectedLength; i++) {
            //   printf(" %x", inputBuffer[i]);
            // }
            //toto = 0x00
          }
        } else if (inputBuffer[0] == 0xFB && expectedLength != 0) {
          howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
          //printf("  FB received %u\r\n", howLongPinHIGH);
        } else if (inputBuffer[0] == 0xAE && expectedLength != 0) {
          howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
          //printf("   AE received %u\r\n", howLongPinHIGH);
        } else {
          //printf("NOT an FA or FB or AE\n");
        }
      }
      tub.flush();
      uart_flush(tubUART);
      clearDataBuffer();
      while (tub.available()) {
        tub.read();
      }
    }
    //delayMicroseconds(290);
  } else {
    if (tub.available() > 0) {
      expectedLength = numberOfBytesToReceive();
      if (expectedLength >= 0) {
        tub.readBytes(inputBuffer, expectedLength);
        if (inputBuffer[0] == 0xFB && expectedLength != 0) {
          howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
          //printf("  FB received %u %i\r\n", howLongPinHIGH, selectCounter);
        }
      }

      tub.flush();
      uart_flush(tubUART);
      clearDataBuffer();
      while (tub.available()) {
        tub.read();
      }
    }
  }
  // if (millis() % 5000 == 0) {
  //   //toto = Serial.read();
  //   //Serial.println(toto);
  //   writeLoop = 1;
  //   tryWrite = true;
  // }
  //We received and MQTT COMMAND and have to process it ASAP
  //RT TODO other commands validation
  if (receivedMQTTFlag) {
    printf("Processing the command outside the CallBack");
    if (receivedData == "1234") {
      printf("Command recognised: LIGHTS %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_LIGHT, sizeof(keyboardCommand_LIGHT));
    } else if (receivedData == "2345") {
      printf("Command recognised: JET2 %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_JET2, sizeof(keyboardCommand_JET2));
    }
    writeLoop = 1;
    tryWrite = true;
    receivedMQTTFlag = false;
  }
  //tub.flush();
  // if (Serial.available()) {
  //   toto = Serial.read();
  //   //Serial.println(toto);
  // }
  //clearDataBuffer();
  ArduinoOTA.handle();
  mqtt_client.loop();
  if (!mqtt_client.connected()) {
    //myString = "MQTT Disconnected\r\n";
    //    String myString = "WIFI Connected " + String(ip[3]);
    printf("MQTT disconnected in loop\r\n");
    reconnect();
  }
}

void clearDataBuffer() {
  expectedLength = 0;
  memset(inputBuffer, 0, 32);
}

void setCommand(uint8_t buff[], size_t len) {  //put command in Command buffer
  //clearCommandBuffer();
  //int lengthCommand = sizeof(buff);
  memcpy(commandBuffer, buff, len);
  //for (int i = 0; i < lengthCommand; i++){
  //  commandBuffer[i] = keyboardCommand_LIGHT[i];
  //}
  iCurrentCommandBufferLength = len;
  printf("Setting Command: \r\n");
  for (int i = 0; i < len; i++) {
    printf("%x", buff[i]);
  }
}

int numberOfBytesToReceive() {
  int msgLength = 0;
  byte lookAtByte = tub.peek();
  // define message length from starting Byte
  switch (lookAtByte) {
    case 0xFA:
      //printf("\r\nSTATE received FA"); *****************************testing only for FB
      msgLength = 23;
      break;
    case 0xAE:
      //printf("\r\nCommand received AE");
      msgLength = 16;
      break;
    case 0xFB:
      //printf("\r\nCommand received FB");
      msgLength = 9;
      break;
    default:
      byte unknownByte = lookAtByte;
      printf("\r\nUnknown message start Byte: %x\r\n", unknownByte);
      return 0;
  }
  //we'll wait here for up to 2.5ms until the expected number of bytes are available
  unsigned long startTime = micros();
  while (tub.available() < msgLength) {
    if (micros() - startTime >= 2500) {
      printf("Timeout: %u bytes not available in 2.5ms\n");
      return 0;
    }
  }
  return msgLength;
}

void prepareOTA() {
  //RT for arduino OTA: May ask for password=> 12356789 for direct connection to module WiFi
  printf("Arduino OTA setup. ");
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else {  // U_SPIFFS
        type = "filesystem";
      }

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      //     printf("Start updating " + type);
    })
    .onEnd([]() {
      printf("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      if (millis() - last_ota_time > 500) {
        //       printff("Progress: %u%%\n", (progress / (total / 100)));
        last_ota_time = millis();
      }
    })
    .onError([](ota_error_t error) {
      //     printff("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        printf("Auth Failed");
      } else if (error == OTA_BEGIN_ERROR) {
        printf("Begin Failed");
      } else if (error == OTA_CONNECT_ERROR) {
        printf("Connect Failed");
      } else if (error == OTA_RECEIVE_ERROR) {
        printf("Receive Failed");
      } else if (error == OTA_END_ERROR) {
        printf("End Failed");
      }
    });

  ArduinoOTA.begin();
}
