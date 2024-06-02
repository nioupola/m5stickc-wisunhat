// Copyright (c) 2022 Akihiro Yamamoto.
// Licensed under the MIT License <https://spdx.org/licenses/MIT.html>
// See LICENSE file in the project root for full license information.
//
#include "Application.hpp"
#include "Bp35a1.hpp"
#include "EchonetLite.hpp"
#include "Gui.hpp"
#include "Telemetry.hpp"
#include "credentials.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <esp_sntp.h>
#include <future>
#include <iomanip>
#include <lvgl.h>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <M5Unified.h>

using namespace std::literals::string_view_literals;
using namespace std::chrono_literals;

// time zone = Asia_Tokyo(UTC+9)
constexpr char TZ_TIME_ZONE[] = "JST-9";

// BP35A1と会話できるポート番号
constexpr int CommPortRx{26};
constexpr int CommPortTx{0};

//
//
//
struct SmartWhm {
  // BP35A1と会話できるポート
  Stream &commport;
  // スマート電力量計のＢルート識別子
  Bp35a1::SmartMeterIdentifier identifier;
  // Echonet Lite PANA session
  bool isPanaSessionEstablished{false};
  //
  SmartWhm(Stream &comm, Bp35a1::SmartMeterIdentifier ident)
      : commport{comm}, identifier{ident} {}
};

// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
// グローバル変数はじまり
//
// 接続相手のスマートメーター
static std::unique_ptr<SmartWhm> smart_watt_hour_meter;
// MQTT
static Telemetry
    telemetry(Telemetry::DeviceId{AWS_IOT_DEVICE_ID},
              // データーベースのパーティションキーであるセンサーＩＤ
              Telemetry::SensorId{"smartmeter"},
              Telemetry::AwsIotEndpoint{AWS_IOT_ENDPOINT},
              Telemetry::AwsIotRootCa{AWS_IOT_ROOT_CA},
              Telemetry::AwsIotCertificate{AWS_IOT_CERTIFICATE},
              Telemetry::AwsIotPrivateKey{AWS_IOT_PRIVATE_KEY});
//
//
// グローバル変数おわり
// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

static void gotWiFiEvent(WiFiEvent_t event) {
  switch (event) {
  case SYSTEM_EVENT_AP_START:
    M5_LOGI("AP Started");
    break;
  case SYSTEM_EVENT_AP_STOP:
    M5_LOGI("AP Stopped");
    break;
  case SYSTEM_EVENT_STA_START:
    M5_LOGI("STA Started");
    break;
  case SYSTEM_EVENT_STA_CONNECTED:
    M5_LOGI("STA Connected");
    break;
  case SYSTEM_EVENT_AP_STA_GOT_IP6: {
    auto localipv6 = WiFi.localIPv6();
    M5_LOGI("STA IPv6: %s", localipv6.toString());
  } break;
  case SYSTEM_EVENT_STA_GOT_IP: {
    auto localip = WiFi.localIP();
    M5_LOGI("STA IPv4: %s", localip.toString());
  } break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    M5_LOGI("STA Disconnected");
    //    WiFi.begin();
    break;
  case SYSTEM_EVENT_STA_STOP:
    M5_LOGI("STA Stopped");
    break;
  default:
    break;
  }
}

//
// WiFi APとの接続待ち
//
static bool waitingForWiFiConnection(Widget::Dialogue &dialogue,
                                     uint16_t max_retries = 100U) {
  auto status = WiFi.status();
  //
  for (auto nth_tries = 1; status != WL_CONNECTED && nth_tries <= max_retries;
       ++nth_tries) {
    // wait for connection.
    dialogue.setMessage("Waiting : "s + std::to_string(nth_tries) + " / "s +
                        std::to_string(max_retries));
    delay(1000);
    //
    status = WiFi.status();
  }
  if (status == WL_CONNECTED) {
    dialogue.setMessage("WiFi connected");
    return true;
  } else {
    dialogue.error("ERROR");
    return false;
  }
}

