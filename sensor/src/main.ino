/*
 * AC power monitor for M5Atomic Socket with AtomS3
 *
 *  Copyright (C) 2024 Hiroshi Kuwagata <kgt9221@gmail.com>
 */
#include <M5AtomS3.h>
#include <AtomSocket.h>
#include <math.h>
#include <float.h>

#undef DISPLAY_TEST

//! レコーダと接続するシリアルのRX信号に割り当てるGPIOの番号
#define RXPIN         (2)

//! レコーダと接続するシリアルのTX信号に割り当てるGPIOの番号
#define TXPIN         (1)
                     
//! センサーデバイスと接続するシリアルのRX信号に割り当てるGPIOの番号
#define RXD           (5)

//! センサーデバイスのリレー制御用のGPIOの番号
#define RELAY         (7)

//! 画面表示モード指定子（電圧値表示モード）
#define MODE_VOLTAGE  (1)

//! 画面表示モード指定子（電流値表示モード）
#define MODE_CURRENT  (2)

//! 画面表示モード指定子（消費電力表示モード）
#define MODE_WATTAGE  (3)

//! 記録用M5Atomとの通信用シリアル
static HardwareSerial LoComm(1);

//! 測定値受信用の数信用シリアル
static HardwareSerial AtomSerial(2);

//! センサーデバイスへのアクセスインタフェース
static ATOMSOCKET ATOM;

//! 画面表示用スプライト(フレームバッファとして使用)
static M5Canvas canvas(&M5.Lcd);

//! タイムスタンプ計算用区間時刻
static unsigned long t0;

//! 表示モード
static int dispMode;

//! 表示を行うか否かをあらわすフラグ
static bool enableLcd;

/**
 * タイムスタンプ
 *
 * @remarks
 *  m5Atomは RTCが無いので、電源投入時刻起点としたタイムスタンプを管理する。ま
 *  た、millis()は32ビット値を返すためそのままでは50日間程度で桁溢れが発生する。
 *  このため、ループ先頭の時刻の差分を積算し64ビットのタイムタンプとして用いる。
 */
uint64_t ts;

//! データ出力用行バッファ
char buf[80];

//! 計測値格納用の構造体
typedef struct {
  //! 電圧値(V)
  float voltage;

  //! 電流値(A)
  float current;

  //! 消費電力(W)
  float wattage;
} measure_value_t;

//! 電力測定モジュールから読み出した値を格納する領域
typedef struct {
  //! 最後に読み出した値
  measure_value_t latest;

  //! 最小値
  measure_value_t min;

  //! 最大値
  measure_value_t max;
} value_set_t;

//! データを格納する領域
value_set_t data = {
  {NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}
};

/**
 * 液晶表示更新
 *
 * param [in] vol 電圧の値(V)
 * param [in] cur 電流の値(A)
 * param [in] wat 消費電力の値(W)
 */
void
display_update()
{
  char* label;
  char* fmt1;
  char* fmt2;
  float value;
  float min;
  float max;
  char* unit;
  char buf[30];

  /*
   * 表示モードに応じて出力内容を選択
   */
  switch (dispMode) {
  case MODE_VOLTAGE:
    label = (char*)"電圧";
    fmt1  = (char*)"%5.1f";
    fmt2  = (char*)"(%.1f〜%.1f)";
    value = data.latest.voltage;
    min   = data.min.voltage;
    max   = data.max.voltage;
    unit  = (char*)"V";
    break;

  case MODE_CURRENT:
    label = (char*)"電流";
    fmt1  = (char*)"%5.2f";
    fmt2  = (char*)"(%.2f〜%.2f)";
    value = data.latest.current;
    min   = data.min.current;
    max   = data.max.current;
    unit  = (char*)"A";
    break;

  case MODE_WATTAGE:
    label = (char*)"消費電力";
    fmt1  = (char*)"%5.1f";
    fmt2  = (char*)"(%.1f〜%.1f)";
    value = data.latest.wattage;
    min   = data.min.wattage;
    max   = data.max.wattage;
    unit  = (char*)"W";
    break;

  default:
    return;
  }

  /*
   * フレームバッファの更新
   */

  // クリア
  canvas.fillScreen(TFT_BLACK);

  // ラベルの描画
  canvas.setFont(&fonts::lgfxJapanGothic_20);
  canvas.setTextColor(TFT_YELLOW);
  canvas.setCursor(12, 28);
  canvas.print(label);

  // 計測値の描画
  if (!isnan(value)) {
    canvas.setFont(&fonts::lgfxJapanGothic_36);
    canvas.setTextColor(TFT_WHITE);
    canvas.setCursor(12, 48);
    canvas.printf(fmt1, value);

    canvas.setFont(&fonts::lgfxJapanGothic_24);
    canvas.setTextColor(TFT_WHITE);
    canvas.setCursor(104, 60);
    canvas.printf("%s", unit);
  }

  // 最小・最大値の描画
  if (!(isnan(min) || isnan(max))) {
    sprintf(buf, fmt2, min, max);

    canvas.setFont(&fonts::lgfxJapanGothic_16);
    canvas.setTextColor(TFT_LIGHTGRAY);
    canvas.setCursor((canvas.width() - canvas.textWidth(buf)) / 2 , 84);
    canvas.printf("%s", buf);
  }

  /*
   * 液晶画面への反映
   */
  M5.Lcd.startWrite();
  canvas.pushSprite(0, 0);
  M5.Lcd.endWrite();
}

