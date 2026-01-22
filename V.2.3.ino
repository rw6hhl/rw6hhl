#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// Упрощенные определения для экономии памяти
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Пины
#define RSSI_PIN A0
#define OUTPUT_PIN 12
#define MODE_BTN 2
#define PLUS_BTN 3
#define MINUS_BTN 4

// Структура для хранения параметров в EEPROM
struct Settings {
  int highThreshold;      // 0-5000
  int lowThreshold;       // 0-5000
  byte readingsCount;     // 1-10
  byte kerchunkTimer;     // 0-50 ×10 мс = 0-500 мс
  byte holdTime;         // 0-50 ×10 мс = 0-500 мс
  byte fragmentationTime; // 0-50 ×10 мс = 0-500 мс
  byte outputLevel;       // 0=LOW active, 1=HIGH active
  int version;           // Версия для проверки данных
};

Settings settings;
const int EEPROM_ADDR = 0;

// Таймер для автоматического закрытия меню (30 секунд)
unsigned long lastMenuInteraction = 0;
const unsigned long MENU_TIMEOUT = 30000; // 30 секунд

// Переменные для быстрой регулировки при долгом нажатии
unsigned long plusPressTime = 0;
unsigned long minusPressTime = 0;
const unsigned long FAST_ADJUST_DELAY = 500; // 500 мс до быстрой регулировки
const unsigned long FAST_ADJUST_INTERVAL = 100; // Интервал быстрой регулировки
unsigned long lastFastAdjust = 0;

// Режимы настройки
enum {
  MODE_DISPLAY,      // 0 - Мониторинг
  MODE_HIGH,         // 1 - Высокий порог (теперь первый)
  MODE_LOW,          // 2 - Низкий порог (теперь второй)
  MODE_COUNT,        // 3 - Количество показаний
  MODE_KERCHUNK,     // 4 - Защита от керчакинга
  MODE_HOLD,         // 5 - Время удержания
  MODE_FRAG,         // 6 - Время фрагментации
  MODE_OUTPUT_LEVEL, // 7 - Уровень на выходе
  MODE_COUNT_TOTAL   // 8 - Общее количество режимов
};

byte currentMode = MODE_DISPLAY;

// Состояния системы
enum {
  STATE_IDLE,    // Ожидание сигнала
  STATE_CHECK,   // Проверка сигнала
  STATE_ACTIVE,  // Активная передача
  STATE_FRAG,    // Фрагментация
  STATE_HOLD     // Удержание
};

byte currentState = STATE_IDLE;
bool outputActive = false;

// Уменьшенный буфер для измерений
#define READINGS_BUFFER_SIZE 5
int rssiReadings[READINGS_BUFFER_SIZE]; // теперь int для значений 0-5000
byte readingIndex = 0;

// Таймеры
unsigned long stateEntryTime = 0;
bool showWelcome = true;
unsigned long welcomeStartTime = 0;

// Функции для работы с EEPROM
void loadSettings() {
  EEPROM.get(EEPROM_ADDR, settings);
  
  // Проверяем валидность данных (версия = 1)
  if (settings.version != 1) {
    // Загружаем значения по умолчанию
    settings.highThreshold = 4000;     // 0-5000
    settings.lowThreshold = 3000;      // 0-5000
    settings.readingsCount = 3;        // 1-10
    settings.kerchunkTimer = 15;       // 0-50 ×10 мс = 0-500 мс
    settings.holdTime = 10;            // 0-50 ×10 мс = 0-500 мс
    settings.fragmentationTime = 20;   // 0-50 ×10 мс = 0-500 мс
    settings.outputLevel = 1;          // 0=LOW active, 1=HIGH active
    settings.version = 1;              // Устанавливаем версию
    
    saveSettings();
  }
}

void saveSettings() {
  settings.version = 1; // Всегда сохраняем версию 1
  EEPROM.put(EEPROM_ADDR, settings);
}

