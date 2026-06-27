#include <Wire.h>
#include <I2C_graphical_LCD_display.h>
#include <PCA9634.h>
#include <driver/i2s.h>
#include <SPI.h>
#include "SD_MMC.h"
#include "FS.h"
#include <tuple>
#include <ArduinoJson.h>
#include <Adafruit_MCP4725.h>

bool showPage(char* source[], const int first_line_id, bool inverse = false);


//размеры интерфейса
#define X_GAP 4
#define Y_GAP 8
#define LCD_WIDTH 128
#define LCD_HEIGHT 64
#define STRX_SIZE 6
#define STRY_SIZE 8
#define LINE_HEIGHT 1
#define LINE_COUNT (LCD_HEIGHT / STRY_SIZE)
#define SYMB_BY_WIDTH ((LCD_WIDTH - X_GAP * 2) / STRX_SIZE)
#define SYMB_BY_HEIGHT ((LCD_HEIGHT - Y_GAP * 2) / STRY_SIZE)
#define MAX_SYMBOLS_ON_SCREEN (SYMB_BY_WIDTH * SYMB_BY_HEIGHT)

//пины
#define OK_BUTTON 5
#define PREV_BUTTON 23
#define NEXT_BUTTON 15

//константы для прочитанного текста
#define TEXT_MAX_BYTES 8000
#define MAX_TEXTS_COUNT 20
#define TITLE_MAX_BYTES 50  // Максимум 50 байт под название (около 25 русских букв)
#define MAX_LINES TEXT_MAX_BYTES / SYMB_BY_WIDTH
#define TOTAL_QUESTIONS 2
#define TIMEOUT_MS 10000

enum DeviceState {
  STATE_TEXT_SELECT,  // Выбор текста из списка
  STATE_READING,      // Отображение текста на страницах
  STATE_QUESTIONS,    // Задать вопрос
  STATE_VARIANTS,     // Дать варианты ответа
  STATE_ANSWERS,      // Показать правильный ответ
  STATE_RESULTS       // Показ результатов после чтения
};


//Дисплей
I2C_graphical_LCD_display lcd;

//светодиод
PCA9634 testModule(0x1C);

//пищалка
Adafruit_MCP4725 buzzer;
int vol1 = 1000;
int vol2 = 100;
int ton;


DeviceState currentState = STATE_TEXT_SELECT;
int current_line = 0;        // Текущая страница текста

//для считывания текста
char currentLoadedText[TEXT_MAX_BYTES];
char currentLoadedTitle[TITLE_MAX_BYTES];
char* offset_titles[MAX_TEXTS_COUNT];
char* offset_list[MAX_LINES];

int total_lines;
int text_count;
const char END_MARKER_CHAR = '\0';
unsigned long start_time;
unsigned int correct_answers_count;
unsigned long total_speed;

//для вопросов
String loadedQuestions[TOTAL_QUESTIONS];
String loadedVariants[TOTAL_QUESTIONS];
String loadedAnswersText[TOTAL_QUESTIONS];
int loadedAnswers[TOTAL_QUESTIONS];
int question_id=0;


void setup() {
  Serial.begin(115200);
  delay(2000);
  uint32_t startTime = millis();
  while (!Serial && (millis() - startTime < 4000)) {
    delay(10);
  }

  Serial.println("\n--- Старт программы ---");

  //кнопки
  pinMode(OK_BUTTON, INPUT);
  pinMode(PREV_BUTTON, INPUT);
  pinMode(NEXT_BUTTON, INPUT);

  // запуск SD карты
  if (!SD_MMC.begin("/text", true)) {
    Serial.println("Ошибка инициализации SD-карты!");
    while (true);
  }
  Serial.println("SD-карта успешно подключена.");

  testModule.begin();
  lcd.begin();
  Wire.begin();
  buzzer.begin(0x60); 
  buzzer.setVoltage(0, false);

  for (int channel = 0; channel < testModule.channelCount(); channel++) {
    testModule.setLedDriverMode(channel, PCA9634_LEDOFF);
    // выключить светодиод в режиме 0/1
    testModule.write1(channel, 0x00);  // выключить все светодиоды в режиме ШИМ
    delay(200);
    testModule.setLedDriverMode(channel, PCA9634_LEDPWM);
    // установка режима ШИМ
  }

  lcd.frameRect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, 1, 1);
  text_count = setTitlesList();
  showPage(offset_titles, 0, true);
}


