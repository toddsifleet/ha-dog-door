#include <WiFiS3.h>
#include <ArduinoHA.h>
#include <secrets.h>

struct Action
{
  int direction;
  int duration; // ms to run
  int power;
};

const int BRAKE_PIN = 9;
const int DIRECTION_PIN = 12;
const int LIMIT_OPEN_PIN = 2;
const int PWM_PIN = 3;
const int CURRENT_PIN = A0;
const int BUTTON_PIN = 7;
bool buttonRead = false;

#define OPEN_DIRECTION LOW
#define CLOSE_DIRECTION HIGH

unsigned long stopMotorAt = 0;

// STATE OF MOTOR
bool stateIsDirty = false;

char motorDirection = OPEN_DIRECTION;
unsigned char motorPower = 0;

WiFiClient client;
HADevice device;
HAMqtt mqtt(client, device);

// "myCover" is unique ID of the cover. You should define your own ID.
HACover cover(UNIQUE_ID, HACover::PositionFeature);

bool isOpen()
{
  return digitalRead(LIMIT_OPEN_PIN);
}

HACover::CoverState getState()
{
  if (motorPower != 0)
  {
    return motorDirection == OPEN_DIRECTION ? HACover::StateOpening : HACover::StateClosing;
  }
  else if (isOpen())
  {
    return HACover::StateOpen;
  }
  else
  {
    return HACover::StateClosed;
  }
}

void connectToWifi()
{
  int connectionAttempts = 2;
  int wifiStatus = WL_IDLE_STATUS;
  while (wifiStatus != WL_CONNECTED)
  {
    if (connectionAttempts-- < 0)
    {
      return;
    }
    Serial.print("[QP] Attempting to connect to SSID: ");
    wifiStatus = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void setMotorPower(int power)
{
  stateIsDirty = true;
  motorPower = power;
  analogWrite(PWM_PIN, motorPower);
}

void setMotor(Action *a)
{
  // NOTE: I'm not sure if we want to use the break.
  if (a->power == 0)
  {
    digitalWrite(BRAKE_PIN, HIGH);
    return;
  }
  else
  {
    digitalWrite(BRAKE_PIN, LOW);
  }

  motorDirection = a->direction;

  digitalWrite(DIRECTION_PIN, motorDirection);
  setMotorPower(a->power);

  if (a->duration > 0)
  {
    stopMotorAt = millis() + a->duration * 50; // scales to allow for up to 10s
  }
  else
  {
    stopMotorAt = 0;
  }
}

void stop()
{
  stopMotorAt = 0;
  setMotorPower(0);
  cover.setState(HACover::StateStopped);
}

void onLimitOpen()
{
  if (motorDirection == OPEN_DIRECTION)
  {
    setMotorPower(0);
    stopMotorAt = 0;
  }
}

void close()
{
  Action a = {
    direction : CLOSE_DIRECTION,
    duration : 85,
    power : 100,
  };
  setMotor(&a);
  cover.setState(HACover::StateClosing);
}

void open()
{
  Action a = {
    direction : OPEN_DIRECTION,
    duration : 0,
    power : 255,
  };
  setMotor(&a);
  cover.setState(HACover::StateOpening);
}

void onCoverCommand(HACover::CoverCommand cmd, HACover *sender)
{
  if (cmd == HACover::CommandOpen && !isOpen())
  {
    open();
  }
  else if (cmd == HACover::CommandClose && isOpen())
  {
    close();
  }
  else if (cmd == HACover::CommandStop)
  {
    stop();
  }
}

void setup()
{
  Serial.begin(115200);
  while (!Serial)
  {
    ;
  }

  connectToWifi();

  pinMode(DIRECTION_PIN, OUTPUT);
  pinMode(PWM_PIN, OUTPUT);
  pinMode(BRAKE_PIN, OUTPUT);
  pinMode(LIMIT_OPEN_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LIMIT_OPEN_PIN), onLimitOpen, HIGH);

  cover.onCommand(onCoverCommand);

  byte mac[WL_MAC_ADDR_LENGTH];
  WiFi.macAddress(mac);
  device.setName("OakleyArduino");
  device.setUniqueId(mac, sizeof(mac));

  delay(2000);
  mqtt.begin(MQTT_IP, MQTT_USERNAME, MQTT_PASSWORD);
}

void toggleDoor()
{
  if (motorPower != 0)
  {
    stop();
  }
  else if (isOpen())
  {
    close();
  }
  else
  {
    open();
  }
}

void loop()
{
  unsigned long currentMillis = millis();
  // We need to protect the motor from running too long if millis rolls over.
  // so we just panic and stop the motor if millis < 10,000 (10 seconds, bootup time)
  if (stopMotorAt > 0 && (stopMotorAt < currentMillis || currentMillis < 10000))
  {
    stop();
  }
  if (stateIsDirty)
  {

    cover.setState(getState());
    stateIsDirty = false;
  }
  if (digitalRead(BUTTON_PIN) == LOW)
  {
    if (!buttonRead)
    {
      toggleDoor();
      buttonRead = true;
    }
  }
  else
  {
    buttonRead = false;
  }

  mqtt.loop();
}
