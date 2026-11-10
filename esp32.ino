#include <ArduinoJson.h>
#include <DHT.h>

// Định nghĩa chân GPIO theo sơ đồ phần cứng ESP32
constexpr int DHT_PIN = 14;
constexpr int LED_RED_PIN = 26;
constexpr int LED_GREEN_PIN = 27;
// Motor 1 in the schematic is driven by an A4950 H-bridge (ESP32 GPIO18/19).
constexpr int PUMP_IN1_PIN = 19; // MOTOR1_INT1 theo schematic
constexpr int PUMP_IN2_PIN = 18; // MOTOR1_INT2 theo schematic
constexpr int LED_STRIP_1_PIN = 13;
constexpr int LED_STRIP_2_PIN = 12;
constexpr int BUZZER_PIN = 21;
constexpr int RS485_RX_PIN = 4;
constexpr int RS485_TX_PIN = 2;
constexpr int RS485_DIRECTION_PIN = 25; // LOW: Nhận, HIGH: Truyền

constexpr unsigned long TELEMETRY_INTERVAL_MS = 1000;
constexpr unsigned long SENSOR_INTERVAL_MS = 2000;
constexpr unsigned long PUMP_MAX_RUN_MS = 20000; // 20 giây cho mô hình
constexpr unsigned long SENSOR_STALE_MS = 6000;

DHT dht(DHT_PIN, DHT11);
HardwareSerial rs485(1);

// Biến trạng thái ESP32
float temperature = NAN;
float humidity = NAN;
bool autoMode = false;
float autoThreshold = 30.0f;
bool pumpOn = false;
bool pumpRequested = false;
enum class PumpState : uint8_t { STOP, RUN, ERROR };
PumpState pumpState = PumpState::STOP;
const char *pumpFault = "";
unsigned long pumpStartedAt = 0;
unsigned long lastValidSensorAt = 0;
bool ledRedOn = false;
bool ledGreenOn = false;
bool ledStripOn = false;
bool ledBlinking = false;
bool songPlaying = false;
const char *currentSong = nullptr;

// Biến trạng thái từ Arduino qua RS485
float arduinoTemperature = NAN;
float arduinoHumidity = NAN;
int rainRaw = -1;
bool heaterOn = false;
int servoAngle = 0;
int ledArray1 = 0;
int ledArray2 = 0;

unsigned long lastSensorRead = 0;
unsigned long lastTelemetry = 0;
unsigned long lastArduinoMessage = 0;
unsigned long lastBlink = 0;
unsigned long nextNoteAt = 0;
int blinkCount = 0;
int noteIndex = 0;
int currentSongIndex = -1;
String usbLine;
String rs485Line;

struct Note {
  int frequency;
  int duration;
};

const Note happyBirthday[] = {
  {262, 300}, {262, 300}, {294, 600}, {262, 600}, {349, 600}, {330, 1000},
  {262, 300}, {262, 300}, {294, 600}, {262, 600}, {392, 600}, {349, 1000}
};
const Note twinkleTwinkle[] = {
  {262, 500}, {262, 500}, {392, 500}, {392, 500}, {440, 500}, {440, 500},
  {392, 900}, {349, 500}, {349, 500}, {330, 500}, {330, 500}, {294, 500}
};
const Note jingleBells[] = {
  {330, 350}, {330, 350}, {330, 700}, {330, 350}, {330, 350}, {330, 700},
  {330, 350}, {392, 350}, {262, 500}, {294, 200}, {330, 900}
};

const Note *songNotes() {
  if (currentSongIndex == 0) return happyBirthday;
  if (currentSongIndex == 1) return twinkleTwinkle;
  if (currentSongIndex == 2) return jingleBells;
  return nullptr;
}

int songLength() {
  if (currentSongIndex == 0) return sizeof(happyBirthday) / sizeof(happyBirthday[0]);
  if (currentSongIndex == 1) return sizeof(twinkleTwinkle) / sizeof(twinkleTwinkle[0]);
  if (currentSongIndex == 2) return sizeof(jingleBells) / sizeof(jingleBells[0]);
  return 0;
}