void loop() {

  static int last_ok_state = HIGH;
  static int last_prev_state = HIGH;
  static int last_next_state = HIGH;

  int ok_state = digitalRead(OK_BUTTON);
  int prev_state = digitalRead(PREV_BUTTON);
  int next_state = digitalRead(NEXT_BUTTON);

  switch (currentState) {
    case STATE_TEXT_SELECT:
      {
        // Листаем вниз
        if (last_next_state == HIGH && next_state == LOW && current_line < (text_count-1)) {
          current_line++;
          Serial.println("Далее");
          showPage(offset_titles, current_line, true);
          delay(50);
        }
        // Листаем вверх
        if (last_prev_state == HIGH && prev_state == LOW && current_line > 0) {
          current_line--;
          Serial.println("Назад");
          showPage(offset_titles, current_line, true);
          delay(50);
        }

        // Нажатие на "OK" — заканчиваем выбор текста
        if (last_ok_state == HIGH && ok_state == LOW) {
          loadTextAndTitleFromSD(current_line);
          total_lines = separate_text_by_lines();
          showPage(offset_list, 0); // Показываем первую страницу текста
          current_line = 0;
          currentState = STATE_READING; // Переходим к чтению
          RGB(13, 6, 15);
          start_time = millis();
          delay(50);
        }
        break;
      }
    case STATE_READING:
      {
        // Листаем страницы вперёд
        if (last_next_state == HIGH && next_state == LOW && current_line < (total_lines - SYMB_BY_HEIGHT)) {
          current_line+=SYMB_BY_HEIGHT;
          showPage(offset_list, current_line);
          delay(50);
        }
        // Листаем страницы назад
        if (last_prev_state == HIGH && prev_state == LOW && current_line >= SYMB_BY_HEIGHT) {
          current_line-=SYMB_BY_HEIGHT;
          showPage(offset_list, current_line);
          delay(50);
        }
        // Нажатие на "OK" — заканчиваем чтение
        if (last_ok_state == HIGH && ok_state == LOW) {
          unsigned long total_time = millis() - start_time;
          total_speed = 0;
          
          int byte_idx = 0;
  
          // Сканируем текст до тех пор, пока не встретим нуль-терминатор конца строки
          while (currentLoadedText[byte_idx] != '\0') {
            bool isCyr = false;
            utf8_to_single_byte(currentLoadedText[byte_idx], currentLoadedText[byte_idx + 1], isCyr);
            byte_idx += (isCyr) ? 2 : 1;
            total_speed++; // Прибавляем ровно 1 полноценный символ
          }
          Serial.println(total_speed);
          total_speed *= 1000;
          total_speed /= total_time;
          RGB(0, 0, 0);
          correct_answers_count = 0;
          currentState = STATE_QUESTIONS; // Переходим к вопросам
          send_message("GET_QUESTIONS");
          
          if (!get_message(currentLoadedText)) break;
          
          question_id=0;
          current_line=0;
          ask_question(0);
        }
        break;
      }
    
    case STATE_QUESTIONS: {
      // Листаем страницы вперёд
      if (last_next_state == HIGH && next_state == LOW && current_line < (total_lines - SYMB_BY_HEIGHT)) {
          current_line+=SYMB_BY_HEIGHT;
          showPage(offset_list, current_line);
          delay(50);
        }
        // Листаем страницы назад
        if (last_prev_state == HIGH && prev_state == LOW && current_line >= SYMB_BY_HEIGHT) {
          current_line-=SYMB_BY_HEIGHT;
          showPage(offset_list, current_line);
          delay(50);
        }
        // Нажатие на "OK" — переход к вариантам
        if (last_ok_state == HIGH && ok_state == LOW) {
          currentState = STATE_VARIANTS;
          showVariants(question_id);
        }
        break;
       }
    
    case STATE_VARIANTS: {
        // выбор варианта ответа
      if (last_next_state == HIGH && next_state == LOW) {
          showAnswers(question_id, 3);
        }
      if (last_prev_state == HIGH && prev_state == LOW) {
        showAnswers(question_id, 1);
      }
      if (last_ok_state == HIGH && ok_state == LOW) {
        showAnswers(question_id, 2);
      }
      break;
      }
    case STATE_ANSWERS: {
    if (last_ok_state == HIGH && ok_state == LOW) {
      question_id++;
      if (question_id==TOTAL_QUESTIONS) { // проверка, есть ли ещё вопросы
        clearLCD();
        currentState = STATE_RESULTS;
        showResults(total_speed, correct_answers_count);
        break;
      }
      //следующий вопрос
      currentState = STATE_QUESTIONS;
      current_line=0;
      ask_question(question_id);
    }
    break;
    }
    case STATE_RESULTS: {
      // нажали "ОК" - возврат в меню текстов
        if (last_ok_state == HIGH && ok_state == LOW) {
          showPage(offset_titles, 0, true);
          currentState = STATE_TEXT_SELECT;
          current_line=0;
          delay(200);
        }
        break;
      }
  last_ok_state = ok_state;
  last_prev_state = prev_state;
  last_next_state = next_state;
}
}

