/*
 * Recorder for AC power monitor
 *
 *  Copyright (C) 2024 Hiroshi Kuwagata <kgt9221@gmail.com>
 */

#include <M5Unified.h>
#include <FastLED.h>
#include <SdFat.h>
#include <time.h>

#include "writer.h"
#include "datetime_ctl.h"

//! RGBLED制御に割り当てられているGPIOの番号
#define LED_PIN         (27)

//! データ受信に使用するシリアルの受信信号に割り当てるGPIOの番号
#define RXPIN           (32)

//! データ受信に使用するシリアルの送信信号に割り当てるGPIOの番号
#define TXPIN           (26)

//! SPI制御のSCKに割り当てるGPIOの番号
#define  SCK            (23)

//! SPI制御のMISOに割り当てるGPIOの番号
#define  MISO           (33)

//! SPI制御のMOSIに割り当てるGPIOの番号
#define  MOSI           (19)

//! SPI制御のSSに割り当てるGPIOの番号
#define  SS             (-1)

//! 待機状態を示す状態コード
#define ST_IDLE         (0)

//! 記録開始のための行末待ち状態を示す状態コード
#define ST_READY        (1)

//! 記録状態を示す状態コード
#define ST_RECORD       (2)

//! 記録終了のための行末待ち状態を示す状態コード
#define ST_FIN          (3)

//! エラー発生状態を示す状態コード
#define ST_ERROR        (4)

//! SdFatコンフィギュレーションデータ
#define SPI_SPEED       SD_SCK_MHZ(10) 
#define SD_CONFIG       SdSpiConfig(0, SHARED_SPI, SPI_SPEED)

//! デフォルトのエラーコード
#define DEFAULT_ERROR   (__LINE__)

//! SDカードインタフェースオブジェクト
SdFat SD;

//! FastLEDで使用するフレームバッファ
CRGB led;

//! 状態管理変数
static int state;

//! 時刻情報が使用可能か否かを示すフラグ
static bool enableDatetime = false;

/*
 * 内部関数
 */

/**
 * IDLE状態への遷移
 *
 * @warning
 *  本関数はグローバル変数stateの書き換えを行う
 */
static void
transition_to_idle()
{
  if (enableDatetime) {
    led = CRGB::Blue;
  } else {
    led = CRGB::DarkCyan;
  }

  FastLED.show();

  state = ST_IDLE;
}

/**
 * READY状態への遷移
 *
 * @warning
 *  本関数はグローバル変数file及びstateの書き換えを行う
 */
static void
transition_to_ready()
{
  led = CRGB::Green;
  FastLED.show();

  state = ST_READY;
}

/**
 * RECORD状態への遷移
 *
 * @warning
 *  本関数はグローバル変数state,usedの書き換えを行う
 */
static void
transition_to_record()
{
  led = CRGB::DarkGreen;
  FastLED.show();

  state = ST_RECORD;
}

/**
 * FIN状態への遷移
 *
 * @warning
 *  本関数はグローバル変数stateの書き換えを行う
 */
static void
transition_to_fin()
{
  led = CRGB::Magenta;
  FastLED.show();

  state = ST_FIN;
}

/**
 * ERROR状態への遷移
 *
 * @warning
 *  本関数はグローバル変数stateの書き換えを行う
 */
static void
transition_to_error()
{
  led = CRGB::Red;
  FastLED.show();

  state = ST_ERROR;
}

/**
 * ボタンのホールド評価関数
 *
 * @return
 *  ボタンが長押しされていると判断した場合はtrueを返す。trueを返すタイミングは、
 *  ボタン押下時間が1秒を超えたタイミングとなっている(ボタンリリース前に発生す
 *  る)。
 */
static bool
was_hold()
{
  bool ret = false;
  static int st = 0;

  switch (st) {
  case 0:
    if (M5.BtnA.pressedFor(1000)) {
      ret = true;
      st  = 1;
    }
    break;

  case 1:
    if (M5.BtnA.wasReleased()) st = 0;
    break;
  }

  return ret;
}

