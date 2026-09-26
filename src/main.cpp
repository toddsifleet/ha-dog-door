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

// Network timing. The WiFi and MQTT libraries block while they try to connect,
// so keep each attempt short and space them out. The button is polled between
// attempts, and no attempt is made while the motor is running.
const unsigned long SERIAL_WAIT_MS = 2000;         // max wait for a debugging computer at boot
const unsigned long WIFI_JOIN_TIMEOUT_MS = 3000;   // max time WiFi.begin may block
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
const unsigned long MQTT_RETRY_INTERVAL_MS = 30000;

unsigned long stopMotorAt = 0;
unsigned long lastWifiAttemptAt = 0;
unsigned long lastMqttAttemptAt = 0;
bool wifiWasConnected = false;

// STATE OF MOTOR
volatile bool stateIsDirty = false;
bool forceStatePublish = false;

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

bool motorRunning()
{
  return motorPower != 0;
}

HACover::CoverState getState()
{
  if (motorRunning())
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

// Codes mirror PubSubClient's state(); see HAMqtt::ConnectionState.
const char *mqttStateName(HAMqtt::ConnectionState state)
{
  switch (state)
  {
  case HAMqtt::StateConnecting: return "connecting";
  case HAMqtt::StateConnectionTimeout: return "connection timeout";
  case HAMqtt::StateConnectionLost: return "connection lost";
  case HAMqtt::StateConnectionFailed: return "connection failed (TCP)";
  case HAMqtt::StateDisconnected: return "disconnected";
  case HAMqtt::StateConnected: return "connected";
  case HAMqtt::StateBadProtocol: return "bad protocol";
  case HAMqtt::StateBadClientId: return "bad client id";
  case HAMqtt::StateUnavailable: return "broker unavailable";
  case HAMqtt::StateBadCredentials: return "bad credentials";
  case HAMqtt::StateUnauthorized: return "unauthorized";
  default: return "unknown";
  }
}

void onMqttConnected()
{
  Serial.println("MQTT: connected");
  // Home Assistant may have restarted or missed updates; resend the current state.
  stateIsDirty = true;
  forceStatePublish = true;
}

void onMqttDisconnected()
{
  Serial.println("MQTT: disconnected");
}

void onMqttStateChanged(HAMqtt::ConnectionState state)
{
  Serial.print("MQTT: state ");
  Serial.print(state);
  Serial.print(" (");
  Serial.print(mqttStateName(state));
  Serial.println(")");
}


void toggleDoor()
{
  if (motorRunning())
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

void handleButton()
{
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
}

void publishStateIfDirty()
{
  if (!stateIsDirty || !mqtt.isConnected())
  {
    return;
  }
  if (cover.setState(getState(), forceStatePublish))
  {
    stateIsDirty = false;
    forceStatePublish = false;
  }
}

// Joins WiFi if needed. Blocks for at most WIFI_JOIN_TIMEOUT_MS, at most once
// per WIFI_RETRY_INTERVAL_MS, and never while the motor is running.
void maintainWifi()
{
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wifiWasConnected)
  {
    wifiWasConnected = connected;
    if (connected)
    {
      Serial.print("WiFi: connected, IP ");
      Serial.println(WiFi.localIP());
    }
    else
    {
      Serial.println("WiFi: not connected");
    }
  }
  if (connected || motorRunning())
  {
    return;
  }
  unsigned long now = millis();
  if (lastWifiAttemptAt != 0 && now - lastWifiAttemptAt < WIFI_RETRY_INTERVAL_MS)
  {
    return;
  }
  lastWifiAttemptAt = now;

  Serial.print("WiFi: joining ");
  Serial.println(WIFI_SSID);
  if (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED)
  {
    Serial.println("WiFi: join timed out, will keep checking");
  }
}

// Services the MQTT client when connected, and reconnects on the same
// rate-limited, motor-aware terms as WiFi.
void maintainMqtt()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }
  if (mqtt.isConnected())
  {
    mqtt.loop();
    return;
  }
  if (motorRunning())
  {
    return;
  }
  unsigned long now = millis();
  if (lastMqttAttemptAt != 0 && now - lastMqttAttemptAt < MQTT_RETRY_INTERVAL_MS)
  {
    return;
  }
  lastMqttAttemptAt = now;
  Serial.print("MQTT: connecting to ");
  Serial.println(MQTT_IP);
  mqtt.loop(); // triggers the library's connect attempt
}

void setup()
{
  Serial.begin(115200);
  // Give a debugging computer a moment to open the port so early prints show
  // up, but never hold a standalone boot longer than SERIAL_WAIT_MS.
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < SERIAL_WAIT_MS)
  {
  }

  // Hardware first, so the button works even if the network never comes up.
  pinMode(DIRECTION_PIN, OUTPUT);
  pinMode(PWM_PIN, OUTPUT);
  pinMode(BRAKE_PIN, OUTPUT);
  pinMode(LIMIT_OPEN_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LIMIT_OPEN_PIN), onLimitOpen, RISING);

  cover.onCommand(onCoverCommand);

  byte mac[WL_MAC_ADDR_LENGTH];
  WiFi.macAddress(mac);
  device.setName("OakleyArduino");
  device.setUniqueId(mac, sizeof(mac));

  WiFi.setTimeout(WIFI_JOIN_TIMEOUT_MS);
  mqtt.onConnected(onMqttConnected);
  mqtt.onDisconnected(onMqttDisconnected);
  mqtt.onStateChanged(onMqttStateChanged);
  mqtt.begin(MQTT_IP, MQTT_USERNAME, MQTT_PASSWORD);

  Serial.print("Dog door ready. WiFi radio firmware ");
  Serial.println(WiFi.firmwareVersion());
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

  handleButton();
  publishStateIfDirty();

  maintainWifi();
  maintainMqtt();
}
