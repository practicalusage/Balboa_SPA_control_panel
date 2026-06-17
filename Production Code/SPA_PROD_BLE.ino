/*
Arduino Sketch to allow RS485 communications with SPA
All rights PracticalUsage.com

2026-5-8 Was able to receive from spa but trouble with probably "printf" kind of going wild sometimes
2026-5-8 Was able to analyse FA state and FB commands from existing keyboard
2026-5-11 Sending command to SPA (fake using another arduino) after interrupt from SPA PIN 5
2026-5-22 Success! Timing perfect when SENDING commands. Clean up code
          for now, only wifi access. Will build button box for direct control.
2026-5-31 Added bluetooth with connection to iPhone app I developed
  ESP32-S3 BLE Server — Companion firmware for the iOS app
  =========================================================
  Board : ESP32-S3 (any variant)
  Library: ESP32 BLE Arduino (built-in with espressif32 platform)

OptoCoupler
  Using an 6N138 optocoupler to isolate incoming signal from PIN 5 of SPA (5V) to respect specs of ESP32 (3.3V)
  PIN 2: 5Volt signal from PIN 5 of spa with 220 to 1K resistor (to protect opto LED current draw)
  PIN 3: Ground
  PIN 1 & 4: NC
  PIN 5: GND on 3 volt side (but shared all around)
  PIN 6: OUTPUT to 3.3 volt  inputpin on ESP32
  PIN 7: OPTIONAL: 4.7K resistor to PIN 5 to improve Switching speed
  PIN 8: 3.3 Volt from ESP32

  PIN 5 from SPA is HIGH at 5V except when SPA control board is talking (or listening) to this panel.
   Hence tracking FALLING to zero triggers the interrupt. But passing through the optocoupler inverses
   the state of incoming signal, so catching RISING instead.

*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <driver/uart.h>
#include <ArduinoOTA.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "clientInfo.h"

uint32_t last_ota_time = 0;  // for standard Arduino OTA

//RT Using a WaveShare WS485. Some info from their code
#define tub Serial1
#define tubUART UART_NUM_1
#define RX_PIN 18
#define TX_PIN 17
#define RTS_PIN 21        // RS485 direction control, RTS RequestToSend TX or RX, required for MAX485 board.
#define PIN_5_FROM_SPA 2  //PIN to receive signal from SPA when it is talking to us on its pin 5: CAREFULL 3.3V on ESP32

//Keyboard Commands
//They seem to all start with 0xFB 0x06. The last byte is the checksum. Could be calculated but I just spyed it...
//WARNING. replace 0x64, 0x35, 0x16, 0x00  with your keyboard UID
//  ( analyse your existing keyboard FB COMMAND to find and copy it )
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


// BLE definitions
// ── UUIDs — must match BLEManager.swift ────────────────────────────────────
#define SERVICE_UUID "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define CHAR_TX_UUID "BEB5483E-36E1-4688-B7F5-EA07361B26A8"  // notify → iPhone
#define CHAR_RX_UUID "BEB5483E-36E1-4688-B7F5-EA07361B26A9"  // write ← iPhone

BLEServer* pServer = nullptr;
BLECharacteristic* pTxChar = nullptr;  // sends notifications
bool deviceConnected = false;

// My variables
int PIN_5_ACTIVE = 0;
int expectedLength = 0;         // Calculated length of received message based on type
const int numBytesMax = 32;     // Maximum number of characters to receive
byte inputBuffer[numBytesMax];  // An array (buffer) to store the data
//testing byte inputBuffer[] = { 0xfa, 0x14, 0x33, 0x34, 0x32, 0x43, 0x0a, 0x23, 0x09, 0x04c,
//                       0x01, 0x01, 0x01, 0x01, 0x12, 0x31, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1, 0xff };
byte inputBufferStateOld[23] = {};   //Store previously received STATE to only process NEW STATE
byte inputBufferCommandOld[9] = {};  //Same for previous COMMAND
byte inputBufferAEOld[16] = {};      //Same for AE previous string
bool flagNewData = true;             //did we just receive something new?
long unsigned whenPinHIGH = 0;
long unsigned howLongPinHIGH = 0;
//bool SPAWaitingForCommand = false;
//byte toto = 0;  //Just for testing. Can be deleted
//unsigned long lastCmdTime = 0;
//int selectCounter = 0; //for testing
int wifiCounter = 0;
int MQTTCounter = 0;
bool tryWrite = false;  // Command ready to write
int writeLoop = 0;      // We try writing the command more than once
//Buffer for command TO SPA  TODO SIMPLIFY**************
#define maxSizeCommandBufferLength 16
int iCurrentCommandBufferLength = 0;
uint8_t commandBuffer[maxSizeCommandBufferLength];  //OUTGOING command buffer only? TODO Vrify

//MQTT and Wifi Section ***********
// const char* ssid = "YOUR SSID";                 // your network SSID (name of wifi network)(now in clientInfo.h)
// const char* password = "YOUR WIFI PASSWORD";      // your network password
// const char* mqtt_server = "YOUR MQTT IP ADDRESS";  // your mqtt server ip
// const int mqtt_port = 1883;                // your mqtt server port
const char* mqtt_topic = "ESP_SPA";        // topics must match MQTT setup
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
    receivedMQTTFlag = true;  //Processed in loop()
  }
}

//MQTT
void reconnect() {
  printf("MQTT Disconnected\r\n");
  // Loop until we're reconnected
  MQTTCounter = 0;
  while (!mqtt_client.connected() && MQTTCounter < 5) {
    //repeat 5 times only. Allows BLE to work in the eevent network is down (or MQRR down)
    MQTTCounter++;
    // Create a random client ID
    String clientId = "ESP32SPA";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (mqtt_client.connect(clientId.c_str())) {
      printf("MQTT connected\r\n");

      mqtt_client.publish(mqtt_topic, "ESP32_SPA On-Line");
      // ... and resubscribe
      mqtt_client.subscribe(mqtt_topic_command);
    } else {  //Using BLE message when asked for PING
      printf("failed, rc= %i, try again later\r\n", mqtt_client.state());
      // Wait before retrying
      delay(1000);
    }
  }
}

// Bluetooth BLE section (definitions above)
// ── Send helper ─────────────────────────────────────────────────────────────
void notifyPhone(const String& msg) {
  if (!deviceConnected) return;
  pTxChar->setValue(msg.c_str());
  pTxChar->notify();
  Serial.print("[TX] ");
  Serial.println(msg);
}

// ── Server callbacks ─────────────────────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    deviceConnected = true;
    Serial.println("iPhone connected.");
    notifyPhone("ESP32-SPA ready!");
  }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    Serial.println("iPhone disconnected. Restarting advertising…");
    BLEDevice::startAdvertising();
  }
};

// ── RX characteristic callbacks (iPhone writes here) ─────────────────────────
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String msg = String(pChar->getValue().c_str());
    msg.trim();
    Serial.print("[RX] ");
    Serial.println(msg);

    if (msg == "LIGHTS") {
      //digitalWrite(LED_PIN, HIGH);
      setCommand(keyboardCommand_LIGHT, sizeof(keyboardCommand_LIGHT));
      writeLoop = 1;
      tryWrite = true;
      notifyPhone("LIGHTS toggled");

    } else if (msg == "PUMP2") {
      //digitalWrite(LED_PIN, LOW);
      setCommand(keyboardCommand_JET2, sizeof(keyboardCommand_JET2));
      writeLoop = 1;
      tryWrite = true;
      notifyPhone("PUMP2 toggled");

    } else if (msg == "STATUS") {
      //notifyPhone("in Status");
      //Supposes we have a valid FA message in the input buffer
      //format data for BLE (or local display) and send it (notify)
      HandleMessage(sizeof(inputBufferStateOld), inputBufferStateOld);


    } else if (msg == "PING") {  //To check for WIFI and MQTT
      if (!mqtt_client.connected()) {
        notifyPhone("MQTT not connected");
      } else if (!WL_CONNECTED) {
        notifyPhone("WIFI not connected");
      } else {
        notifyPhone("WIFI & MQTT OK");
      }

    } else if (msg == "UP") {  //RT Careful: not called
      setCommand(keyboardCommand_UP, sizeof(keyboardCommand_UP));
      writeLoop = 1;
      tryWrite = true;
      notifyPhone("Temp UP");

    } else if (msg == "DOWN") {
      setCommand(keyboardCommand_DOWN, sizeof(keyboardCommand_DOWN));
      writeLoop = 1;
      tryWrite = true;
      notifyPhone("Temp Down");

    } else {
      notifyPhone("ECHO:" + msg);
    }
  }
};


void IRAM_ATTR panelSelected() { //Interrupt routine. Mostly for testing.
  //msgStartTime = micros();
  //SPAWaitingForCommand = true;
  //uartFlush(tubUART);
  whenPinHIGH = micros();  //All that's needed if not testing
  //selectCounter++;
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
  while (tub.available() > 0) {  // workaround for bug with hanging during Serial2.begin -
                                 // https://github.com/espressif/arduino-esp32/issues/5443
    Serial.read();
  }

  WiFi.begin(ssid, password);
  // attempt to connect to Wifi network: try 10 times
  // This is necessary to allow BLE to work even if not connected to Wifi (i.e. network down)
  while (WiFi.status() != WL_CONNECTED && wifiCounter < 10) {
    printf(".");
    // wait 1 second for re-trying
    wifiCounter++;
    delay(1000);
  }
  wifiCounter = 0;

  IPAddress ip = WiFi.localIP();
  printf("WIFI Connected: with IP: %i \r\n", ip[3]);
  delay(1000);

  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(callback);

  prepareOTA();  //RT. See below

  attachPanelInterrupt();

  //BLE
  prepareBLE();  //RT See below


  printf("End of setup\r\n");
}

void loop() {

  PIN_5_ACTIVE = digitalRead(PIN_5_FROM_SPA);  // Is SPA talking to us, on this panel
  if (PIN_5_ACTIVE) {                          //SPA TALKING TO US?
    if (tub.available() > 0) {                 //anything in the receiving buffer?

      expectedLength = numberOfBytesToReceive();  //Based on 3 types of messages
      if (expectedLength >= 0) {
        tub.readBytes(inputBuffer, expectedLength);

        if (inputBuffer[0] == 0xFA && expectedLength != 0) {  //After much experimenting: Only look at FA messages
          howLongPinHIGH = micros() - whenPinHIGH;            //set in interrupt. Mostly for testing
          //printf(" FA received %u %i\r\n", howLongPinHIGH, selectCounter);// To test timing
          //Acting on new data only
          flagNewData = false;
          for (int i = 0; i < expectedLength; i++) { 
            if (inputBuffer[i] != inputBufferStateOld[i]) { // Only process if we get a new FA message
              flagNewData = true;
            }
          }
          if (flagNewData) {
            flagNewData = false;
            for (int i = 0; i < expectedLength; i++) { // Keep a copy of the new command for comparison
              //Careful: inputBufferStateOld is only used here so we know it's the right size and content in handleMessage()
              //It might be erroneous when the SPA has just started, but then it's initiazed empty. We could test
              // its length just before calling handleMessage() in BLE callback
              inputBufferStateOld[i] = inputBuffer[i];
            }
            // Process the received data (e.g., print it)
            // The ESP32 subcontracts (!) the printing chores to a separate process. It does not really slow
            //  the main processor. This is crucial because at this point in the processing of the FA message,
            //  timing is crucial. MQTT publish seems to be handled the same way.
            for (int i = 0; i < expectedLength; i++) {
              printf("%x", inputBuffer[i]);  // Print the byte in hexadecimal format
              //printf(" ");
            }
            printf("\r\n");
            //RT TODO: publish only if MQTT is connected
            mqtt_client.publish(mqtt_topic_RCVD, inputBuffer, expectedLength);  //send it ASAP for study
          }
          // 3 FA commands coming including the first one. Might have to try sending the command again
          //      but my tests have been conclusive on first send. Anyway, SPA ignores the 2 remaining sends...
          if (tryWrite == true && writeLoop < 4) {  //only try writing 3 times, one for each FA
            writeLoop++;
            if (writeLoop > 3) {
              tryWrite = false;
            }
            digitalWrite(RTS_PIN, HIGH);  //Tell the RS485 interface that we want to write on the bus. Standard.
            delayMicroseconds(20);        //THIS IS THE IMPORTANT DELAY to finish processing incoming FA state
            tub.write(commandBuffer, iCurrentCommandBufferLength);
            //delay(3);  //this delay my be important when sending to an Arduino for testing: adjust: works with 5 ms, not with 1 or 2 ms bur OK with 3!!!!!!!!!!!!!!!
            //Arduino UNO was receiving the right command (LIGHT) for the first time.
            //delayMicroseconds(300);  //for testing with Arduino again
            howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
            tub.flush(true);                          //from netmindz
            delay(1);                                 //from netmindz
            //printf("Command sent %u %i\n", howLongPinHIGH, selectCounter);
            digitalWrite(RTS_PIN, LOW);
          }
        } else if (inputBuffer[0] == 0xFB && expectedLength != 0) {
          // In my setup, SPA only sends FB when this panel is NOT selected. So this is not necessary
          //howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
          //printf("  FB received %u\r\n", howLongPinHIGH);
        } else if (inputBuffer[0] == 0xAE && expectedLength != 0) {
          // AE buffers are boring... I don't know what they do!
          //howLongPinHIGH = micros() - whenPinHIGH;  //set in interrupt
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
    // We just really need to clear the tub buffer (>0) unless we are testing
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
  // if (millis() % 5000 == 0) { //TESTING sending command at fixed interval
  //   writeLoop = 1;
  //   tryWrite = true;
  // }


  //We received and MQTT COMMAND and have to process it ASAP
  //RT TODO other commands validation. Transfer code to a function, called from MQTT directly.
  if (receivedMQTTFlag) {
    printf("Processing the command outside the CallBack");
    if (receivedData == "1234") {  //TESTING TODO phase out
      printf("Command recognised: LIGHTS %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_LIGHT, sizeof(keyboardCommand_LIGHT));
    } else if (receivedData == "2345") {  //TESTING TODO phase out
      printf("Command recognised: JET2 %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_JET2, sizeof(keyboardCommand_JET2));
    } else if (receivedData == "LIGHTS") {
      printf("Command recognised: LIGHTS %s\r\n", receivedData);
      setCommand(keyboardCommand_LIGHT, sizeof(keyboardCommand_LIGHT));
    } else if (receivedData == "PUMP1") {
      printf("Command recognised: PUMP1 %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_JET1, sizeof(keyboardCommand_JET1));
    } else if (receivedData == "PUMP2") {
      printf("Command recognised: PUMP2 %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_JET2, sizeof(keyboardCommand_JET2));
    } else if (receivedData == "TIME") {
      printf("Command recognised: TIME %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_TIME, sizeof(keyboardCommand_TIME));
    } else if (receivedData == "UP") {
      printf("Command recognised: UP %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_UP, sizeof(keyboardCommand_UP));
    } else if (receivedData == "DOWN") {
      printf("Command recognised: DOWN %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_DOWN, sizeof(keyboardCommand_DOWN));
    } else if (receivedData == "MODE") {
      printf("Command recognised: MODE %s\r\n", receivedData);
      //Testing with LIGHT toggle
      setCommand(keyboardCommand_CHANGE_MODE, sizeof(keyboardCommand_CHANGE_MODE));
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

  // Might place the rest of this code in a ELSE of the FA message processing to gain speed.
  ArduinoOTA.handle();
  mqtt_client.loop();
  if (!mqtt_client.connected() && millis() % 10 == 0) {  //delay to repeat every few seconds if disconnected
    while (WiFi.status() != WL_CONNECTED && wifiCounter < 10) {
      printf(".");
      // wait 1 second for re-trying
      wifiCounter++;
      delay(1000);
    }
    wifiCounter = 0;
    printf("MQTT disconnected in loop\r\n");
    reconnect();
  }
}

void clearDataBuffer() {
  expectedLength = 0;
  memset(inputBuffer, 0, 32);
}

// Put command in Command buffer, ready to send when next FA message comes in
void setCommand(uint8_t buff[], size_t len) {
  //clearCommandBuffer();
  memcpy(commandBuffer, buff, len); //Trying memcpy. Seems to be same speed as for()
  //for (int i = 0; i < lengthCommand; i++){
  //  commandBuffer[i] = keyboardCommand_LIGHT[i];
  //}
  iCurrentCommandBufferLength = len;
  printf("Setting Command: \r\n");
  for (int i = 0; i < len; i++) {
    printf("%x", buff[i]);
  }
}

/*
 * numberOfBytesToReceive() checks the first byte in the serial buffer
 * since we've cleared the serial buffer when pin5 went low
 * there will only be data meant for our panel in the buffer.
 * Depending on the message type, wait for the expected number
 * of bytes to be available with a timeout of 2.5ms
 * returns number of bytes to read
 */