/**
 * 書き込みタスクの起動
 */
static void
start_writer_task()
{
  // あまり良い方法ではないが、管理が面倒だったので書き込みタスクに渡すパス文
  // 字列を保持し続けるためにstaticローカルにしています。
  // ※状態遷移的に、writer_finish()が呼び出されるまでこの関数が呼び出されるこ
  //   とはないので内容は変更されないはず…
  
  static char path[64];

  bool err;
  time_t t;
  struct tm* tm;

  int fno = 1;

  if (enableDatetime) {
    t  = time(NULL);
    tm = localtime(&t);

    sprintf(path,
            "/output-%04d%02d%02d-%02d%02d%02d.csv",
            1900 + tm->tm_year,
            tm->tm_mon + 1,
            tm->tm_mday,
            tm->tm_hour,
            tm->tm_min,
            tm->tm_sec);

  } else {
    do {
      sprintf(path, "/output-%03d.csv", fno++);
    } while (SD.exists(path));
  }

  writer_start(path);

  // UTF-8 BOM (これがないとExcelで化ける)
  writer_puts("\xef\xbb\xbf", NULL);

  // CSVヘッダ
  writer_puts("\"タイムスタンプ\",\"電圧\",\"電流\",\"消費電力\"\n", NULL);
}

/**
 * データの出力
 *
 * @param [in] ch  出力する文字データ
 *
 * @remarks
 *  モニタ用シリアルへの出力もこの関数で行う
 */
static void
output_data(char ch)
{
  writer_push(ch, NULL);
  Serial.print(ch);
}

/**
 * 書き込みタスクの停止
 */
static void
stop_writer_task()
{
  writer_finish();
}

/**
 * 待機状態の処理の実装
 *
 * @param [in] ch   シリアルで受信した文字(受信がなかった場合はNUL文字)
 * @param [in] btn   ボタン操作状態(trueの場合は長押し検知)
 *
 * @remarks
 *  この状態では、ボタンが長押しされた場合に以下の処理を行う。
 *
 *   - 行末待ち状態への遷移
 *   - 出力ファイル名の決定
 *   - 書き込みタスクの起動
 *
 *  出力先のファイル名は "output-[番号].csv"で、未使用の空いているファイル名を
 *  自動的に探査する。
 */
static void
do_idle_state_proc(char ch, bool btn)
{
  if (btn) {
    transition_to_ready();
    start_writer_task();
  }
}

/**
 * 記録開始のための行末待ち状態の処理の実装
 *
 * @param [in] ch   シリアルで受信した文字(受信がなかった場合はNUL文字)
 * @param [in] btn   ボタン操作状態(trueの場合は長押し検知)
 *
 * @remarks
 *  本状態は行の途中からの記録を避けるために設けられている。改行文字を受信する
 *  まで状態の維
 *  持を行い、改行文字の受信をトリガとして記録状態へ遷移させる。
 *  なお、本状態中にボタンの長押しがあった場合は処理の中断とみなし、以下の処理
 *  を行う。
 *
 *    - 書き込みタスクの停止
 *    - 待機状態への遷移
 */
static void
do_ready_state_proc(char ch, bool btn)
{
  if (btn) {
    stop_writer_task();
    transition_to_idle();

  } else if (ch == '\n') {
    transition_to_record();
  }
}

/**
 * 記録状態の処理の実装
 *
 * @param [in] ch   シリアルで受信した文字(受信がなかった場合はNUL文字)
 * @param [in] btn   ボタン操作状態(trueの場合は長押し検知)
 *
 * @remarks
 *  この状態では、シリアルから受信した文字をSDカードへ記録を行う (未受信状態で
 *  呼び出された場合は記録しない) 。
 *  本状態中にボタンの長押しがあった場合は記録終了のための行末待ち状態に遷移さ
 *  せる。ただし、改行文字の受取と、ボタンの長押しの検出が同時に行われた場合は
 *  例外として待機状態への遷移を行う。
 */