// генератор нот
void note(int type, int duration) { 
  switch (type) {
    case 1:  ton = 1000; break;
    case 2:  ton = 860;  break;
    case 3:  ton = 800;  break;
    case 4:  ton = 700;  break;
    case 5:  ton = 600;  break;
    case 6:  ton = 525;  break;
    case 7:  ton = 450;  break;
    case 8:  ton = 380;  break;
    case 9:  ton = 315;  break;
    case 10: ton = 250;  break;
    case 11: ton = 190;  break;
    case 12: ton = 130;  break;
    case 13: ton = 80;   break;
    case 14: ton = 30;   break;
    case 15: ton = 1;    break;
    default: ton = 0;    break;
  }
  
  if (ton == 0) return;

  // Воспроизведение звука с определенной тональностью и длительностью
  for (int i = 0; i < duration; i++) {
    buzzer.setVoltage(vol1, false);
    buzzer.setVoltage(vol2, false);
    delayMicroseconds(ton);
  }
}

// звук правильного ответа
void playCorrectSound() {
  Serial.println("Звук: Правильно!");
  // Короткие, идущие вверх чистые ноты (например, сначала низкая, потом высокая)
  note(5, 100); 
  delay(20);    // Микропауза между нотами
  note(1, 100); 
  delay(20);    // Микропауза между нотами
  note(14, 200); 

  
  buzzer.setVoltage(0, false); // Глушим звук в конце
}

// звук ложного ответа
void playWrongSound() {
  Serial.println("Звук: Ошибка!");
  // Тягучие, идущие вниз низкие тональности
  note(9, 250); 
  delay(40);    // Микропауза между нотами
  note(12, 350); 
  
  buzzer.setVoltage(0, false); // Глушим звук в конце
}

// функция вывода вопроса на экран
void ask_question(int i) {
  strlcpy(currentLoadedText, loadedQuestions[i].c_str(), TEXT_MAX_BYTES);
  total_lines = separate_text_by_lines();
  current_line = 0;
  showPage(offset_list, current_line);
}

// функция вывода вариантов на экран
void showVariants(int i) {
  strlcpy(currentLoadedText, loadedVariants[i].c_str(), TEXT_MAX_BYTES);
  total_lines = separate_text_by_lines();
  current_line = 0;
  showPage(offset_list, current_line);
  currentState = STATE_VARIANTS;
}

// функция вывода ответа на экран
void showAnswers(int i, int answer_id) {
  strlcpy(currentLoadedText, loadedAnswersText[i].c_str(), TEXT_MAX_BYTES);
  char* temp[2] = {currentLoadedText, "\0"};
  clearLCD();
  printLine(temp, 0, X_GAP, LCD_HEIGHT/2-STRY_SIZE);
  currentState = STATE_ANSWERS;
  if (loadedAnswers[i] == answer_id) {
    correct_answers_count++;
    playCorrectSound();
  }
  else {
    playWrongSound();
  }
}

// функция принятия json-файла с сервера
bool get_message(char* currentLoadedText) {
  bool success = false;
  JsonDocument doc;
  bool pcReady = false;
  unsigned long waitStart = millis();
  
  while (millis() - waitStart < 3000) { // Тайм-аут 3 секунды
    if (Serial.available() > 0) {
      String response = Serial.readStringUntil('\n');
      response.trim();
      
      if (response == "SEND_TEXT") {
        pcReady = true;
        break;
      }
    }
    delay(1);
  }

  if (pcReady) {
    Serial.println(currentLoadedText);
    Serial.flush();
  } else {
    Serial.println("ERROR: ПК не подтвердил начало операции");
  }
  waitStart = millis();
  while (millis() - waitStart < TIMEOUT_MS) {
      if (Serial.available() > 0) {
        String pcResponse = Serial.readStringUntil('\n');
        pcResponse.trim();
        
        DeserializationError error = deserializeJson(doc, pcResponse);
        if (!error) {
          success = true;
          break;
        }
      }
      delay(1);
    }

    if (success) {
      // Код, если компьютер ответил вовремя
      Serial.println("Успешно продолжено.");
      for (int i=0; i<TOTAL_QUESTIONS; i++) {
        loadedQuestions[i] = doc["questions"][i].as<String>();
        loadedVariants[i] = doc["variants"][i].as<String>();
        loadedAnswersText[i] = doc["answersText"][i].as<String>();
        loadedAnswers[i] = doc["answers"][i].as<int>();
      }
    } else {
      // Код на случай, если компьютер проигнорировал запрос или завис
      Serial.println("Ошибка: Компьютер не ответил за 5 секунд!");
      current_line=0;
      showPage(offset_titles, 0, true);
      currentState = STATE_TEXT_SELECT;
      delay(200);
      return false;
    }
  delay(200);
  return true;
}