void setup() {
  // Минимальная инициализация
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  
  pinMode(RSSI_PIN, INPUT);
  pinMode(OUTPUT_PIN, OUTPUT);
  
  pinMode(MODE_BTN, INPUT_PULLUP);
  pinMode(PLUS_BTN, INPUT_PULLUP);
  pinMode(MINUS_BTN, INPUT_PULLUP);
  
  // Загружаем настройки из EEPROM
  EEPROM.begin();
  loadSettings();
  
  // Устанавливаем начальное состояние выхода
  digitalWrite(OUTPUT_PIN, settings.outputLevel == 1 ? LOW : HIGH);
  
  // Быстрая инициализация I2C
  Wire.begin();
  
  // Инициализация дисплея
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display.begin(SSD1306_EXTERNALVCC, 0x3C);
  }
  
  // Минимальная настройка дисплея
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  
  // Показать экран приветствия с версией V.2.3
  showWelcomeScreen();
  welcomeStartTime = millis();
  
  // Инициализация буфера
  for(byte i = 0; i < READINGS_BUFFER_SIZE; i++) {
    rssiReadings[i] = 0;
  }
  
  digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
  static unsigned long lastUpdate = 0;
  
  // Проверка времени показа приветствия (3 секунды)
  if(showWelcome && millis() - welcomeStartTime > 3000) {
    showWelcome = false;
    display.clearDisplay();
    display.display();
  }
  
  // Если показываем приветствие - не обрабатываем основную логику
  if(showWelcome) {
    delay(10);
    return;
  }
  
  // Проверка таймаута меню (30 секунд бездействия)
  if (currentMode != MODE_DISPLAY && (millis() - lastMenuInteraction > MENU_TIMEOUT)) {
    currentMode = MODE_DISPLAY;
    saveSettings(); // Сохраняем настройки при выходе из меню
  }
  
  // Чтение кнопок
  handleButtons();
  
  // Проверка долгого нажатия для быстрой регулировки
  checkFastAdjust();
  
  // Чтение RSSI (0-1023 → 0-5000)
  int rssiRaw = analogRead(RSSI_PIN);
  int rssiScaled = map(rssiRaw, 0, 1023, 0, 5000);
  
  // Для внутренней логики используем масштаб 0-50 (делим на 100)
  byte rssiForLogic = rssiScaled / 100;
  
  // Добавление в буфер
  rssiReadings[readingIndex] = rssiScaled;
  readingIndex = (readingIndex + 1) % READINGS_BUFFER_SIZE;
  
  // Подсчет валидных измерений (используем scaled/100 для сравнения с порогами/100)
  byte validCount = 0;
  for(byte i = 0; i < settings.readingsCount && i < READINGS_BUFFER_SIZE; i++) {
    byte idx = (readingIndex - 1 - i + READINGS_BUFFER_SIZE) % READINGS_BUFFER_SIZE;
    if((rssiReadings[idx] / 100) >= (settings.highThreshold / 100)) {
      validCount++;
    }
  }
  
  // Обработка состояний
  handleStateMachine(rssiForLogic, validCount);
  
  // Обновление дисплея каждые 200 мс
  if(millis() - lastUpdate > 200) {
    updateDisplay(rssiScaled, validCount);
    lastUpdate = millis();
  }
  
  // Индикация светодиодом
  if(outputActive) {
    digitalWrite(LED_BUILTIN, millis() % 1000 < 500);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
  }
  
  delay(10);
}

void showWelcomeScreen() {
  display.clearDisplay();
  
  // Большой заголовок
  display.setTextSize(2);
  display.setCursor(10, 5);
  display.println("RSSI");
  display.setCursor(10, 25);
  display.println("CTRL");
  
  // Версия V.2.0 (автоматически)
  display.setTextSize(1);
  display.setCursor(80, 20);
  display.println("V.2.3");
  
  // Разделительная линия
  display.drawFastHLine(0, 50, 128, WHITE);
  
  // Информация
  display.setCursor(0, 55);
  display.print("MODE:SET  +/-:ADJ");
  
  display.display();
}

