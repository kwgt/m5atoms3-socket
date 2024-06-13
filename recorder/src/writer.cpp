/*
 * Recorder for AC power monitor
 *
 *  Copyright (C) 2024 Hiroshi Kuwagata <kgt9221@gmail.com>
 */

#include <stdint.h>

#include <FastLED.h>
#include <SdFat.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <writer.h>

//! バッファのサイズ
#define BUFF_SIZE       (8192)

//! デフォルトのエラーコード
#define DEFAULT_ERROR   (__LINE__)

//! タスク終了通知イベント
#define TASK_COMPLETE   (0x000000001)

//! LED点灯用のウェイト時間 (ミリ秒で指定)
#define EMIT_DURATION   (500)

//! SDカードインタフェースオブジェクト
extern SdFat SD;

//! FastLED用フレームバッファ
extern CRGB led;

//! 処理状態
static int state = 0;

//! 排他制御用のミューテックス
static SemaphoreHandle_t mutex = NULL;

//! 書き込みタスクとの連絡用キュー
static QueueHandle_t queue = NULL;

//! 書き込みタスクのハンドラ
static TaskHandle_t task = NULL;

//! イベント通知用のイベントグループ
static EventGroupHandle_t events = NULL;

//! バッファ（表面）
static uint8_t buff_plane1[BUFF_SIZE];

//! バッファ（裏面）
static uint8_t buff_plane2[BUFF_SIZE];

//! 現在のデータ追加用のバッファの面
static uint8_t* cur_buff = buff_plane1;

//! 現在のバッファ使用量
static size_t used = 0;

//! 書き込み情報
struct Command {
  enum {
    OP_FLUSH,
    OP_EXIT
  } op;

  uint8_t* data;
  size_t size;
};

/*
 * 内部関数の定義
 */
static void
writer_task_func(void* arg)
{
  char *path = (char*)arg;
  bool error;
  Command cmd;
  SdFile file;

  error = !file.open(path, O_WRONLY | O_CREAT | O_TRUNC);

  /*
   * コマンド受信ループ
   */
  while (true) {
    if (xQueueReceive(queue, &cmd, portMAX_DELAY) == pdPASS) {
      if (!error) {
        if (cmd.size > 0) {
          /*
           * LEDを書き込み色に変更
           */
          led = CRGB::Red;
          FastLED.show();

          if (file.write((const uint8_t*)cmd.data, cmd.size) == cmd.size) {
            error = !file.sync();
          } else {
            error = true;
          }

          // 受信レートと、SDカードへの書き込みレートを考えると時間的に余裕が
          // 十分あるので書き込みインディケータが視認できるようにディレイをか
          // ける。
          vTaskDelay(pdMS_TO_TICKS(EMIT_DURATION));

          /*
           * LEDを書き込み後の色に変更
           */
          if (error) {
            // エラーが有った場合はマゼンタ
            led = CRGB::Magenta;
          } else {
            // 正常に書き込めた場合は緑
            led = CRGB::DarkGreen;
          }
          FastLED.show();
        }
      }

      if (cmd.op == Command::OP_EXIT) break;

    } else {
      ESP_LOGE("writer_task_func", "command receive failed");
    }
  }

  file.close();

  xEventGroupSetBits(events, TASK_COMPLETE);
  vTaskDelete(NULL);
}

static int
push_byte(uint8_t b, bool* dst)
{
  int ret;
  Command cmd;

  /*
   * initialize
   */
  ret   = 0;

  /*
   * push data
   */
  cur_buff[used++] = b;

  if (used == BUFF_SIZE) {
    cmd.op   = Command::OP_FLUSH;
    cmd.data = cur_buff;
    cmd.size = BUFF_SIZE;

    if (xQueueSend(queue, &cmd, portMAX_DELAY) != pdPASS) {
      ESP_LOGD("writer_push", "Queue fauled.");
      ret = DEFAULT_ERROR;
    }

    cur_buff = (cur_buff == buff_plane1)? buff_plane2: buff_plane1;
    used     = 0;

    // 書き込みのエッジのみマーク
    if (*dst == false) *dst = true;
  }

  /*
   * post process
   */
  // nothing

  return ret;
}

/*
 * 公開関数の定義
 */