static void
do_record_state_proc(char ch, bool btn)
{
  switch (ch) {
  case 0:
    if (btn) transition_to_fin();
    break;

  case '\n':
    output_data(ch);
    if (btn) {
      stop_writer_task();
      transition_to_idle();
    }
    break;

  default:
    output_data(ch);
    if (btn) transition_to_fin();
    break;
  }
}

/**
 * 記録終了のための行末待ち状態の処理の実装
 *
 * @param [in] ch   シリアルで受信した文字
 * @param [in] btn   ボタン操作状態(trueの場合は長押し検知)
 *
 * @remarks
 *  本状態は行の途中の記録終了を避けるために設けられている。改行文字を受信する
 *  まで記録を引き続き行い、改行文字の受信と同時に以下の処理を行う。
 *
 *    - 改行文字の記録
 *    - 書き込みタスクの終了
 *    - 待機状態への遷移
 *
 *  なお、本状態ではボタン押下状態は無視する。
 */
static void
do_fin_state_proc(char ch, bool btn)
{
  switch (ch) {
  case 0:
    // ignore
    break;

  case '\n':
    output_data(ch);
    stop_writer_task();
    transition_to_idle();
    break;

  default:
    output_data(ch);
    break;
  }
}

#ifdef DEBUG
/**
 * マウントされているカードのCID情報の表示
 *
 * @param [in] card  カードインタフェースのポインタ
 */
static void
show_cid_info()
{
  SdCard* card = SD.card();
  cid_t cid;

  if (card->readCID(&cid)) {
    Serial.print("Manufacturer ID: ");
    Serial.println(cid.mid, HEX);

    Serial.print("OEM ID: ");
    Serial.print((char)cid.oid[0]);
    Serial.println((char)cid.oid[1]);

    Serial.print("Product: ");
    for (uint8_t i = 0; i < 5; i++) {
      Serial.print((char)cid.pnm[i]);
    }
    Serial.println();

    Serial.print("Version: ");
    Serial.println(cid.prv);

    Serial.print("Serial number: ");
    Serial.println(cid.psn());

  } else {
    Serial.println("Failed to read CID information.");
  }
}

/**
 * マウントされているカードのCSD情報の表示
 *
 * @param [in] card  カードインタフェースのポインタ
 */
static void
show_csd_info()
{
  SdCard* card = SD.card();
  csd_t csd;

  if (card->readCSD(&csd)) {
    Serial.print("Card capacity: ");
    Serial.print(card->sectorCount());
    Serial.println(" sectors");
  }
}

/**
 * マウントされているカードのファイルシステム情報の表示
 *
 * @param [in] card  カードインタフェースのポインタ
 */
static void
show_fs_info()
{
  FsVolume* vol = SD.vol();
  FsVolume::FsInfo_t info;

  if (vol->readInfo(&fsInfo)) {
    Serial.print("File system type: ");
    switch (vol->fatType()) {
      case FAT_TYPE_FAT12:
        Serial.println("FAT12");
        break;

      case FAT_TYPE_FAT16:
        Serial.println("FAT16");
        break;

      case FAT_TYPE_FAT32:
        Serial.println("FAT32");
        break;

      case FAT_TYPE_EXFAT:
        Serial.println("exFAT");
        break;

      default:
        Serial.println("Unknown");
    }

    Serial.print("Volume size: ");
    Serial.print(fsInfo.volumeSize);
    Serial.println("MB");

    Serial.print("Free space: ");
    Serial.print(fsInfo.freeSpace);
    Serial.println("MB");

    Serial.print("Cluster count: ");
    Serial.println(fsInfo.clusterCount);

    Serial.print("Clusters free: ");
    Serial.println(fsInfo.freeClusterCount);

    Serial.print("Cluster size: ");
    Serial.print(fsInfo.bytesPerCluster);
    Serial.println("bytes");

  } else {
    Serial.println("Failed to read file system information.");
  }
}