/**
 * セットアップ関数
 */
void
setup()
{
  /*
   * M5Atomの初期化
   */
  M5.begin();

  /*
   * センサデバイスの初期化
   */
  ATOM.Init(AtomSerial, RELAY, RXD);
  ATOM.SetPowerOn();

  /*
   * ローカル通信用シリアルの初期化
   */
  LoComm.begin(115200, SERIAL_8N1, RXPIN, TXPIN);

  /*
   * ディスプレイの初期化
   */
  M5.Lcd.begin();
  M5.Lcd.fillScreen(TFT_BLACK);

  /*
   * フレームバッファの初期化
   */
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Lcd.width(), M5.Lcd.height());

  /*
   * 各変数の初期化
   */
  t0        = millis();
  ts        = 0;
  dispMode  = MODE_VOLTAGE;
  enableLcd = true;
}

void
load_measure_data()
{
#ifdef DISPLAY_TEST
  float vol = 103.77;
  float cur = 2.3;
  float wat = 60.2;
#else /* defined(DISPLAY_TEST) */
  float vol = ATOM.GetVol();
  float cur = ATOM.GetCurrent();
  float wat = ATOM.GetActivePower();
#endif /* defined(DISPLAY_TEST) */

  /*
   * 電圧値の最小・最大を更新
   */
  if (vol >= 0.0) {
    data.latest.voltage = vol;

    if (isnan(data.min.voltage) || vol < data.min.voltage) {
      data.min.voltage = vol;
    }

    if (isnan(data.max.voltage) || vol > data.max.voltage) {
      data.max.voltage = vol;
    }
  }


  /*
   * 電流値の最小・最大を更新
   */
  if (cur >= 0.0) {
    data.latest.current = cur;

    if (isnan(data.min.current) || cur < data.min.current) {
      data.min.current = cur;
    }

    if (isnan(data.max.current) || cur > data.max.current) {
      data.max.current = cur;
    }
  }

  /*
   * 消費電力の最小・最大を更新
   */
  if (wat >= 0.0) {
    data.latest.wattage = wat;

    if (isnan(data.min.wattage) || wat < data.min.wattage) {
      data.min.wattage = wat;
    }

    if (isnan(data.max.wattage) || wat > data.max.wattage) {
      data.max.wattage = wat;
    }
  }
}

/**
 * ルーパー関数
 */
void
loop()
{
  /*
   * ボタン状態の更新。
   */
  M5.update();

  if (M5.BtnA.wasSingleClicked()) {
    // 通常の押下の場合 (表示内容の切り替え)
 
    if (enableLcd) {
      switch (dispMode) {
      case MODE_VOLTAGE:
        dispMode = MODE_CURRENT;
        break;

      case MODE_CURRENT:
        dispMode = MODE_WATTAGE;
        break;

      case MODE_WATTAGE:
        dispMode = MODE_VOLTAGE;
        break;
      }
    }

  } else if (M5.BtnA.wasDoubleClicked()) {
    // ダブルクリックの場合 (最大最小値のクリア)
 
    data.min = {NAN, NAN, NAN};
    data.max = {NAN, NAN, NAN};

  } else if (M5.BtnA.wasHold()) {
    // 長押しの場合 (LCDの表示・消灯のトグル)

    if (enableLcd) {
      enableLcd = false;
      M5.Lcd.sleep();

    } else {
      enableLcd = true;
      M5.Lcd.wakeup();
    }
  }

  /*
   * センサーからデータが届くまで待つ
   */
#ifndef DISPLAY_TEST
  ATOM.SerialReadLoop();
#endif /* defined(DISPLAY_TEST) */

  /*
   * 読み出せるデータが存在すれば処理を実施
   */
#ifndef DISPLAY_TEST
  if (ATOM.SerialRead == 1) {
#endif /* defined(DISPLAY_TEST) */
    M5.Display.startWrite();

    // 区間先頭時刻の記録
    unsigned long t = millis() - t0;

    // データのロード
    load_measure_data();

    // 画面表示の更新
    display_update();

    // タイムスタンプの計算
    ts += t;

    // データの出力
    sprintf(buf,
            "%lu,%f,%f,%f",
            ts,
            data.latest.voltage,
            data.latest.current,
            data.latest.wattage);

    LoComm.println(buf);

    // 区間先頭時刻の更新
    t0 += t;

#ifndef DISPLAY_TEST
    ATOM.SerialRead = 0;
  }
#endif /* defined(DISPLAY_TEST) */
}