//
// NTPと同期する
//
static bool initializeTime(Widget::Dialogue &dialogue,
                           uint16_t max_retries = 300U) {
  sntp_sync_status_t status = sntp_get_sync_status();
  //
  dialogue.setMessage("Setting time using SNTP");
  configTzTime(TZ_TIME_ZONE, "ntp.jst.mfeed.ad.jp", "time.cloudflare.com",
               "ntp.nict.jp");
  //
  for (auto nth_tries = 1;
       status != SNTP_SYNC_STATUS_COMPLETED && nth_tries <= max_retries;
       ++nth_tries) {
    // wait for time sync.
    dialogue.setMessage("Waiting : "s + std::to_string(nth_tries) + " / "s +
                        std::to_string(max_retries));
    delay(1000);
    //
    status = sntp_get_sync_status();
  }
  if (status == SNTP_SYNC_STATUS_COMPLETED) {
    dialogue.setMessage("Time synced.");
    return true;
  } else {
    dialogue.error("ERROR");
    return false;
  }
}

// Arduinoのsetup()関数
void setup() {
  if (new Application(M5.Display)) {
    Application::getInstance()->startup();
  } else {
    M5_LOGE("out of memory");
    M5.Display.print("out of memory.");
    std::this_thread::sleep_for(10min);
    esp_system_abort("out of memory.");
  }
}

//
// BP35A1から受信したイベントを処理する
//
static void process_event(const Bp35a1::ResEvent &ev) {
  switch (ev.num.u8) {
  case 0x01: // EVENT 1 :
             // NSを受信した
    M5_LOGI("Received NS");
    break;
  case 0x02: // EVENT 2 :
             // NAを受信した
    M5_LOGI("Received NA");
    break;
  case 0x05: // EVENT 5 :
             // Echo Requestを受信した
    M5_LOGI("Received Echo Request");
    break;
  case 0x1F: // EVENT 1F :
             // EDスキャンが完了した
    M5_LOGI("Complete ED Scan.");
    break;
  case 0x20: // EVENT 20 :
             // BeaconRequestを受信した
    M5_LOGI("Received BeaconRequest");
    break;
  case 0x21: // EVENT 21 :
             // UDP送信処理が完了した
    M5_LOGD("UDP transmission successful.");
    break;
  case 0x24: // EVENT 24 :
             // PANAによる接続過程でエラーが発生した(接続が完了しなかった)
    M5_LOGD("PANA reconnect");
    if (smart_watt_hour_meter) {
      smart_watt_hour_meter->isPanaSessionEstablished = false;
    } else {
      M5_LOGD("No connection to smart meter.");
    }
    break;
  case 0x25: // EVENT 25 :
             // PANAによる接続が完了した
    M5_LOGD("PANA session connected");
    if (smart_watt_hour_meter) {
      smart_watt_hour_meter->isPanaSessionEstablished = true;
    } else {
      M5_LOGD("No connection to smart meter.");
    }
    break;
  case 0x26: // EVENT 26 :
             // 接続相手からセッション終了要求を受信した
    M5_LOGD("session terminate request");
    if (smart_watt_hour_meter) {
      smart_watt_hour_meter->isPanaSessionEstablished = false;
    } else {
      M5_LOGD("No connection to smart meter.");
    }
    break;
  case 0x27: // EVENT 27 :
             // PANAセッションの終了に成功した
    M5_LOGD("PANA session terminate");
    if (smart_watt_hour_meter) {
      smart_watt_hour_meter->isPanaSessionEstablished = false;
    } else {
      M5_LOGD("No connection to smart meter.");
    }
    break;
  case 0x28: // EVENT 28 :
             // PANAセッションの終了要求に対する応答がなくタイムアウトした(セッションは終了)
    M5_LOGD("PANA session terminate. reason: timeout");
    if (smart_watt_hour_meter) {
      smart_watt_hour_meter->isPanaSessionEstablished = false;
    } else {
      M5_LOGD("No connection to smart meter.");
    }
    break;
  case 0x29: // PANAセッションのライフタイムが経過して期限切れになった
    M5_LOGI("PANA session expired");
    if (smart_watt_hour_meter) {
      smart_watt_hour_meter->isPanaSessionEstablished = false;
    } else {
      M5_LOGD("No connection to smart meter.");
    }
    break;
  case 0x32: // ARIB108の送信緩和時間の制限が発動した
    M5_LOGI("");
    break;
  case 0x33: // ARIB108の送信緩和時間の制限が解除された
    M5_LOGI("");
    break;
  default:
    break;
  }
}

