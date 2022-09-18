// Copyright (c) 2022 Akihiro Yamamoto.
// Licensed under the MIT License <https://spdx.org/licenses/MIT.html>
// See LICENSE file in the project root for full license information.
//
#include "Application.hpp"
#include "credentials.h"
#include <M5StickCPlus.h>
#undef min
#include "Bp35a1.hpp"
#include "Gauge.hpp"
#include "SmartWhm.hpp"
#include "Telemetry.hpp"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <esp_sntp.h>
#include <map>
#include <optional>
#include <queue>
#include <string>
#include <vector>

// time zone = Asia_Tokyo(UTC+9)
static constexpr char TZ_TIME_ZONE[] = "JST-9";

// BP35A1と会話できるポート番号
constexpr int CommPortRx{26};
constexpr int CommPortTx{0};

// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
// グローバル変数

// BP35A1 初期化が完了するまでnull
Bp35a1 *bp35a1(nullptr);

// 瞬時電力量
static Gauge<SmartWhm::InstantWatt> instant_watt_gauge{
    2, 4, YELLOW, std::make_pair(10, 10),
    [](std::optional<SmartWhm::InstantWatt> iw) -> std::string {
      auto result = std::string{"---- W"};
      if (iw.has_value()) {
        char buff[100]{'\0'};
        std::sprintf(buff, "%4d W", iw.value().watt);
        result = std::string(buff);
      }
      return result;
    }};

// 瞬時電流
static Gauge<SmartWhm::InstantAmpere> instant_ampere_gauge{
    1, 4, WHITE, std::make_pair(10, 10 + 48),
    [](std::optional<SmartWhm::InstantAmpere> ia) -> std::string {
      //
      auto to_string = [](int32_t deci_ampere) -> std::string {
        auto i = deci_ampere / 10; // 整数部
        auto f = deci_ampere % 10; // 小数部
        char buff[50]{'\0'};
        std::sprintf(buff, "%2d.%01d", i, f);
        return std::string(buff);
      };
      //
      auto result = std::string{"R:--.- A, T:--.- A"};
      if (ia.has_value()) {
        auto r = to_string(ia.value().r_deciA);
        auto t = to_string(ia.value().t_deciA);
        result = std::string{};
        result += "R:" + r + " A, ";
        result += "T:" + t + " A";
      }
      return result;
    }};

// 積算電力量
static Gauge<SmartWhm::CumulativeWattHour> cumulative_watt_hour_gauge{
    1, 4, WHITE, std::make_pair(10, 10 + 48 + 24),
    [](std::optional<SmartWhm::CumulativeWattHour> watt_hour) -> std::string {
      std::string result = std::string{"--:-- ----------    "};
      if (watt_hour) {
        auto wh = watt_hour.value();
        //
        result = std::string{};
        // 時間
        {
          char buff[100]{'\0'};
          std::sprintf(buff, "%02d:%02d", wh.hour(), wh.minutes());
          result += std::string(buff);
        }
        // 電力量
        std::optional<std::string> opt = wh.to_string_kwh();
        if (opt.has_value()) {
          result += " " + opt.value() + " kwh";
        } else {
          // 単位がないならそのまま出す
          result += " " + std::to_string(wh.raw_cumlative_watt_hour());
        }
      }
      return result;
    }};

// グローバル変数
// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

//
static bool connectToWiFi(std::size_t retry_count = 100) {
  if (WiFi.status() == WL_CONNECTED) {
    ESP_LOGD(MAIN, "WIFI connected, pass");
    return true;
  }
  ESP_LOGI(MAIN, "Connecting to WIFI SSID %s", WIFI_SSID.data());

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID.data(), WIFI_PASSWORD.data());
  for (std::size_t retry = 0; retry < retry_count; ++retry) {
    if (WiFi.status() == WL_CONNECTED) {
      break;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    ESP_LOGI(MAIN, "WiFi connected, IP address: %s",
             WiFi.localIP().toString().c_str());
    return true;
  } else {
    return false;
  }
}
//
static bool initializeTime(std::size_t retry_count = 100) {
  ESP_LOGI(MAIN, "Setting time using SNTP");

  configTzTime(TZ_TIME_ZONE, "ntp.nict.jp", "time.google.com",
               "ntp.jst.mfeed.ad.jp");
  //
  for (std::size_t retry = 0; retry < retry_count; ++retry) {
    delay(500);
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      char buf[50];
      time_t now = time(nullptr);
      ESP_LOGI(MAIN, "local time: \"%s\"", asctime_r(localtime(&now), buf));
      ESP_LOGI(MAIN, "Time initialized!");
      return true;
    }
  }
  //
  ESP_LOGE(MAIN, "SNTP sync failed");
  return false;
}