// функция отправки сообщения на сервер
void send_message(String message) {
  Serial.println("MESSAGE:" + message);
  Serial.flush();
}


void RGB(byte r, byte g, byte b) {
  testModule.write1(3, r); // Красный 1
  testModule.write1(2, g); // Зеленый 1
  testModule.write1(5, b); // Синий 1
}


//записывает передданный заголовок и текст в файл под указанным id
void writeTextToSD(int textId, const char* title, const char* textData) {
  // Динамически собираем имя файла, например "/text5.txt"
  String fileName = "/text" + String(textId) + ".txt";

  // FILE_WRITE полностью перезаписывает файл (и создает его, если файла нет)
  File myFile = SD_MMC.open(fileName.c_str(), FILE_WRITE);
  if (myFile) {
    myFile.println(title);
    myFile.print(textData);
    myFile.close();
    Serial.println("Файл сохранён");
  }
}

//Сохраняет заголовок и текст из файла в currentLoadedTitle и currentLoadedText соотв.
bool loadTextAndTitleFromSD(int targetId) {
  String fileName = "/text" + String(targetId) + ".txt";

  File myFile = SD_MMC.open(fileName.c_str(), FILE_READ);
  if (!myFile) {
    currentLoadedTitle[0] = '\0';
    currentLoadedText[0] = '\0';
    return false;
  }

  int byte_idx = 0;

  // читаем заголовок(первая строка)
  while (myFile.available() && byte_idx < (TITLE_MAX_BYTES - 1)) {
    char ch = myFile.read();
    if (ch == '\n' || ch == '\r') {
      if (ch == '\r' && myFile.peek() == '\n') myFile.read();  // Пропускаем \n из пары \r\n
      break;
    }
    currentLoadedTitle[byte_idx++] = ch;
  }
  currentLoadedTitle[byte_idx] = '\0';

  // читаем текст
  byte_idx = 0;
  while (myFile.available() && byte_idx < (TEXT_MAX_BYTES - 1)) {
    currentLoadedText[byte_idx++] = myFile.read();
  }
  currentLoadedText[byte_idx] = '\0';

  myFile.close();
  Serial.println("Файл открыт");
  return (byte_idx > 0);  // Вернет true, если текст успешно считан
}

//Сохраняет заголовок из файла в currentLoadedTitle
bool loadTitleFromSD(int targetId) {
  String fileName = "/text" + String(targetId) + ".txt";
  currentLoadedText[0] = '\0';
  File myFile = SD_MMC.open(fileName.c_str(), FILE_READ);
  if (!myFile) {
    currentLoadedTitle[0] = '\0';
    return false;
  }

  int byte_idx = 0;

  while (myFile.available() && byte_idx < (TITLE_MAX_BYTES - 1)) {
    char ch = myFile.read();
    if (ch == '\n' || ch == '\r') {
      if (ch == '\r' && myFile.peek() == '\n') myFile.read(); 
      break;
    }
    currentLoadedTitle[byte_idx++] = ch;
  }
  currentLoadedTitle[byte_idx] = '\0';

  myFile.close();
  Serial.println("Заголовок открыт");
  return (byte_idx > 0);  // Вернет true, если текст успешно считан
}

//функция. Проверяет на то, кириллица ли. (p.s. Замена на однобайтный символ не работает)
char utf8_to_single_byte(unsigned char current_byte, unsigned char next_byte, bool& is_cyrillic) {
  is_cyrillic = false;
  if (current_byte == 0xD0) {
    is_cyrillic = true;
    if (next_byte == 0x81) return 0xA8;                                     // Ё
    if (next_byte >= 0x90 && next_byte <= 0xBF) return (next_byte + 0x30);  // А-Я, а-п
  } else if (current_byte == 0xD1) {
    is_cyrillic = true;
    if (next_byte == 0x91) return 0xB8;                                     // ё
    if (next_byte >= 0x80 && next_byte <= 0x8F) return (next_byte + 0x70);  // р-я
  }
  return current_byte;
}