int
writer_start(const char* path)
{
  int ret;
  BaseType_t err;

  /*
   * initialize
   */
  ret = 0;

  /*
   * argument check
   */
  if (path == NULL) ret = DEFAULT_ERROR;

  /*
   * state check
   */
  if (state != 0) ret = DEFAULT_ERROR;

  /*
   * start task
   */
  if (!ret) {
    queue = xQueueCreate(3, sizeof(Command));
    if (queue == NULL) ret = DEFAULT_ERROR;
  }

  if (!ret) {
    mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) ret = DEFAULT_ERROR;
  }

  if (!ret) {
    events = xEventGroupCreate();
    if (events == NULL) ret = DEFAULT_ERROR;
  }

  if (!ret) {
    // 無線系を使っていないのでPRO_CPUが余ってるはず…
    err = xTaskCreateUniversal(writer_task_func,
                               "Writer task",
                               4096,
                               (void*)path,
                               1,
                               &task,
                               PRO_CPU_NUM);
    if (err != pdPASS) ret = DEFAULT_ERROR;
  }

  /*
   * transition state
   */
  if (!ret) state = 1;

  /*
   * post process
   */
  if (ret) {
    if (queue != NULL) vQueueDelete(queue);
    if (mutex != NULL) vSemaphoreDelete(mutex);
    if (events != NULL) vEventGroupDelete(events);
    if (task != NULL) vTaskDelete(task);

    queue  = NULL;
    mutex  = NULL;
    events = NULL;
    task   = NULL;
  }

  return 0;
}

int
writer_puts(const char* s, bool* dst)
{
  int ret;
  int err;
  int lock;
  bool wrote;
  int i;

  /*
   * initialize
   */
  ret   = 0;
  lock  = 0;
  wrote = false;

  /*
   * mutex lock
   */
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    lock = !0;
  } else {
    ret = DEFAULT_ERROR;
  }

  /*
   * state check
   */
  if (!ret) {
    if (state != 1) ret = DEFAULT_ERROR;
  }

  /*
   * push data
   */
  if (!ret) {
    for (i = 0; s[i] != '\0'; i++) {
      err = push_byte((uint8_t)s[i], &wrote);
      if (err) {
        ret = DEFAULT_ERROR;
        break;
      }
    }
  }

  /*
   * put return parameter
   */
  if (!ret) {
    if (dst != NULL) *dst = wrote;
  }

  /*
   * post process
   */
  if (lock) xSemaphoreGive(mutex);

  return ret;
}

int
writer_push(uint8_t b, bool* dst)
{
  int ret;
  int err;
  int lock;
  bool wrote;

  /*
   * initialize
   */
  ret   = 0;
  lock  = 0;
  wrote = false;

  /*
   * mutex lock
   */
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    lock = !0;
  } else {
    ret = DEFAULT_ERROR;
  }

  /*
   * state check
   */
  if (!ret) {
    if (state != 1) ret = DEFAULT_ERROR;
  }

  /*
   * push data
   */
  if (!ret) {
    err = push_byte(b, &wrote);
    if (err) ret = DEFAULT_ERROR;
  }

  /*
   * put return parameter
   */
  if (!ret) {
    if (dst != NULL) *dst = wrote;
  }

  /*
   * post process
   */
  if (lock) xSemaphoreGive(mutex);

  return ret;
}

int
writer_finish()
{
  int ret;
  int lock;
  Command cmd;

  /*
   * initialize
   */
  ret  = 0;
  lock = 0;

  /*
   * mutex lock
   */
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
    lock = !0;
  } else {
    ret = DEFAULT_ERROR;
  }

  /*
   * state check
   */
  if (ret) {
    if (state != 1) ret = DEFAULT_ERROR;
  }

  /*
   * queue command
   */
  if (!ret) {
    cmd.op   = Command::OP_EXIT;
    cmd.data = cur_buff;
    cmd.size = used;

    if (xQueueSend(queue, &cmd, portMAX_DELAY) != pdPASS) {
      ESP_LOGD("writer_finish", "Queue fauled.");
      ret = DEFAULT_ERROR;
    }
  }

  /*
   * wait for task complete
   */
  if (!ret) {
    xEventGroupWaitBits(events, TASK_COMPLETE, pdTRUE, pdTRUE, portMAX_DELAY);
  }

  /*
   * state transition
   */
  if (!ret) state = 2;

  /*
   * post process
   */
  if (lock) xSemaphoreGive(mutex);

  if (!ret) {
    if (queue != NULL) vQueueDelete(queue);
    if (mutex != NULL) vSemaphoreDelete(mutex);
    if (events != NULL) vEventGroupDelete(events);

    queue    = NULL;
    mutex    = NULL;
    events   = NULL;
    task     = NULL;

    cur_buff = buff_plane1;
    used     = 0;
    state    = 0;
  }

  return ret;
}