void setLeds(bool red, bool green, bool strip) {
  ledRedOn = red;
  ledGreenOn = green;
  ledStripOn = strip;
  digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, green ? HIGH : LOW);
  digitalWrite(LED_STRIP_1_PIN, strip ? HIGH : LOW);
  digitalWrite(LED_STRIP_2_PIN, strip ? HIGH : LOW);
}

void setPump(bool enabled) {
  // This records intent only. updateState() is the sole owner of the outputs.
  pumpRequested = enabled;
}

const char *pumpStateName() {
  switch (pumpState) {
    case PumpState::RUN: return "RUN";
    case PumpState::ERROR: return "ERROR";
    default: return "STOP";
  }
}

bool sensorInputValid(unsigned long now) {
  return isfinite(temperature) && isfinite(humidity) &&
         temperature >= -40.0f && temperature <= 80.0f &&
         humidity >= 0.0f && humidity <= 100.0f &&
         lastValidSensorAt != 0 && now - lastValidSensorAt <= SENSOR_STALE_MS;
}

void enterPumpError(const char *reason) {
  pumpState = PumpState::ERROR;
  pumpFault = reason;
  pumpRequested = false;
}

// Gọi mỗi vòng loop. Không block; tất cả giới hạn dùng millis().
void updateState(unsigned long now) {
  const bool validInput = sensorInputValid(now);

  if (pumpState == PumpState::RUN) {
    if (!validInput) {
      enterPumpError("invalid_sensor");
    } else if (now - pumpStartedAt >= PUMP_MAX_RUN_MS) {
      pumpRequested = false;
      pumpState = PumpState::STOP;
      pumpFault = "";
    } else if (!pumpRequested) {
      pumpState = PumpState::STOP;
      pumpFault = "";
    }
  } else if (pumpState == PumpState::STOP && pumpRequested) {
    if (validInput) {
      pumpState = PumpState::RUN;
      pumpStartedAt = now;
      pumpFault = "";
    } else {
      enterPumpError("invalid_sensor");
    }
  } else if (pumpState == PumpState::ERROR && !pumpRequested) {
    // Lỗi được chốt; lệnh OFF xác nhận dừng và xóa lỗi.
    // Lệnh ON không thể khởi động lại nếu chưa gửi OFF.
  }

  pumpOn = pumpState == PumpState::RUN;
  // A4950: IN1=HIGH, IN2=LOW chạy một chiều; LOW/LOW nhả motor.
  digitalWrite(PUMP_IN1_PIN, pumpOn ? HIGH : LOW);
  digitalWrite(PUMP_IN2_PIN, LOW);
}

void stopSong() {
  noTone(BUZZER_PIN);
  songPlaying = false;
  currentSong = nullptr;
  currentSongIndex = -1;
}

void startSong(const char *song, int index) {
  currentSong = song;
  currentSongIndex = index;
  noteIndex = 0;
  songPlaying = true;
  nextNoteAt = 0;
}

// Chuyển tiếp lệnh điều khiển từ ESP32 sang Arduino qua bus RS485
void sendToArduino(const String &cmd) {
  digitalWrite(RS485_DIRECTION_PIN, HIGH); // Bật chế độ truyền RS485
  delayMicroseconds(200);
  rs485.println(cmd);
  rs485.flush();
  delayMicroseconds(200);
  digitalWrite(RS485_DIRECTION_PIN, LOW);  // Quay lại chế độ nhận RS485
}

void sendTelemetry(unsigned long now);

