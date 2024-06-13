/*
 * Recorder for AC power monitor
 *
 *  Copyright (C) 2024 Hiroshi Kuwagata <kgt9221@gmail.com>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef __WRITER_H__
#define __WRITER_H__

#ifdef __cplusplus
extern "C" {
#endif /* defined(__cplusplus) */

/**
 * ライターモジュールの動作開始
 *
 * @param [in] path   書き込み対象のファイルへのパス
 *
 * @retrun
 *   処理に成功した場合は0を、失敗した場合は0以外の値を返す。
 *
 * @remark
 *  本関数を呼び出すと、バックグラウンドでファイル書き込みを行うタスクを起動
 *  する。本関数呼び出し後、writer_push()関数でデータを書き込むことができる。
 *
 * @warning
 *  引数pathで渡した文字列は、writer_finish()を呼び出して書き込みタスクを停止
 *  させるまで、呼び出し側で保持する必要がある。
 */
int writer_start(const char* path);

/**
 * データ書き込み
 *
 * @param [in] s  書き込む文字列
 * @param [out] dst 物理書き込みが行われたか否かの結果を返すポインタ(NULL可)
 *
 * @retrun
 *   処理に成功した場合は0を、失敗した場合は0以外の値を返す。
 *
 * @remark
 *  本関数で書き込まれたデータは、まず内部バッファに登録される。その結果内部バ
 *  ッファが満杯になった場合、書き込みタスクによって書き込みが行われる（この場
 *  合、引数dstで指定された領域にtrueが書き込まれる）。
 *  内部では、二面バッファによる管理が行われるのでバックグラウンドでの書き込み
 *  が完了していない状態でも本関数によるデータの書き込みが可能である。
 *
 * @warning
 *  書き込みレートが高すぎて二面バッファでもまかないきれない場合の挙動は未定義
 *  となる。
 */
int writer_puts(const char* s, bool *dst);

/**
 * データ書き込み
 *
 * @param [in] b  書き込むデータ
 * @param [out] dst 物理書き込みが行われたか否かの結果を返すポインタ(NULL可)
 *
 * @retrun
 *   処理に成功した場合は0を、失敗した場合は0以外の値を返す。
 *
 * @remark
 *  本関数で書き込まれたデータは、まず内部バッファに登録される。その結果内部バ
 *  ッファが満杯になった場合、書き込みタスクによって書き込みが行われる（この場
 *  合、引数dstで指定された領域にtrueが書き込まれる）。
 *  内部では、二面バッファによる管理が行われるのでバックグラウンドでの書き込み
 *  が完了していない状態でも本関数によるデータの書き込みが可能である。
 *
 * @warning
 *  書き込みレートが高すぎて二面バッファでもまかないきれない場合の挙動は未定義
 *  となる。
 */
int writer_push(uint8_t b, bool* dst);

/**
 * ライターモジュールの動作終了
 *
 * @retrun
 *   処理に成功した場合は0を、失敗した場合は0以外の値を返す。
 *
 * @remark
 *  書き込みを終了させ、書き込みタスクを終了させる。この時、バッファに残ってい
 *  たデータはフラッシュされる。
 */
int writer_finish();

#endif /* !defined(__WRITER_H__) */

#ifdef __cplusplus
}
#endif /* defined(__cplusplus) */