//
static bool establishConnection() {
  connectToWiFi();
  bool ok = true;
  ok = ok ? initializeTime() : ok;
  ok = ok ? connectToAwsIot() : ok;
  return ok;
}

//
static bool checkWiFi(std::size_t retry_count = 100) {
  for (std::size_t retry = 0; retry < retry_count; ++retry) {
    if (WiFi.isConnected()) {
      break;
    } else {
      ESP_LOGI(MAIN, "WiFi reconnect");
      WiFi.reconnect();
      establishConnection();
    }
  }
  return WiFi.isConnected();
}

//
//
//
static void check_MQTT_connection_task(void *) {
  while (1) {
    delay(1000);
    // WiFi接続検査
    if (checkWiFi()) {
      // MQTT接続検査
      checkTelemetry();
    } else {
      // WiFiの接続に失敗しているのでシステムリセットして復帰する
      esp_restart();
    }
  }
}

//
// bootメッセージ表示用
//
void display_boot_message(const char *s) { M5.Lcd.print(s); }

//
// Arduinoのsetup()関数
//
void setup() {
  M5.begin(true, true, true);
  M5.Lcd.setRotation(3);
  M5.Lcd.setTextSize(2);
  //
  Serial2.begin(115200, SERIAL_8N1, CommPortRx, CommPortTx);
  //
  display_boot_message("connect to IoT Hub\n");
  if (!establishConnection()) {
    display_boot_message("can't connect to IotHub, bye\n");
    ESP_LOGD(MAIN, "can't connect to IotHub");
    delay(10000);
    esp_restart();
  }
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  //
  static Bp35a1 bp35a1_instance(Serial2, std::make_pair(BID, BPASSWORD));
  if (!bp35a1_instance.boot(display_boot_message)) {
    display_boot_message("boot error, bye");
    ESP_LOGD(MAIN, "boot error");
    delay(10000);
    esp_restart();
  }
  // 初期化が完了したのでグローバルにセットする
  bp35a1 = &bp35a1_instance;
  //
  ESP_LOGD(MAIN, "setup success");

  //
  // ディスプレイ表示
  //
  M5.Lcd.fillScreen(BLACK);
  instant_watt_gauge.update(true);
  instant_ampere_gauge.update(true);
  cumulative_watt_hour_gauge.update(true);

  //
  // FreeRTOSタスク起動
  //
  xTaskCreatePinnedToCore(check_MQTT_connection_task, "checkMQTTtask", 8192,
                          nullptr, 2, nullptr, ARDUINO_RUNNING_CORE);
}

//
// BP35A1から受信したイベントを処理する
//
static void process_event(const Bp35a1::Response &r) {
  switch (std::strtol(r.keyval.at("NUM").c_str(), nullptr, 16)) {
  case 0x21: // EVENT 21 :
             // UDP送信処理が完了した
    ESP_LOGD(MAIN, "UDP transmission successful.");
    break;
  case 0x24: // EVENT 24 :
             // PANAによる接続過程でエラーが発生した(接続が完了しなかった)
    ESP_LOGD(MAIN, "reconnect");
    // 再接続を試みる
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(0, 0);
    display_boot_message("reconnect");
    if (!bp35a1->connect(display_boot_message)) {
      display_boot_message("reconnect error, try to reboot");
      ESP_LOGD(MAIN, "reconnect error, try to reboot");
      delay(5000);
      esp_restart();
    }
    M5.Lcd.fillScreen(BLACK);
    instant_watt_gauge.set(std::nullopt).update();
    instant_ampere_gauge.set(std::nullopt).update();
    cumulative_watt_hour_gauge.set(std::nullopt).update();
    break;
  case 0x29: // ライフタイムが経過して期限切れになった
    ESP_LOGD(MAIN, "session timeout occurred");
    break;
  default:
    break;
  }
}

//
// ノードプロファイルクラスのEchonetLiteフレームを処理する
//
static void process_node_profile_class_frame(const EchonetLiteFrame &frame) {
  for (const auto &v : splitToEchonetLiteData(frame.edata)) {
    auto prop = reinterpret_cast<const EchonetLiteProp *>(v.data());
    switch (prop->epc) {
    case 0xD5:              // インスタンスリスト通知
      if (prop->pdc >= 4) { // 4バイト以上
        ESP_LOGD(MAIN, "instances list");
        uint8_t total_number_of_instances = prop->edt[0];
        const EchonetLiteObjectCode *p =
            reinterpret_cast<const EchonetLiteObjectCode *>(&prop->edt[1]);
        //
        ESP_LOGD(MAIN, "total number of instances: %d",
                 total_number_of_instances);
        std::string str;
        for (uint8_t i = 0; i < total_number_of_instances; ++i) {
          char buffer[10]{'\0'};
          std::sprintf(buffer, "%02X%02X%02X", p[i].class_group,
                       p[i].class_code, p[i].instance_code);
          str += std::string(buffer) + ",";
        }
        str.pop_back(); // 最後の,を削る
        ESP_LOGD(MAIN, "list of object code(EOJ): %s", str.c_str());
      }
      //
      // 通知されているのは自分自身だろうから
      // なにもしませんよ
      //
      break;
    default:
      break;
    }
  }
}