//
// ノードプロファイルクラスのEchonetLiteフレームを処理する
//
static void process_node_profile_class_frame(const EchonetLiteFrame &frame) {
  for (const EchonetLiteProp &prop : frame.edata.props) {
    switch (prop.epc) {
    case 0xD5:                    // インスタンスリスト通知
      if (prop.edt.size() >= 4) { // 4バイト以上
        std::ostringstream oss;
        auto it = prop.edt.cbegin();
        uint8_t total_number_of_instances = *it++;
        while (std::distance(it, prop.edt.cend()) >= 3) {
          auto obj = EchonetLiteObjectCode({*it++, *it++, *it++});
          oss << obj << ",";
        }
        M5_LOGD("list of instances (EOJ): %s", oss.str().c_str());
      }
      //
      // 通知されているのは自分自身だろうから
      // なにもしませんよ
      //
      break;
    default:
      M5_LOGD("unknown EPC: %02X", prop.epc);
      break;
    }
  }
}

//
// BP35A1から受信したERXUDPイベントを処理する
//
static void process_erxudp(std::chrono::system_clock::time_point at,
                           const Bp35a1::ResErxudp &ev) {
  // EchonetLiteFrameに変換
  if (auto opt = deserializeToEchonetLiteFrame(ev.data)) {
    const EchonetLiteFrame &frame = opt.value();
    //  EchonetLiteフレームだった
    M5_LOGD("%s", to_string(frame).c_str());
    //
    if (frame.edata.seoj.s == NodeProfileClass::EchonetLiteEOJ) {
      // ノードプロファイルクラス
      process_node_profile_class_frame(frame);
    } else if (frame.edata.seoj.s == SmartElectricEnergyMeter::EchonetLiteEOJ) {
      // 低圧スマート電力量計クラス
      namespace M = SmartElectricEnergyMeter;
      for (auto rx : M::process_echonet_lite_frame(frame)) {
        if (auto *p = std::get_if<M::Coefficient>(&rx)) {
          Application::getElectricPowerData().whm_coefficient = *p;
        } else if (std::get_if<M::EffectiveDigits>(&rx)) {
          // no operation
        } else if (auto *p = std::get_if<M::Unit>(&rx)) {
          Application::getElectricPowerData().whm_unit = *p;
        } else if (auto *p = std::get_if<M::InstantAmpere>(&rx)) {
          Application::getElectricPowerData().instant_ampere =
              std::make_pair(at, *p);
          // 送信バッファへ追加する
          telemetry.enqueue(std::make_pair(at, *p));
        } else if (auto *p = std::get_if<M::InstantWatt>(&rx)) {
          Application::getElectricPowerData().instant_watt =
              std::make_pair(at, *p);
          // 送信バッファへ追加する
          telemetry.enqueue(std::make_pair(at, *p));
        } else if (auto *p = std::get_if<M::CumulativeWattHour>(&rx)) {
          if (auto unit = Application::getElectricPowerData().whm_unit) {
            auto coeff =
                Application::getElectricPowerData().whm_coefficient.value_or(
                    M::Coefficient{});
            Application::getElectricPowerData().cumlative_watt_hour =
                std::make_tuple(*p, coeff, *unit);
            // 送信バッファへ追加する
            telemetry.enqueue(std::make_tuple(*p, coeff, *unit));
          }
        }
      }
    }
  }
}

//
// スマートメーターに最初の要求を出す
//
static void
send_first_request(std::chrono::system_clock::time_point current_time) {
  using E = SmartElectricEnergyMeter::EchonetLiteEPC;
  std::vector<E> epcs = {
      E::Operation_status,            // 動作状態
      E::Installation_location,       // 設置場所
      E::Fault_status,                // 異常発生状態
      E::Manufacturer_code,           // メーカーコード
      E::Coefficient,                 // 係数
      E::Unit_for_cumulative_amounts, // 積算電力量単位
      E::Number_of_effective_digits,  // 積算電力量有効桁数
  };
  M5_LOGD("request status / location / fault / manufacturer / coefficient / "
          "unit for whm / request number of "
          "effective digits");
  // スマートメーターに要求を出す
  const auto tid = EchonetLiteTransactionId({12, 34});

  if (smart_watt_hour_meter) {
    Bp35a1::send_request(smart_watt_hour_meter->commport,
                         smart_watt_hour_meter->identifier, tid, epcs);
  } else {
    M5_LOGD("No connection to smart meter.");
  }
}

