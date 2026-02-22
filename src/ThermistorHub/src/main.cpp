#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <WebServer.h>

const int numChannels = 6;
// 計測に使用するピン D0 ~ D5
const int analogPins[numChannels] = {D0, D1, D2, D3, D4, D5};

// 回路とサーミスタのパラメータ
const float SERIES_RESISTOR = 47000.0;     // 分圧抵抗 47kΩ
const float NOMINAL_RESISTANCE = 10000.0;  // 25℃でのサーミスタ抵抗 10kΩ
const float NOMINAL_TEMPERATURE = 25.0;    // 基準温度 25℃
const float B_COEFFICIENT = 3950.0;        // B定数
const int ADC_MAX = 4095;                  // ESP32S3のADC分解能(12bit)

// --- 移動平均用の設定 ---
const int numReadings = 10;
int readings[numChannels][numReadings];
int readIndex = 0;
long total[numChannels];

// 最新の温度データを保持する配列（Webサーバー送信用）
float currentTemperatures[numChannels];

// --- SDMMCロギング用の設定 ---
const int SD_CLK = D10;
const int SD_CMD = D9;
const int SD_DAT0 = D8;
const char* logFileName = "/datalog.csv";
bool sdInitialized = false;

// --- Wi-Fi AP & Webサーバーの設定 ---
const char* ssid = "XIAO_TEMP_AP";       // スマホ等から見えるWi-Fiの名前
const char* password = "wasawasa";    // Wi-Fiのパスワード (8文字以上)
WebServer server(80);

// --- 非同期処理（タイマー）用の変数 ---
unsigned long lastMeasureTime = 0;
const unsigned long measureInterval = 1000; // 計測・記録の間隔（ミリ秒）

// プロトタイプ宣言
float calculateTemperature(float adcValue);
void appendFile(fs::FS &fs, const char * path, const char * message);
void handleRoot();
void handleData();

void setup() {
  // 電源が入ったら内蔵LEDをHIGHにする
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // LOWにして点灯（ESP32の内蔵LEDはアクティブLOW）

  Serial.begin(115200);
  analogReadResolution(12);

  // ピンと移動平均の初期化
  for (int i = 0; i < numChannels; i++) {
    pinMode(analogPins[i], INPUT);
    total[i] = 0;
    currentTemperatures[i] = 0.0;
    
    int initialVal = analogRead(analogPins[i]);
    for (int j = 0; j < numReadings; j++) {
      readings[i][j] = initialVal;
      total[i] += initialVal;
    }
  }

  // --- SDカードの初期化 ---
  Serial.println("SDカードをマウントしています...");
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_DAT0);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SDカードのマウントに失敗しました。");
  } else {
    Serial.println("SDカードが正常に認識されました。");
    sdInitialized = true;
    String header = "Time(ms),CH0,CH1,CH2,CH3,CH4,CH5\n";
    appendFile(SD_MMC, logFileName, header.c_str());
  }

  // --- Wi-Fi APの初期化 ---
  Serial.println("Wi-Fi APを起動しています...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IPアドレス: ");
  Serial.println(IP); // デフォルトでは 192.168.4.1

  // --- Webサーバーのルーティング設定 ---
  server.on("/", handleRoot);     // ブラウザでアクセスしたときのHTML画面
  server.on("/data", handleData); // JSが裏側でデータを取得するためのAPI
  server.begin();
  Serial.println("Webサーバーを開始しました。");
}