// Xử lý các lệnh điều khiển nhận từ USB Serial (Server Web)
void handleUsbCommand(const String &line) {
  StaticJsonDocument<384> command;
  DeserializationError err = deserializeJson(command, line);
  if (err) {
    Serial.println("{\"type\":\"error\",\"message\":\"invalid_json\"}");
    return;
  }

  const char *type = command["type"] | "";

  if (strcmp(type, "query") == 0 || strcmp(type, "ping") == 0) {
    sendTelemetry(millis());
  } else if (strcmp(type, "setPump") == 0) {
    bool enabled = command["enabled"] | false;
    if (!enabled) {
      pumpRequested = false;
      if (pumpState == PumpState::ERROR) {
        pumpState = PumpState::STOP;
        pumpFault = "";
      }
    } else if (pumpState != PumpState::ERROR) {
      setPump(true);
    }
    updateState(millis());
    sendTelemetry(millis());
  } else if (strcmp(type, "setAutoMode") == 0) {
    autoMode = command["enabled"] | false;
    sendTelemetry(millis());
  } else if (strcmp(type, "setThreshold") == 0) {
    const float value = command["value"] | -1.0f;
    if (value >= 0.0f && value <= 60.0f) {
      autoThreshold = value;
    }
    sendTelemetry(millis());
  } else if (strcmp(type, "setLed") == 0) {
    const bool enabled = command["enabled"] | false;
    const char *name = command["name"] | "";
    ledBlinking = false;
    if (strcmp(name, "ledRed") == 0) {
      ledRedOn = enabled;
      digitalWrite(LED_RED_PIN, enabled ? HIGH : LOW);
    } else if (strcmp(name, "ledGreen") == 0) {
      ledGreenOn = enabled;
      digitalWrite(LED_GREEN_PIN, enabled ? HIGH : LOW);
    } else if (strcmp(name, "ledStrip") == 0) {
      ledStripOn = enabled;
      digitalWrite(LED_STRIP_1_PIN, enabled ? HIGH : LOW);
      digitalWrite(LED_STRIP_2_PIN, enabled ? HIGH : LOW);
    }
    sendTelemetry(millis());
  } else if (strcmp(type, "ledPreset") == 0) {
    const char *preset = command["preset"] | "";
    if (strcmp(preset, "allOn") == 0) {
      ledBlinking = false;
      setLeds(true, true, true);
    } else if (strcmp(preset, "allOff") == 0) {
      ledBlinking = false;
      setLeds(false, false, false);
    } else if (strcmp(preset, "blink") == 0) {
      ledBlinking = true;
      blinkCount = 0;
      lastBlink = 0;
    }
    sendTelemetry(millis());
  } else if (strcmp(type, "playSong") == 0) {
    const char *song = command["song"] | "";
    if (strcmp(song, "happy-birthday") == 0) startSong("happy-birthday", 0);
    else if (strcmp(song, "twinkle-twinkle") == 0) startSong("twinkle-twinkle", 1);
    else if (strcmp(song, "jingle-bells") == 0) startSong("jingle-bells", 2);
    sendTelemetry(millis());
  } else if (strcmp(type, "stopSong") == 0) {
    stopSong();
    sendTelemetry(millis());
  } 
  // Chuyển tiếp các lệnh điều khiển phần cứng của Arduino qua RS485
  else if (strcmp(type, "setHeater") == 0 ||
           strcmp(type, "setServo") == 0 ||
           strcmp(type, "setArrayLed") == 0) {
    sendToArduino(line);
  }
}

void readUsbCommands() {
  while (Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\n') {
      handleUsbCommand(usbLine);
      usbLine = "";
    } else if (value != '\r' && usbLine.length() < 380) {
      usbLine += value;
    }
  }
}

// Đọc dữ liệu từ Arduino gửi về qua RS485
void readArduinoMessages() {
  while (rs485.available()) {
    const char value = static_cast<char>(rs485.read());
    if (value == '\n') {
      StaticJsonDocument<384> message;
      if (!deserializeJson(message, rs485Line) &&
          strcmp(message["type"] | "", "arduinoTelemetry") == 0) {
        if (!message["temp"].isNull()) {
          arduinoTemperature = message["temp"].as<float>();
        }
        if (!message["humidity"].isNull()) {
          arduinoHumidity = message["humidity"].as<float>();
        }
        rainRaw = message["rainRaw"] | -1;
        heaterOn = message["heater"] | false;
        if (!message["servoAngle"].isNull()) {
          servoAngle = message["servoAngle"].as<int>();
        }
        ledArray1 = message["ledArray1"] | 0;
        ledArray2 = message["ledArray2"] | 0;

        lastArduinoMessage = millis();
      }
      rs485Line = "";
    } else if (value != '\r' && rs485Line.length() < 380) {
      rs485Line += value;
    }
  }
}

void updateBuzzer(unsigned long now) {
  if (!songPlaying) return;
  if (now < nextNoteAt) return;

  if (noteIndex >= songLength()) {
    stopSong();
    return;
  }

  const Note note = songNotes()[noteIndex++];
  if (note.frequency > 0) tone(BUZZER_PIN, note.frequency, note.duration);
  nextNoteAt = now + static_cast<unsigned long>(note.duration + 70);
}