void handleButtons() {
  static byte lastModeState = HIGH;
  static byte lastPlusState = HIGH;
  static byte lastMinusState = HIGH;
  
  byte modeState = digitalRead(MODE_BTN);
  byte plusState = digitalRead(PLUS_BTN);
  byte minusState = digitalRead(MINUS_BTN);
  
  // Кнопка MODE
  if(modeState == LOW && lastModeState == HIGH) {
    delay(50);
    
    // Если выходим из режима настройки - сохраняем
    if (currentMode != MODE_DISPLAY) {
      saveSettings();
    }
    
    currentMode = (currentMode + 1) % MODE_COUNT_TOTAL;
    
    // Сбрасываем таймер взаимодействия с меню
    if (currentMode != MODE_DISPLAY) {
      lastMenuInteraction = millis();
    }
    
    delay(300);
  }
  lastModeState = modeState;
  
  // Кнопка PLUS (короткое нажатие)
  if(plusState == LOW && lastPlusState == HIGH) {
    plusPressTime = millis(); // Запоминаем время нажатия
    delay(50);
    adjustParameter(1, false); // Одиночное нажатие
    lastMenuInteraction = millis(); // Сбрасываем таймер
  }
  
  // Кнопка MINUS (короткое нажатие)
  if(minusState == LOW && lastMinusState == HIGH) {
    minusPressTime = millis(); // Запоминаем время нажатия
    delay(50);
    adjustParameter(-1, false); // Одиночное нажатие
    lastMenuInteraction = millis(); // Сбрасываем таймер
  }
  
  // Отпускание кнопок
  if(plusState == HIGH && lastPlusState == LOW) {
    plusPressTime = 0; // Сбрасываем время нажатия
  }
  
  if(minusState == HIGH && lastMinusState == LOW) {
    minusPressTime = 0; // Сбрасываем время нажатия
  }
  
  lastPlusState = plusState;
  lastMinusState = minusState;
}

void checkFastAdjust() {
  unsigned long currentTime = millis();
  
  // Быстрая регулировка PLUS
  if (plusPressTime > 0 && (currentTime - plusPressTime > FAST_ADJUST_DELAY)) {
    if (currentTime - lastFastAdjust > FAST_ADJUST_INTERVAL) {
      adjustParameter(1, true); // Быстрая регулировка на 1 единиц
      lastFastAdjust = currentTime;
      lastMenuInteraction = currentTime; // Сбрасываем таймер
    }
  }
  
  // Быстрая регулировка MINUS
  if (minusPressTime > 0 && (currentTime - minusPressTime > FAST_ADJUST_DELAY)) {
    if (currentTime - lastFastAdjust > FAST_ADJUST_INTERVAL) {
      adjustParameter(-1, true); // Быстрая регулировка на 1 единиц
      lastFastAdjust = currentTime;
      lastMenuInteraction = currentTime; // Сбрасываем таймер
    }
  }
}

void adjustParameter(int delta, bool isFast) {
  switch(currentMode) {
    case MODE_HIGH:
      if (isFast) {
        settings.highThreshold = constrain(settings.highThreshold + (delta * 10), 
                                         settings.lowThreshold + 10, 5000);
      } else {
        settings.highThreshold = constrain(settings.highThreshold + delta, 
                                         settings.lowThreshold + 10, 5000);
      }
      break;
    case MODE_LOW:
      if (isFast) {
        settings.lowThreshold = constrain(settings.lowThreshold + (delta * 10), 
                                        0, settings.highThreshold - 10);
      } else {
        settings.lowThreshold = constrain(settings.lowThreshold + delta, 
                                        0, settings.highThreshold - 10);
      }
      break;
    case MODE_COUNT:
      if (isFast) {
        settings.readingsCount = constrain(settings.readingsCount + delta, 1, 10);
      } else {
        settings.readingsCount = constrain(settings.readingsCount + delta, 1, 10);
      }
      break;
    case MODE_KERCHUNK:
      if (isFast) {
        settings.kerchunkTimer = constrain(settings.kerchunkTimer + delta, 5, 50);
      } else {
        settings.kerchunkTimer = constrain(settings.kerchunkTimer + delta, 5, 50);
      }
      break;
    case MODE_HOLD:
      if (isFast) {
        settings.holdTime = constrain(settings.holdTime + delta, 5, 50);
      } else {
        settings.holdTime = constrain(settings.holdTime + delta, 5, 50);
      }
      break;
    case MODE_FRAG:
      if (isFast) {
        settings.fragmentationTime = constrain(settings.fragmentationTime + delta, 5, 50);
      } else {
        settings.fragmentationTime = constrain(settings.fragmentationTime + delta, 5, 50);
      }
      break;
    case MODE_OUTPUT_LEVEL:
      settings.outputLevel = constrain(settings.outputLevel + delta, 0, 1);
      // Немедленно обновляем выход
      if(!outputActive) {
        digitalWrite(OUTPUT_PIN, settings.outputLevel == 1 ? LOW : HIGH);
      }
      break;
  }
}