void loop() {
  // Webサーバーのクライアントからのリクエストを処理（常に呼び出す）
  server.handleClient();

  // 1000ミリ秒（1秒）経過したかチェックし、経過していたら計測・保存を実行
  unsigned long currentMillis = millis();
  if (currentMillis - lastMeasureTime >= measureInterval) {
    lastMeasureTime = currentMillis;

    String logData = String(currentMillis) + ","; 
    Serial.print("温度計測: ");
    
    for (int i = 0; i < numChannels; i++) {
      total[i] -= readings[i][readIndex];
      readings[i][readIndex] = analogRead(analogPins[i]);
      total[i] += readings[i][readIndex];
      
      float averageAdc = (float)total[i] / numReadings;
      float temperature = calculateTemperature(averageAdc);
      
      // グローバル配列に最新温度を保存（Webサーバー用）
      currentTemperatures[i] = temperature;

      Serial.print("CH");
      Serial.print(i);
      Serial.print(":");
      Serial.print(temperature, 1);
      Serial.print("*C  ");

      logData += String(temperature, 1);
      if (i < numChannels - 1) {
        logData += ",";
      }
    }
    Serial.println();
    logData += "\n"; 

    if (sdInitialized) {
      appendFile(SD_MMC, logFileName, logData.c_str());
    }

    readIndex = (readIndex + 1) % numReadings;
  }
}

// ==========================================
// Webサーバー用関数群
// ==========================================

// トップページ (HTML + JavaScript) を返す関数
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>XIAO リアルタイム温度モニター</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin-top: 50px; }
    .sensor-box { display: inline-block; margin: 10px; padding: 20px; border: 1px solid #ccc; border-radius: 10px; width: 120px; }
    .ch-name { font-size: 1.2em; color: #555; }
    .ch-temp { font-size: 2em; font-weight: bold; color: #d32f2f; margin-top: 10px; }
  </style>
  <script>
    // 1秒に1回、裏側で /data にアクセスして数値を更新する
    setInterval(() => {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          for(let i=0; i<6; i++) {
            document.getElementById('temp' + i).innerText = data['ch' + i].toFixed(1) + ' °C';
          }
        });
    }, 1000);
  </script>
</head>
<body>
  <h2>リアルタイム温度モニター</h2>
  <div id="sensors">
    <div class="sensor-box"><div class="ch-name">CH 0</div><div class="ch-temp" id="temp0">--.- °C</div></div>
    <div class="sensor-box"><div class="ch-name">CH 1</div><div class="ch-temp" id="temp1">--.- °C</div></div>
    <div class="sensor-box"><div class="ch-name">CH 2</div><div class="ch-temp" id="temp2">--.- °C</div></div>
    <div class="sensor-box"><div class="ch-name">CH 3</div><div class="ch-temp" id="temp3">--.- °C</div></div>
    <div class="sensor-box"><div class="ch-name">CH 4</div><div class="ch-temp" id="temp4">--.- °C</div></div>
    <div class="sensor-box"><div class="ch-name">CH 5</div><div class="ch-temp" id="temp5">--.- °C</div></div>
  </div>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

// 最新の温度データをJSON形式で返す関数 (JavaScriptから呼ばれる)
void handleData() {
  String json = "{";
  for (int i = 0; i < numChannels; i++) {
    json += "\"ch" + String(i) + "\":" + String(currentTemperatures[i], 1);
    if (i < numChannels - 1) json += ",";
  }
  json += "}";
  server.send(200, "application/json", json);
}

// ==========================================
// ユーティリティ・計算関数群
// ==========================================

void appendFile(fs::FS &fs, const char * path, const char * message) {
  File file = fs.open(path, FILE_APPEND);
  if(!file) {
    Serial.println("ファイルを開けませんでした");
    return;
  }
  file.print(message);
  file.close();
}

float calculateTemperature(float adcValue) {
  if (adcValue <= 0 || adcValue >= ADC_MAX) {
    return -999.0; 
  }
  float resistance = SERIES_RESISTOR * ((ADC_MAX - adcValue) / adcValue);
  float steinhart;
  steinhart = resistance / NOMINAL_RESISTANCE;
  steinhart = log(steinhart);
  steinhart /= B_COEFFICIENT;
  steinhart += 1.0 / (NOMINAL_TEMPERATURE + 273.15);
  steinhart = 1.0 / steinhart;
  steinhart -= 273.15;
  return steinhart;
}