//
// スマートメーターに定期的な要求を出す
//
static void
send_periodical_request(std::chrono::system_clock::time_point current_time) {
  using E = SmartElectricEnergyMeter::EchonetLiteEPC;
  std::vector<E> epcs = {
      E::Measured_instantaneous_power,    // 瞬時電力要求
      E::Measured_instantaneous_currents, // 瞬時電流要求
  };
  M5_LOGD("request inst-epower and inst-current");
  //
  std::time_t displayed_jst = []() -> std::time_t {
    if (Application::getElectricPowerData().cumlative_watt_hour.has_value()) {
      auto [cwh, unuse, unused] =
          Application::getElectricPowerData().cumlative_watt_hour.value();
      return cwh.get_time_t().value_or(0);
    } else {
      return 0;
    }
  }();
  if constexpr (false) {
    char buf[50]{'\0'};
    auto tm = std::chrono::system_clock::to_time_t(current_time);
    M5_LOGI("current time:%s", ctime_r(&tm, buf));
    M5_LOGI("displayed time:%s", ctime_r(&displayed_jst, buf));
  }
  auto measured_at = std::chrono::system_clock::from_time_t(displayed_jst);
  if (auto elapsed = current_time - measured_at;
      elapsed >= std::chrono::minutes{36}) {
    // 表示中の定時積算電力量計測値が36分より古い場合は
    // 定時積算電力量計測値(30分値)をつかみそこねたと判断して
    // 定時積算電力量要求を出す
    epcs.push_back(
        E::Cumulative_amounts_of_electric_energy_measured_at_fixed_time);
    // 定時積算電力量計測値(正方向計測値)
    M5_LOGD("request amounts of electric power");
  }
#if 0
// 積算履歴収集日
  if (!whm.day_for_which_the_historcal.has_value()) {
    epcs.push_back(E::Day_for_which_the_historcal_data_1);
    ESP_LOGD(MAIN, "request day for historical data 1");
  }
#endif
  // スマートメーターに要求を出す
  if (smart_watt_hour_meter) {
    const auto tid = EchonetLiteTransactionId({12, 34});
    Bp35a1::send_request(smart_watt_hour_meter->commport,
                         smart_watt_hour_meter->identifier, tid, epcs);
  } else {
    M5_LOGD("No connection to smart meter.");
  }
}

//
// スマートメーターに要求を送る
//
static void send_request_to_smart_meter() {
  using namespace std::chrono;
  static system_clock::time_point send_time_at;
  auto nowtp = system_clock::now();
  //
  constexpr auto INTERVAL = 15;
  if (auto elapsed = nowtp - send_time_at; elapsed < seconds{INTERVAL}) {
    return;
  }
  auto sec = duration_cast<seconds>(nowtp.time_since_epoch()).count() % 60;
  //
  // 積算電力量単位が初期値の場合にスマートメーターに最初の要求を出す
  //
  if (!Application::getElectricPowerData().whm_unit.has_value()) {
    send_first_request(nowtp);
    // 送信時間を記録する
    send_time_at = nowtp;
  }
  //
  // １分毎にスマートメーターに定期要求を出す
  //
  else if (sec < INTERVAL) {
    send_periodical_request(nowtp);
    // 送信時間を記録する
    send_time_at = nowtp;
  }
}

//
// 高速度loop()関数
//
inline void high_speed_loop(std::chrono::system_clock::time_point nowtp) {
  M5.update();
  if (M5.BtnA.wasPressed()) {
    Application::getGui().moveNext();
  }
  //
  // メッセージ受信バッファ
  //
  static std::queue<
      std::pair<std::chrono::system_clock::time_point, Bp35a1::Response>>
      received_message_fifo{};

  //
  // (あれば)連続でスマートメーターからのメッセージを受信する
  //
  if (smart_watt_hour_meter) {
    for (auto count = 0; count < 25; ++count) {
      if (auto resp =
              Bp35a1::receive_response(smart_watt_hour_meter->commport)) {
        received_message_fifo.push({nowtp, resp.value()});
      }
      delay(10);
    }
    //
    // スマートメーターからのメッセージ受信処理
    //
    if (!received_message_fifo.empty()) {
      auto [time_at, resp] = received_message_fifo.front();
      std::visit([](const auto &x) { M5_LOGD("%s", to_string(x).c_str()); },
                 resp);
      if (auto *pevent = std::get_if<Bp35a1::ResEvent>(&resp)) {
        // イベント受信処理
        process_event(*pevent);
      } else if (auto *perxudp = std::get_if<Bp35a1::ResErxudp>(&resp)) {
        // ERXUDPを処理する
        process_erxudp(time_at, *perxudp);
      }
      // 処理したメッセージをFIFOから消す
      received_message_fifo.pop();
    }
  }
}