int numberOfBytesToReceive() {
  int msgLength = 0;
  byte lookAtByte = tub.peek();
  // define message length from starting Byte
  switch (lookAtByte) {
    case 0xFA:
      //printf("\r\nSTATE received FA");
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
  //we'll wait here for up to 2.5ms until the expected number of bytes are available (from netmindz)
  unsigned long startTime = micros();
  while (tub.available() < msgLength) {
    if (micros() - startTime >= 2500) {
      printf("Timeout: %u bytes not available in 2.5ms\n");
      return 0;  //RT: drastic GOTO!
    }
  }
  return msgLength;
}

void HandleMessage(size_t len, uint8_t buff[]) {
  /* we are here because we got an FA14 valid message.
This code was added to format data for bluetooth transmission
instead of formating in the iPhone application
This code could also be used for MQTT instead of sending the complete HEX buffer
*/
  //Prepare BLE Notification buffers
  char notificationBuffer[80];
  char newBuffer[80];  //RT This is the way I found to concatenate buffer with snprintf.

  // Let's check for a valid tempUnit
  String tempUnit = "?";

  if (buff[5] == 0x43) {  //ASCII char
    tempUnit = "C";
  } else if (buff[5] == 0x46) {
    tempUnit = "F";
  } else if (buff[5] == 0x2d) {
    tempUnit = "-";
  }

  if (tempUnit != "?") {
    //Then calculate temp
    int temperature = 0;
    if (tempUnit == "C") {
      temperature = (buff[2] & 0x0f) * 100;  //Temp is actual ASCII digits so we keep only rightmost halfbyte
      temperature += (buff[3] & 0x0f) * 10; //TODO Carefull as C temp is floating point with one decimal
      temperature += (buff[4] & 0x0f);
      temperature = temperature;

      printf("temperature: %.1f %s ", temperature / 10.0, tempUnit);
    } else if (tempUnit == "F") {
      if (buff[2] != 0x20) {
        temperature = (buff[2] & 0x0f) * 100;  //a space (0x20) as first digit if below 100F
      }
      temperature += (buff[3] & 0x0f) * 10;
      temperature += (buff[4] & 0x0f);
      temperature = temperature;

      snprintf(notificationBuffer, sizeof(notificationBuffer), "Temperature: %i %s ", temperature, tempUnit);
    }
  } else {
    snprintf(notificationBuffer, sizeof(notificationBuffer), "Temperature ?");
  }
  snprintf(newBuffer, sizeof(newBuffer), "%s", notificationBuffer);  //Concatenate with existing content in buffer

  //Let's process the PUMP information
  byte pumpInfo = buff[6];  //Might optimize with 0x0f as only right digit is used
  int pump1State = 0;
  int pump2State = 0;
  String pump1StateString = "Off";
  String pump2StateString = "Off";

  switch (pumpInfo) {  //?????Verify if MSD or LSD. Old code = LSD => last 4 bits (substring 13)

    case 0x00:  // Pump 1 Off - Pump 2 Off - 0b0000
      break;

    case 0x01:  // Pump 1 Low - Pump 2 Off - 0b0001
      pump1State = 1;
      pump1StateString = "Low";
      pump2State = 0;
      pump2StateString = "Low";
      break;

    case 0x02:  // Pump 1 High - Pump 2 Off - 0b0010
      pump1State = 2;
      pump1StateString = "High";
      pump2State = 0;
      pump2StateString = "Off";
      break;

    case 0x04:  // Pump 1 Off - Pump 2 low - 0b0100
      pump1State = 0;
      pump1StateString = "Off";
      pump2State = 1;
      pump2StateString = "Low";
      break;

    case 0x05:  // Pump 1 Low - Pump 2 Low - 0b0101
      pump1State = 1;
      pump1StateString = "Low";
      pump2State = 1;
      pump2StateString = "Low";
      break;

    case 0x06:  // Pump 1 High - Pump 2 Low - 0b0110
      pump1State = 2;
      pump1StateString = "High";
      pump2State = 1;
      pump2StateString = "Low";
      break;

    case 0x08:  // Pump 1 Off - Pump 2 High - 0b1000
      pump1State = 0;
      pump1StateString = "Off";
      pump2State = 2;
      pump2StateString = "High";
      break;

    case 0x09:  // Pump 1 Low - Pump 2 High - 0b1001
      pump1State = 1;
      pump1StateString = "Low";
      pump2State = 2;
      pump2StateString = "High";
      break;

    case 0x0a:  // Pump 1 High - Pump 2 HIGH = 0b1010
      pump1State = 2;
      pump1StateString = "High";
      pump2State = 2;
      pump2StateString = "High";
      break;

    default:
      printf("Received an unknown pump state");
      break;
  };
  snprintf(notificationBuffer, sizeof(notificationBuffer), "%s Pump1 %s Pump2 %s", newBuffer, pump1StateString, pump2StateString);
  //printf("Pump1 state: %s ", pump1StateString);
  //printf("Pump2 state: %s ", pump2StateString);
  snprintf(newBuffer, sizeof(newBuffer), "%s", notificationBuffer);


  // Heather
  byte heaterInfo = buff[7] & 0xf0;  //Only look at first bits?
  int heaterState = 0;
  if (heaterInfo == 0x00) {
    heaterState = 0;  // Heather OFF
  } else {
    heaterState = 1;  // CAUTION heatherInfo may be 2, an unknown state?
  }
  snprintf(notificationBuffer, sizeof(notificationBuffer), "%s Heater %i", newBuffer, heaterState);
  //printf(" Heater state: %i ", heaterState);
  snprintf(newBuffer, sizeof(newBuffer), "%s", notificationBuffer);

  // Lights
  byte lightInfo = buff[7] & 0x0f;  //Only look at last bits?
  int lightState = 0;
  if (lightInfo == 0x00) {
    lightState = 0;
  } else {
    if (lightInfo == 0x03) {
      lightState = 1;
    }
  }
  snprintf(notificationBuffer, sizeof(notificationBuffer), "%s Lights %i", newBuffer, lightState);
  //snprintf(newBuffer, sizeof(newBuffer), "%s", notificationBuffer); //if you continue, Careful woth LENGTH!
  //printf(" light state: %i", lightState);

  notifyPhone(String(notificationBuffer));  //TODO might be better in BLE functions. Would have to declare buffers global
}

//Standard Arduino supplied OTA
void prepareOTA() {
  //RT for arduino OTA: May ask for password ==> 12356789 for direct connection to module WiFi
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

void prepareBLE() {
  BLEDevice::init("ESP32-SPA");  //RT Careful: Must match BLEManager.swift advertiser

  //RT Set Max Power (+9 dBm) for Default, Advertising, and Scanning
  // because a tub full of water is bad for BLE signal...
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // TX characteristic (ESP32 → iPhone via notify)
  pTxChar = pService->createCharacteristic(
    CHAR_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY);
  pTxChar->addDescriptor(new BLE2902());  // required for notify

  // RX characteristic (iPhone → ESP32 via write)
  BLECharacteristic* pRxChar = pService->createCharacteristic(
    CHAR_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);  // helps iPhone discover faster
  BLEDevice::startAdvertising();

  Serial.println("BLE advertising as 'ESP32-SPA'…");
}