//offset_list и currentLoadedText глобальные
// разделяет currentLoadedText по строкам и сохраняет в offset_list
  int separate_text_by_lines() {
    int row = 0;
    int byte_idx = 0;
    while (currentLoadedText[byte_idx] != '\0') {
      offset_list[row] = &currentLoadedText[byte_idx];
      for (int i = 0; i < SYMB_BY_WIDTH; i++) {
        bool isCyr = false;
        if (currentLoadedText[byte_idx] == '\0') {
          break;
        }

        if (currentLoadedText[byte_idx] == '\n' || currentLoadedText[byte_idx] == '\r') {
          byte_idx++;                         
        }
        utf8_to_single_byte(currentLoadedText[byte_idx], currentLoadedText[byte_idx + 1], isCyr);
        byte_idx += (isCyr) ? 2 : 1;
      }
      row++;
      if (row == MAX_LINES) break;
    }
    offset_list[row] = &currentLoadedText[byte_idx];
    return row;
  }

String limitStringWidth(String text, int max_symbols) {
  int byte_idx = 0;
  int symbols_count = 0;
  int cut_byte_idx = -1; // Байт, где заканчиваются первые (max_symbols - 3) букв

  // считаем реальные буквы в UTF-8
  while (byte_idx < text.length()) {

    if (symbols_count == (max_symbols - 3)) {
      cut_byte_idx = byte_idx;
    }

    bool isCyr = false;
    char next_char = (byte_idx + 1 < text.length()) ? text[byte_idx + 1] : '\0';
    utf8_to_single_byte(text[byte_idx], next_char, isCyr);
    
    byte_idx += (isCyr) ? 2 : 1;
    symbols_count++;
  }

  // если букв больше лимита - обрезаем и добавляем точки
  if (symbols_count > max_symbols && cut_byte_idx != -1) {
    text = text.substring(0, cut_byte_idx) + "...";
  }

  return text;
}


int setTitlesList() {
  // очистка
  for (int i = 0; i < MAX_TEXTS_COUNT; i++) {
    if (offset_titles[i] != nullptr && offset_titles[i] != &END_MARKER_CHAR) {
      free(offset_titles[i]);
      offset_titles[i] = nullptr;
    } else {
      break; 
    }
  }

  int targetId = 0;
  
  // загрузка заголовков с SD
  while (targetId < MAX_TEXTS_COUNT - 1 && loadTitleFromSD(targetId)) {
    String text_to_copy = limitStringWidth(String(currentLoadedTitle), SYMB_BY_WIDTH);
    offset_titles[targetId] = strdup(text_to_copy.c_str());
    targetId++;
  }
  
  offset_titles[targetId] = (char*)&END_MARKER_CHAR;
  return targetId;
}

// вывод строки
void printLine(char* source[], int line_index, int x, int y) {
  char* start_ptr = (char*)source[line_index];
  char* end_ptr = (char*)source[line_index + 1];

  // Запоминаем оригинальный символ, где начинается следующая строка
  char temporary_char = *end_ptr;
  
  if (*end_ptr != '\0') *end_ptr = '\0';
  lcd.gotoxy(x, y);
  lcd.string(start_ptr); // Выводим всю строку
  if (temporary_char != '\0')*end_ptr = temporary_char; // Возвращаем оригинальный символ
}


// вывод страницы
bool showPage(char* source[], const int first_line_id, bool inverse) {
  clearLCD();
  for (int i = 0; i < SYMB_BY_HEIGHT; i++) {
    if ((first_line_id + i) == MAX_LINES || *source[first_line_id + i] == '\0') return false;
    if (inverse && i==0) {
      lcd.setInv(true);
      printLine(source, first_line_id + i, X_GAP, Y_GAP + i * STRY_SIZE);
      lcd.setInv(false);
      continue;
      }
    printLine(source, first_line_id + i, X_GAP, Y_GAP + i * STRY_SIZE);
  }
  return true;
}

void showResults(float speed, int rate) {
  clearLCD();
  lcd.gotoxy(X_GAP * 2, LCD_HEIGHT / 2 - STRY_SIZE - Y_GAP);
  lcd.string(("Скорость: " + String(speed)).c_str());
  lcd.gotoxy(X_GAP * 2, LCD_HEIGHT / 2);
  lcd.string(("Оценка: " + String(rate) + "/" + String(TOTAL_QUESTIONS)).c_str());
}

//очистка экрана
void clearLCD() {
  for (byte y = STRY_SIZE; y < LCD_HEIGHT - STRY_SIZE; y += STRY_SIZE) {
    lcd.gotoxy(1, y);

    // Линейно заполняем строку байтами от левой до правой рамки
    for (byte x = 1; x < LCD_WIDTH - 1; x++) {
      lcd.writeData(0x00);
    }
  }
}
