// ============================================================
// Entwise Sprout Companion Sketch  v0.2 (librarified)
//
// All networking, config, reconfiguration, and topic plumbing now
// lives in sprout_api.h / sprout_api.cpp. This file is scaffolding
// only -- the same role sprout_api's own comment block describes.
// ============================================================

#include "sprout_api.h"

// -- [USER CONFIGURATION] ---------------------
Sprout_API sprout("Opta");

// -- Setup Section ----------------------------
// Code here runs once

void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("Program has begun.");

  sprout.initSprout();

  if (!sprout.connectWiFi()) {
    Serial.println("WiFi not connected at boot -- will keep retrying from loop().");
  }

  if (sprout.connectMQTT()) {
    sprout.applyTopicSubscriptions();
    sprout.publishOtaCompleteStatus();
  } else {
    Serial.println("MQTT not connected at boot -- will keep retrying from loop().");
  }
}

// --Main Program Loop-------------------------
// Code here runs continuously

void loop() {
  sprout.poll();
  // USER CODE
  // Write your code in the function call at the bottom of the sketch.
  // Update this call with any return values and inputs that you add.
  userLoop();
}

// ============================================================
// USER CODE
// ============================================================
// Read inputs:   sprout.sproutButton1(), sprout.sproutButton2(),
//                sprout.sproutInputNum1(), sprout.sproutInputNum2(),
//                sprout.sproutInputStr()
// Write outputs: sprout.sproutSetOutputStatus1(bool), sprout.sproutSetOutputStatus2(bool),
//                sprout.sproutSetOutputNum1(float), sprout.sproutSetOutputNum2(float),
//                sprout.sproutSetOutputStr(const char*)
//
// Runs once per loop() pass, right after MQTT messages are received and
// before outputs are published, so anything you set here goes out this
// same pass. Avoid delay() or long blocking calls here -- they'll stall
// MQTT servicing and WiFi reconnect logic for the whole device, not just
// your own code.
void userLoop() {
  // Starter example -- replace with your own logic:
  if (sprout.sproutButton1()) {
    sprout.sproutSetOutputStatus1(true);
  }
}