void handleStateMachine(byte rssiScaled, byte validCount) {
  unsigned long currentTime = millis();
  
  switch(currentState) {
    case STATE_IDLE:
      if(validCount >= settings.readingsCount) {
        currentState = STATE_CHECK;
        stateEntryTime = currentTime;
      }
      outputActive = false;
      digitalWrite(OUTPUT_PIN, settings.outputLevel == 1 ? LOW : HIGH);
      break;
      
    case STATE_CHECK:
      if(currentTime - stateEntryTime >= (settings.kerchunkTimer * 10)) {
        currentState = STATE_ACTIVE;
        outputActive = true;
        digitalWrite(OUTPUT_PIN, settings.outputLevel == 1 ? HIGH : LOW);
      }
      if((rssiScaled * 100) < settings.lowThreshold) {
        currentState = STATE_IDLE;
      }
      break;
      
    case STATE_ACTIVE:
      if((rssiScaled * 100) < settings.lowThreshold) {
        currentState = STATE_FRAG;
        stateEntryTime = currentTime;
      }
      break;
      
    case STATE_FRAG:
      if((rssiScaled * 100) >= settings.highThreshold) {
        currentState = STATE_ACTIVE;
      } else if(currentTime - stateEntryTime >= (settings.fragmentationTime * 10)) {
        currentState = STATE_HOLD;
        stateEntryTime = currentTime;
        outputActive = false;
        digitalWrite(OUTPUT_PIN, settings.outputLevel == 1 ? LOW : HIGH);
      }
      break;
      
    case STATE_HOLD:
      if((rssiScaled * 100) >= settings.highThreshold) {
        currentState = STATE_ACTIVE;
        outputActive = true;
        digitalWrite(OUTPUT_PIN, settings.outputLevel == 1 ? HIGH : LOW);
      } else if(currentTime - stateEntryTime >= (settings.holdTime * 10)) {
        currentState = STATE_IDLE;
      }
      break;
  }
}

