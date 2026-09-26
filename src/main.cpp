#include <WiFiS3.h>
#include <ArduinoHA.h>
#include <Bounce2.h>
#include <secrets.h>

struct Action
{
  int direction;
  unsigned long duration; // ms to run, or 0 to run until the limit switch
  int power;
};

const int BRAKE_PIN = 9;
const int DIRECTION_PIN = 12;
const int LIMIT_OPEN_PIN = 2;
const int PWM_PIN = 3;
const int BUTTON_PIN = 7;
const unsigned long BUTTON_DEBOUNCE_MS = 50;

#define OPEN_DIRECTION LOW
#define CLOSE_DIRECTION HIGH

// Closing unwinds the string and lets the door drop; there is no closed
// limit switch, so the close runs for a fixed time.
const unsigned long CLOSE_RUN_MS = 4250;
// A close starts with the door resting on the open limit switch, and the
// switch bounces as the door lifts off it. Ignore the switch for this long
// after a close starts so that bounce cannot stop the motor.
const unsigned long CLOSE_LIMIT_ARM_MS = 750;

// Network timing. The WiFi and MQTT libraries block while they try to connect,
// so keep each attempt short and space them out. The button is polled between
// attempts, and no attempt is made while the motor is running. A hardware
// watchdog is deliberately not used: the RA4M1's maximum period is about
// 5.6 s, shorter than the radio's 10 s TCP timeout.
const unsigned long SERIAL_WAIT_MS = 2000;         // max wait for a debugging computer at boot
const unsigned long WIFI_JOIN_TIMEOUT_MS = 3000;   // max time WiFi.begin may block
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
const unsigned long MQTT_AFTER_WIFI_MS = 2000;     // let the connection settle before the first broker attempt
const unsigned long MQTT_RETRY_MIN_MS = 10000;     // matches the library's own reconnect interval
const unsigned long MQTT_RETRY_MAX_MS = 30000;     // retries back off from MIN to MAX

volatile unsigned long motorStartedAt = 0;
unsigned long motorRunFor = 0; // 0 = run until the limit switch
unsigned long lastWifiAttemptAt = 0;
unsigned long lastMqttAttemptAt = 0;
unsigned long mqttRetryInterval = MQTT_RETRY_MIN_MS;
bool wifiConnected = false; // read once per loop pass by maintainWifi()
unsigned long wifiConnectedSince = 0;

// STATE OF MOTOR
// These are written from the limit switch interrupt as well as the main
// loop, so they must be volatile.
volatile bool stateIsDirty = false;
volatile int motorDirection = OPEN_DIRECTION;
volatile int motorPower = 0;
bool forceStatePublish = false;

Bounce2::Button button;

WiFiClient client;
HADevice device;
HAMqtt mqtt(client, device);

HACover cover(UNIQUE_ID);

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
  // The brake shorts the motor windings. The gearbox cannot be back-driven,
  // so it is only engaged here for completeness when power is zero.
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
  motorStartedAt = millis();
  motorRunFor = a->duration;
  setMotorPower(a->power);
}

void stop()
{
  motorRunFor = 0;
  setMotorPower(0);
  cover.setState(HACover::StateStopped);
}

// Fires on the rising edge of the open limit switch. Opening winds the
// string up until the door trips the switch. If a close overruns, the
// string winds up the other way and trips the same switch, so stop in that
// direction too, once the arming delay has passed.
void onLimitOpen()
{
  if (!motorRunning())
  {
    return;
  }
  if (motorDirection == CLOSE_DIRECTION && millis() - motorStartedAt < CLOSE_LIMIT_ARM_MS)
  {
    return;
  }
  setMotorPower(0);
}

void close()
{
  Action a = {CLOSE_DIRECTION, CLOSE_RUN_MS, 100};
  setMotor(&a);
  cover.setState(HACover::StateClosing);
}

void open()
{
  Action a = {OPEN_DIRECTION, 0, 255};
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
  mqttRetryInterval = MQTT_RETRY_MIN_MS;
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
  button.update();
  if (button.pressed())
  {
    toggleDoor();
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
  if (connected != wifiConnected)
  {
    wifiConnected = connected;
    if (connected)
    {
      wifiConnectedSince = millis();
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
// motor-aware terms as WiFi. A failed TCP connect blocks for the radio's
// own timeout (about 10 s), so attempts are spaced out with a backoff.
void maintainMqtt()
{
  if (!wifiConnected)
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
  if (now - wifiConnectedSince < MQTT_AFTER_WIFI_MS)
  {
    return;
  }
  if (lastMqttAttemptAt != 0 && now - lastMqttAttemptAt < mqttRetryInterval)
  {
    return;
  }
  if (WiFi.localIP() == IPAddress((uint32_t)0))
  {
    return; // associated but no address yet
  }
  lastMqttAttemptAt = now;
  Serial.print("MQTT: connecting to ");
  Serial.println(MQTT_IP);
  mqtt.loop(); // triggers the library's connect attempt
  if (!mqtt.isConnected())
  {
    mqttRetryInterval = min(mqttRetryInterval * 2, MQTT_RETRY_MAX_MS);
  }
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
  button.attach(BUTTON_PIN, INPUT_PULLUP);
  button.interval(BUTTON_DEBOUNCE_MS);
  button.setPressedState(LOW);
  attachInterrupt(digitalPinToInterrupt(LIMIT_OPEN_PIN), onLimitOpen, RISING);

  cover.onCommand(onCoverCommand);
  cover.setName(DEVICE_NAME);

  byte mac[WL_MAC_ADDR_LENGTH];
  WiFi.macAddress(mac);
  device.setName(DEVICE_NAME);
  device.setUniqueId(mac, sizeof(mac));
  // Publish online/offline so Home Assistant marks the door unavailable when
  // the board drops off, instead of freezing on the last state it saw.
  device.enableSharedAvailability();
  device.enableLastWill();

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
  // Unsigned subtraction gives the correct elapsed time across millis() rollover.
  if (motorRunning() && motorRunFor > 0 && millis() - motorStartedAt >= motorRunFor)
  {
    stop();
  }

  handleButton();
  publishStateIfDirty();

  maintainWifi();
  maintainMqtt();
}