//
// BP35A1から受信したERXUDPイベントを処理する
//
static void process_erxudp(const Bp35a1::Response &r,
                           SmartWhm &smart_watt_hour_meter,
                           std::queue<std::string> &to_sending_message_fifo) {
  //
  // key-valueストアに入れるときにテキスト形式に変換してあるので元のバイナリに戻す
  //
  // ペイロード(テキスト形式)
  std::string_view textformat = r.keyval.at("DATA");
  // ペイロード(バイナリ形式)
  std::vector<uint8_t> binaryformat =
      Bp35a1::Response::text_to_binary(textformat);
  // EchonetLiteFrameに変換
  EchonetLiteFrame *frame =
      reinterpret_cast<EchonetLiteFrame *>(binaryformat.data());
  // フレームヘッダの確認
  if (frame->ehd1 == EchonetLiteEHD1 && frame->ehd2 == EchonetLiteEHD2) {
    // EchonetLiteフレームだった
    ESP_LOGD(MAIN, "%s", SmartWhm::show(*frame).c_str());
    //
    auto const seoj = std::array<uint8_t, 3>{
        frame->edata.seoj[0], frame->edata.seoj[1], frame->edata.seoj[2]};
    if (seoj == NodeProfileClass::EchonetLiteEOJ()) {
      // ノードプロファイルクラス
      process_node_profile_class_frame(*frame);
    } else if (seoj == SmartWhm::EchonetLiteEOJ()) {
      // 低圧スマート電力量計クラス
      smart_watt_hour_meter.process_echonet_lite_frame(r.created_at, *frame,
                                                       to_sending_message_fifo);
    } else {
      ESP_LOGD(MAIN, "Unknown SEOJ: [0x%x, 0x%x, 0x%x]", seoj[0], seoj[1],
               seoj[2]);
    }
  } else {
    ESP_LOGD(MAIN, "unknown frame header. EHD1: 0x%x, EHD2: 0x%x]", frame->ehd1,
             frame->ehd2);
    return;
  }
}

//
// プログレスバーを表示する
//
static void render_progress_bar(uint32_t permille) {
  int32_t bar_width = M5.Lcd.width() * permille / 1000;
  int32_t y = M5.Lcd.height() - 2;
  M5.Lcd.fillRect(bar_width, y, M5.Lcd.width(), M5.Lcd.height(), BLACK);
  M5.Lcd.fillRect(0, y, bar_width, M5.Lcd.height(), YELLOW);
}

// スマートメーターに最初の要求を出す
static bool send_first_request() {
  std::vector<SmartWhm::EchonetLiteEPC> epcs{};
  // 係数
  // 積算電力量単位
  // 積算電力量有効桁数
  epcs = {
      SmartWhm::EchonetLiteEPC::Coefficient,
      SmartWhm::EchonetLiteEPC::Unit_for_cumulative_amounts,
      SmartWhm::EchonetLiteEPC::Number_of_effective_digits,
  };
  ESP_LOGD(MAIN, "%s",
           "request coefficient / unit for whm / request number of "
           "effective digits");
  // スマートメーターに要求を出す
  return bp35a1->send_request(epcs);
}

// スマートメーターに定期的な要求を出す
template <class Clock, class Duration>
static bool
send_periodical_request(std::chrono::time_point<Clock, Duration> current_time,
                        const SmartWhm &whm) {
  std::vector<SmartWhm::EchonetLiteEPC> epcs{};
  // 瞬時電力要求
  // 瞬時電流要求
  epcs = {SmartWhm::EchonetLiteEPC::Measured_instantaneous_power,
          SmartWhm::EchonetLiteEPC::Measured_instantaneous_currents};
  ESP_LOGD(MAIN, "%s", "request inst-epower and inst-current");
  // 定時積算電力量計測値(正方向計測値)
  //
  auto get_time_at = [&whm]() -> std::time_t {
    std::time_t tm{0};
    auto &opt = whm.cumlative_watt_hour;
    if (opt.has_value()) {
      tm = opt.value().get_time_t().value_or(0);
    }
    return tm;
  };
  using namespace std::chrono;
  auto measured_at = system_clock::from_time_t(get_time_at());
  auto elapsed = duration_cast<minutes>(current_time - measured_at);
  if (elapsed >= minutes{36}) {
    // 表示中の定時積算電力量計測値が36分より古い場合は定時積算電力量要求を出す
    epcs.push_back(
        SmartWhm::EchonetLiteEPC::
            Cumulative_amounts_of_electric_energy_measured_at_fixed_time);
    ESP_LOGD(MAIN, "%s", "request amounts of electric power");
  }
  // 積算履歴収集日
  if (!whm.day_for_which_the_historcal.has_value()) {
    epcs.push_back(
        SmartWhm::EchonetLiteEPC::Day_for_which_the_historcal_data_1);
    ESP_LOGD(MAIN, "%s", " day for historical data 1");
  }
  // スマートメーターに要求を出す
  return bp35a1->send_request(epcs);
}