void updateDisplay(int rssiScaled, byte validCount) {
  display.clearDisplay();
  
  if(currentMode == MODE_DISPLAY) {
    // ========== РЕЖИМ МОНИТОРИНГА ==========
    
    // Поднятая на строку надпись RSSI (строка 0)
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("RSSI");
    
    // Большое отображение RSSI в центре (0-5000) - поднято
    display.setTextSize(3);
    display.setCursor(25, 10); // Поднято на 5 пикселей
    if (rssiScaled < 1000) display.print(" ");
    display.print(rssiScaled);
    
    // Состояние выхода - поднято еще на одну строку
    display.setTextSize(1);
    display.setCursor(0, 38); // Было 40, стало 38 (поднято на 2 пикселя)
    display.print(outputActive ? "ON" : "OFF");
    
    // Простой график RSSI внизу (тонкая линия) - позиция не изменена
    int barWidth = map(rssiScaled, 0, 5000, 0, 120);
    display.drawRect(4, 50, 120, 7, WHITE);
    display.fillRect(4, 50, barWidth, 6, WHITE);
    
    // Маркеры порогов на графике с подписями L и H
    int highPos = map(settings.highThreshold, 0, 5000, 0, 120);
    int lowPos = map(settings.lowThreshold, 0, 5000, 0, 120);
    
    // Вертикальные линии маркеров
    display.drawFastVLine(4 + highPos, 50, 6, WHITE);
    display.drawFastVLine(4 + lowPos, 50, 6, WHITE);
    
    // Подписи L и H под маркерами
    display.setTextSize(1);
    
    // Подпись L (Low) - с проверкой чтобы не выйти за границы
    if (lowPos > 5) { // Если маркер не в самом начале
      display.setCursor(4 + lowPos - 2, 58); // Центрируем под маркером
      display.print("L");
    } else {
      display.setCursor(4 + lowPos, 58); // Если в начале, смещаем вправо
      display.print("L");
    }
    
    // Подпись H (High) - с проверкой чтобы не выйти за границы
    if (highPos < 115) { // Если маркер не в самом конце
      display.setCursor(4 + highPos - 2, 58); // Центрируем под маркером
      display.print("H");
    } else {
      display.setCursor(4 + highPos - 4, 58); // Если в конце, смещаем влево
      display.print("H");
    }
    
  } else {
    // ========== РЕЖИМ НАСТРОЙКИ ==========
    
    // Заголовок режима (верхняя строка)
    display.setTextSize(1);
    display.setCursor(0, 0);
    
    const char* modeTitles[] = {
      "MONITOR", "HIGH THR", "LOW THR", "READINGS", 
      "KERCHUNK", "HOLD TIME", "FRAG TIME", "OUTPUT"
    };
    
    if(currentMode < MODE_COUNT_TOTAL) {
      display.print(modeTitles[currentMode]);
    }
    
    // Таймер до закрытия меню
    unsigned long timeLeft = (MENU_TIMEOUT - (millis() - lastMenuInteraction)) / 1000;
    if (timeLeft <= 30) {
      display.setCursor(100, 0);
      display.print(timeLeft);
      display.print("s");
    }
    
    // Разделительная линия
    display.drawFastHLine(0, 10, 128, WHITE);
    
    // Большое отображение значения - поднято
    display.setCursor(0, 15); // Поднято на 5 пикселей
    display.setTextSize(2);  // Средний шрифт
    
    switch(currentMode) {
      case MODE_HIGH:
        display.print(settings.highThreshold);
        break;
      case MODE_LOW:
        display.print(settings.lowThreshold);
        break;
      case MODE_COUNT:
        display.setTextSize(3);
        display.print(settings.readingsCount);
        break;
      case MODE_KERCHUNK:
        display.print(settings.kerchunkTimer * 10);
        display.setTextSize(1);
        display.print("ms");
        break;
      case MODE_HOLD:
        display.print(settings.holdTime * 10);
        display.setTextSize(1);
        display.print("ms");
        break;
      case MODE_FRAG:
        display.print(settings.fragmentationTime * 10);
        display.setTextSize(1);
        display.print("ms");
        break;
      case MODE_OUTPUT_LEVEL:
        display.setTextSize(2);
        display.print(settings.outputLevel == 1 ? "HIGH" : "LOW");
        break;
    }
    
    // Единицы измерения - подняты на одну строку
    display.setTextSize(1);
    
    switch(currentMode) {
      case MODE_HIGH:
      case MODE_LOW:
        display.setCursor(0, 40); // Было 45, стало 40 (поднято на 5 пикселей)
        display.print("RSSI value (0-5000)");
        break;
      case MODE_COUNT:
        display.setCursor(0, 40); // Было 45, стало 40 (поднято на 5 пикселей)
        display.print("Measurements count");
        break;
      case MODE_KERCHUNK:
        display.setCursor(0, 40); // Было 45, стало 40 (поднято на 5 пикселей)
        display.print("Kerchunk protection");
        break;
      case MODE_HOLD:
        display.setCursor(0, 40); // Было 45, стало 40 (поднято на 5 пикселей)
        display.print("Hold time");
        break;
      case MODE_FRAG:
        display.setCursor(0, 40); // Было 45, стало 40 (поднято на 5 пикселей)
        display.print("Fragmentation time");
        break;
      case MODE_OUTPUT_LEVEL:
        display.setCursor(0, 40); // Было 45, стало 40 (поднято на 5 пикселей)
        display.print("Output active level");
        break;
    }
    
    // Инструкция внизу - поднята на одну строку
    display.setCursor(0, 55); // Было 58, стало 55 (поднято на 3 пикселя)
    display.print("MODE  +/-  LONG:FAST");
  }
  
  display.display();
}