//
// 低速度loop()関数
//
inline void low_speed_loop(std::chrono::system_clock::time_point nowtp) {
  if (M5.Power.getBatteryLevel() < 100 &&
      M5.Power.isCharging() == m5::Power_Class::is_discharging) {
    // バッテリー駆動時は明るさを下げる
    if (M5.Display.getBrightness() != 75) {
      M5.Display.setBrightness(75);
    }
  } else {
    // 通常の明るさ
    if (M5.Display.getBrightness() != 150) {
      M5.Display.setBrightness(150);
    }
  }
  //
  using namespace std::chrono;
  //
  auto display_message = [](const std::string &str, void *user_data) -> void {
    M5_LOGD("%s", str.c_str());
    if (user_data) {
      static_cast<Widget::Dialogue *>(user_data)->setMessage(str);
    }
  };
  //
  if (!smart_watt_hour_meter) {
    // 接続対象のスマートメーターの情報が無い場合は探す。
    M5_LOGD("Find a smart energy meter");
    Widget::Dialogue dialogue{"Find a meter."};
    display_message("seeking...", &dialogue);
    auto identifier = Bp35a1::startup_and_find_meter(
        Serial2, {BID, BPASSWORD}, display_message, &dialogue);
    if (identifier) {
      // 見つかったスマートメーターをグローバル変数に設定する
      smart_watt_hour_meter =
          std::make_unique<SmartWhm>(SmartWhm(Serial2, identifier.value()));
    } else {
      // スマートメーターが見つからなかった
      M5_LOGE("ERROR: meter not found.");
      dialogue.error("ERROR: meter not found.");
      delay(1000);
    }
  } else if (!smart_watt_hour_meter->isPanaSessionEstablished) {
    // スマートメーターとのセッションを開始する。
    M5_LOGD("Connect to a meter.");
    Widget::Dialogue dialogue{"Connect to a meter."};
    display_message("Send request to a meter.", &dialogue);
    // スマートメーターに接続要求を送る
    if (auto ok = connect(smart_watt_hour_meter->commport,
                          smart_watt_hour_meter->identifier, display_message,
                          &dialogue);
        ok) {
      // 接続成功
      smart_watt_hour_meter->isPanaSessionEstablished = true;
    } else {
      // 接続失敗
      smart_watt_hour_meter->isPanaSessionEstablished = false;
      M5_LOGE("smart meter connection error.");
      dialogue.error("smart meter connection error.");
      delay(1000);
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    // WiFiが接続されていない場合は接続する。
    Widget::Dialogue dialogue{"Connect to WiFi."};
    if (auto ok = waitingForWiFiConnection(dialogue); !ok) {
      dialogue.error("ERROR: WiFi");
      delay(1000);
    }
  } else if (bool connected = telemetry.isConnected(); !connected) {
// AWS IoTと接続されていない場合は接続する。
#if 0
    Widget::Dialogue dialogue{"Connect to AWS IoT."};
    if (auto ok = telemetry.connectToAwsIot(std::chrono::seconds{60},
                                            display_message, &dialogue);
        !ok) {
      dialogue.error("ERROR");
      delay(1000);
    }
#endif
  }

  // MQTT送受信
  telemetry.loop_mqtt();

  // スマートメーターに要求を送る
  send_request_to_smart_meter();
}

//
// Arduinoのloop()関数
//
void loop() {
  using namespace std::chrono;
  static system_clock::time_point before{};
  auto nowtp = system_clock::now();
  high_speed_loop(nowtp);
  //
  if (nowtp - before >= 1s) {
    low_speed_loop(nowtp);
    before = nowtp;
  }
}