//
// Arduinoのloop()関数
//
void loop() {
  // スマートメーター
  static SmartWhm smart_watt_hour_meter{};
  // メッセージ受信バッファ
  static std::queue<Bp35a1::Response> received_message_fifo{};
  // IoT Hub送信用バッファ
  static std::queue<std::string> to_sending_message_fifo{};
  using namespace std::chrono;
  // IoT Coreにメッセージを送信した時間
  static time_point time_of_sent_message_to_iot_core{system_clock::now()};
  // スマートメーターにメッセージを送信した時間
  static time_point time_of_sent_message_to_smart_whm{system_clock::now()};

  // (あれば)２５個連続でメッセージを受信する
  for (std::size_t count = 0; count < 25; ++count) {
    std::optional<Bp35a1::Response> r = bp35a1->watch_response();
    if (r.has_value()) {
      auto val = r.value();
      received_message_fifo.push(std::move(val));
    }
    delay(10);
  }

  // システムの時間は日本時間であるはず
  static_assert(TZ_TIME_ZONE, "JST-9");
  // 現在時刻(日本時間)
  time_point current_time = system_clock::now();
  auto current_epoch = current_time.time_since_epoch();
  auto current_millis = duration_cast<milliseconds>(current_epoch);
  auto current_seconds = duration_cast<seconds>(current_epoch);

  // IoT Coreへ送信(1秒以上の間隔をあけて)
  if (auto elapsed = current_time - time_of_sent_message_to_iot_core;
      elapsed >= seconds{1}) {
    if (!to_sending_message_fifo.empty()) {
      // 送信するべき測定値があればIoTHubへ送信する
      if (sendTelemetry(to_sending_message_fifo.front())) {
        // 処理したメッセージをFIFOから消す
        to_sending_message_fifo.pop();
      }
      // 送信時間を記録する
      time_of_sent_message_to_iot_core = current_time;
    }
  }

  // メッセージ受信処理
  if (!received_message_fifo.empty()) {
    Bp35a1::Response r = received_message_fifo.front();
    ESP_LOGD(MAIN, "%s", r.show().c_str());
    switch (r.tag) {
    case Bp35a1::Response::Tag::EVENT:
      // イベント受信処理
      process_event(r);
      break;
    case Bp35a1::Response::Tag::ERXUDP:
      // ERXUDPを処理する
      process_erxudp(r, smart_watt_hour_meter, to_sending_message_fifo);
      // 測定値をセットする
      instant_watt_gauge.set(smart_watt_hour_meter.instant_watt);
      instant_ampere_gauge.set(smart_watt_hour_meter.instant_ampere);
      cumulative_watt_hour_gauge.set(smart_watt_hour_meter.cumlative_watt_hour);
      break;
    default:
      break;
    }
    // 処理したメッセージをFIFOから消す
    received_message_fifo.pop();
  }

  // 測定値を更新する
  instant_watt_gauge.update();
  instant_ampere_gauge.update();
  cumulative_watt_hour_gauge.update();
  //
  M5.update();
  loopTelemetry();

  // プログレスバーを表示する
  const duration one_minutes = milliseconds{60000};
  render_progress_bar(1000 * (one_minutes - (current_millis % one_minutes)) /
                      one_minutes);

  // スマートメーターに要求を出す(1秒以上の間隔をあけて)
  if (auto elapsed = current_time - time_of_sent_message_to_smart_whm;
      elapsed >= seconds{1}) {
    // 積算電力量単位がない場合に最初の要求を出す
    if (!smart_watt_hour_meter.whm_unit.has_value()) {
      if (!send_first_request()) {
        ESP_LOGD(MAIN, "request NG");
      }
    } else if (current_seconds % 60 == seconds{0}) {
      // 毎分0秒にスマートメーターに要求を出す
      if (!send_periodical_request(current_time, smart_watt_hour_meter)) {
        ESP_LOGD(MAIN, "request NG");
      }
    }
    // 送信時間を記録する
    time_of_sent_message_to_smart_whm = current_time;
  }
}