/**
 * マウントされているカード情報の表示
 */
static void
show_card_info()
{
  /*
   * CID情報の表示
   */
  show_cid_info();

  /*
   * CSD情報の表示
   */
  show_csd_info();

  /*
   * ファイルシステム情報の表示
   */
  show_fs_info();
}
#endif /* defined(DEBUG) */

/**
 * SdFatの時刻情報取得用のコールバック関数
 *
 * @param [out] dst1  日付情報の書き込み先
 * @param [out] dst2  時刻情報(秒以上)の書き込み先
 * @param [out] dst3  時刻情報(秒未満)の書き込み先
 *
 * @remark
 *  FATエントリのタイムスタンプについて、Linuxと Windowsでタイムゾーンの扱いが
 *  異なるのでWindowsで正しい時間が表示されるように調整を行っている。
 */
static void
datetime(uint16_t* dst1, uint16_t* dst2, uint8_t* dst3)
{
  time_t t;
  struct tm* tm;

  if (enableDatetime) {
    t  = time(NULL);
    tm = localtime(&t);   // for Windows
    // tm = gmtime(&t);    // for Linux

    *dst1 = FS_DATE(1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday);
    *dst2 = FS_TIME(tm->tm_hour, tm->tm_min, tm->tm_sec);
    *dst3 = 0;
  }
}

/*
 * 公開関数
 */

/**
 * セットアップ関数
 *
 * @remarks
 *  本プログラムのエントリポイント
 */
void
setup()
{
  /*
   * Atomの初期化
   */
  M5.begin();

  /*
   * FastLEDの初期化
   */
  FastLED.addLeds<SK6812, LED_PIN, GRB>(&led, 1);
  FastLED.setBrightness(5);

  /*
   * LEDを初期化中を表す色に設定
   */
  led = CRGB::Yellow;
  FastLED.show();

  /*
   * シリアルの初期化
   *   Seirialはコンソール出力として使用。
   *   Serial2はセンサーモジュールからのデータ受信用として使用。
   */
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXPIN, TXPIN);

  /*
   * SDカードの初期化
   */
  SPI.begin(SCK, MISO, MOSI, SS);

  if (!SD.begin(SD_CONFIG)) {
    transition_to_error();
#ifdef DEBUG
    SD.initErrorHalt(&Serial);
#else /* defined(DEBUG) */
    return;
#endif /* defined(DEBUG) */
  }

#ifdef DEBUG
  show_card_info();
#endif /* defined(DEBUG) */

  /*
   * 時刻の設定
   */
  if (!datetime_initialize()) {
    FsDateTime::setCallback(datetime);
    enableDatetime = true;
  }

  /*
   * 状態をIDLEに遷移
   */
  transition_to_idle();
}

/**
 * ルーパー本体
 *
 * @remarks
 *  本プログラムの主処理。setup()呼び出し後に、本関数が繰り返し呼び出される。
 *  本関数は、シリアルからの受信（1文字単位）に対する異イベントハンドラのよう
 *  な動作を行う。一文字ごとに状態遷移を回す構成になっている。
 */
void
loop()
{
  M5.update();

  char ch = (Serial2.available())? Serial2.read(): 0;
  bool btn = was_hold();

  switch (state) {
  case ST_IDLE:     // 待機状態
    do_idle_state_proc(ch, btn);
    break;

  case ST_READY:    // 記録開始のための行末待ち状態
    do_ready_state_proc(ch, btn);
    break;

  case ST_RECORD:   // 記録中状態
    do_record_state_proc(ch, btn);
    break;

  case ST_FIN:      // 記録終了のための行末待ち状態
    do_fin_state_proc(ch, btn);
    break;

  case ST_ERROR:
    // 何もしない
    break;
  }
}
