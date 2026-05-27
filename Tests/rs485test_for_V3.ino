/*
Arduino Sketch to allow RS232/RS485 communications
via two Arduinos with attached SN75176 ICs.

From: earl@microcontrollerelectronics.com
*/

#include <SoftwareSerial.h>

SoftwareSerial mySerial(8, 9);  // RX, TX
#define RTS_PIN 7
byte toto = 0;
int stateLength = 23;
byte state[] = { 0xfa, 0x14, 0x20, 0x39, 0x37, 0x46, 0x01, 0x10, 0x29, 0x00, 0x04,
                 0x00, 0x80, 0x00, 0x0e, 0x21, 0x61, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2d };
int commandToSendLength = 9;
byte commandToSend[] = { 0xFB, 0x06, 0x64, 0x35, 0x16, 0x00, 0x09, 0xF6, 0x06 };  //LIGHT toggle
int AETestLength = 16;
byte AETest[] = { 0xAE, 0xD1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a };
const int sendPIN = 12;
byte inputBuffer[9] = {};
bool sendBack = false;

int i = 0;

void setup() {
  Serial.begin(115200);
  mySerial.begin(115200);
  pinMode(RTS_PIN, OUTPUT);
  digitalWrite(RTS_PIN, LOW);
  pinMode(sendPIN, OUTPUT);
  digitalWrite(sendPIN, HIGH);
  mySerial.flush();
}

void loop() {
  if (millis() % 5 == 0) {
    //Serial.print(".");
    //delay(50);
    delayMicroseconds(200);
    //Serial.print("Sending Data ");
    digitalWrite(sendPIN, LOW);
    delay(1);
    digitalWrite(RTS_PIN, HIGH);
    mySerial.write(state, stateLength);
    digitalWrite(RTS_PIN, LOW);
    //Serial.println("");
    digitalWrite(sendPIN, HIGH);
    mySerial.flush();
  }

  if (mySerial.available()) {
    //delay(100);
    int bytesRead = mySerial.readBytes(inputBuffer, 9);
    Serial.print("Received Command sent to SPA: ");
    //digitalWrite(RTS_PIN, HIGH);
    for (int i = 0; i < 9; i++) {
      Serial.print(inputBuffer[i], HEX);
      //mySerial.write(inputBuffer[i]);
    }
    //digitalWrite(RTS_PIN, LOW);
    mySerial.flush();
    Serial.println("");
    //sendBack = true;
  }

  if (Serial.available()) {  //Create fake interrupt for testing. PIN5 from spa HIGH by default
    toto = Serial.read();
    //Serial.print(toto);
    //delayMicroseconds(290);
    //delay(1);
    //delay(50);
    //Serial.print("Sending Data ");
    digitalWrite(sendPIN, LOW);
    delay(1);
    digitalWrite(RTS_PIN, HIGH);
    if (toto == 49) {
      mySerial.write(state, stateLength);
    }
    if (toto == 50) {
      mySerial.write(commandToSend, commandToSendLength);
    }
    if (toto == 51) {
      mySerial.write(AETest, AETestLength);
      for (i = 0; i < AETestLength; i++) {
      }
    }
    //

    delay(1);
    digitalWrite(RTS_PIN, LOW);
    //Serial.println("");
    mySerial.flush();
    digitalWrite(sendPIN, HIGH);

    // delay(1);
    // digitalWrite(sendPIN, HIGH);
  }

  // if (Serial.available()) {
  //   //sendBack = false;
  //   toto = Serial.read();
  //   //Serial.print(toto);
  //   Serial.println("Sending a command to SPA");
  //   digitalWrite(sendPIN, LOW);
  //   digitalWrite(RTS_PIN, HIGH);
  //   for (i = 0; i < commandToSendLength; i++) {
  //     //Serial.println("Writing a byte");
  //     mySerial.write(commandToSend[i]);
  //     //Serial.print(commandToSend[i], HEX);
  //   }
  //   delay(1);
  //   digitalWrite(RTS_PIN, LOW);
  //   digitalWrite(sendPIN, HIGH);
  // }
}