void updateBlink(unsigned long now) {
  if (!ledBlinking || now - lastBlink < 350) return;
  lastBlink = now;
  const bool enabled = blinkCount % 2 == 0;
  setLeds(enabled, enabled, enabled);
  blinkCount++;
  if (blinkCount >= 10) {
    ledBlinking = false;
    setLeds(false, false, false);
  }
}

// Đóng gói và gửi toàn bộ telemetry lên Web Server qua USB Serial
void sendTelemetry(unsigned long now) {
  StaticJsonDocument<512> message;
  message["type"] = "telemetry";

  if (isfinite(temperature)) message["temp"] = temperature;
  else message["temp"] = nullptr;

  if (isfinite(humidity)) message["humidity"] = humidity;
  else message["humidity"] = nullptr;

  message["pump"] = pumpOn;
  message["pumpState"] = pumpStateName();
  message["pumpFault"] = pumpFault[0] ? pumpFault : nullptr;
  message["ledRed"] = ledRedOn;
  message["ledGreen"] = ledGreenOn;
  message["ledStrip"] = ledStripOn;
  message["autoMode"] = autoMode;
  message["autoThreshold"] = autoThreshold;
  message["buzzerSong"] = songPlaying ? currentSong : nullptr;

  // Kiểm tra kết nối RS485 với Arduino trong vòng 6 giây gần nhất
  const bool ardConn = (lastArduinoMessage != 0 && (now - lastArduinoMessage < 6000));
  message["arduinoConnected"] = ardConn;

  if (ardConn && isfinite(arduinoTemperature)) message["arduinoTemp"] = arduinoTemperature;
  else message["arduinoTemp"] = nullptr;

  if (ardConn && isfinite(arduinoHumidity)) message["arduinoHumidity"] = arduinoHumidity;
  else message["arduinoHumidity"] = nullptr;

  if (ardConn && rainRaw >= 0) message["rainRaw"] = rainRaw;
  else message["rainRaw"] = nullptr;

  message["heater"] = heaterOn;
  message["servoAngle"] = servoAngle;
  message["ledArray1"] = ledArray1;
  message["ledArray2"] = ledArray2;

  serializeJson(message, Serial);
  Serial.println();
}

void setup() {
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  // Nạp mức an toàn vào latch trước khi bật chế độ OUTPUT.
  digitalWrite(PUMP_IN1_PIN, LOW);
  digitalWrite(PUMP_IN2_PIN, LOW);
  pinMode(PUMP_IN1_PIN, OUTPUT);
  pinMode(PUMP_IN2_PIN, OUTPUT);
  pinMode(LED_STRIP_1_PIN, OUTPUT);
  pinMode(LED_STRIP_2_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RS485_DIRECTION_PIN, OUTPUT);

  // Mặc định chân RS485 ở chế độ nhận (LOW)
  digitalWrite(RS485_DIRECTION_PIN, LOW);
  setLeds(false, false, false);
  setPump(false);
  noTone(BUZZER_PIN);

  Serial.begin(115200);
  rs485.begin(115200, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  dht.begin();
}

void loop() {
  const unsigned long now = millis();
  readUsbCommands();
  readArduinoMessages();

  // Đọc cảm biến nhiệt độ & độ ẩm định kỳ
  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;
    const float newTemperature = dht.readTemperature();
    const float newHumidity = dht.readHumidity();
    if (isfinite(newTemperature) && isfinite(newHumidity) &&
        newTemperature >= -40.0f && newTemperature <= 80.0f &&
        newHumidity >= 0.0f && newHumidity <= 100.0f) {
      temperature = newTemperature;
      humidity = newHumidity;
      lastValidSensorAt = now;
    } else {
      temperature = NAN;
      humidity = NAN;
    }

    // Chế độ tự động cảnh báo nhiệt độ
    if (autoMode && !ledBlinking && isfinite(temperature)) {
      const bool isHot = temperature >= autoThreshold;
      setLeds(isHot, !isHot, ledStripOn);
    }
  }

  updateBlink(now);
  updateBuzzer(now);
  updateState(now);

  // Gửi telemetry định kỳ lên máy tính
  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = now;
    sendTelemetry(now);
  }
}
