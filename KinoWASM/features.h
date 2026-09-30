/* features.h
 *
 * KinoRuntime のビルド時 feature gate を集約するヘッダ。
 *
 * gate は「パーサがそのモジュールを受理するか」だけを制御する。実行エンジン (core) 側には
 * gate が無く、core が実装していない命令はパーサを通しても core build で必ず失敗する。
 * そのため「core が実装していない機能」は gate ではなくパーサで常時拒否している:
 *   - threads & atomics (0xFE prefix): core 未実装のため常に ERR_FEATURE_DISABLED
 *     (shared memory の宣言自体は atomics を使わなければ有効なので受理する)
 * 逆に core が完全実装している機能 (exception handling) と、core が関与しないホスト側の
 * 機能 (WASI / 拡張ホスト関数) は gate を持たず常時有効。
 *
 * 各 gate は次の場所で参照される:
 *   - parser/kw_parser.c と parser 配下の .inc 群: prefix opcode と value type の検査
 *   - store/kw_store.c: const 式評価 (v128)
 *
 * Gate を OFF にした場合の挙動:
 *   - 該当する opcode / 値型を含む wasm モジュールは load 時に
 *     ERR_FEATURE_DISABLED を返して reject される
 *   - すでに load 済みのモジュールは影響なし
 *
 * 新しい gate を追加するときは:
 *   1. ここに `#ifndef KINOWASM_ENABLE_XXX` ... `#endif` で宣言
 *   2. CMakeLists.txt の `option()` を追加
 *   3. 該当箇所を `#if KINOWASM_ENABLE_XXX` で囲う
 *   4. test/CI で OFF 構成のビルド + testsuite 実行を確認
 */
#ifndef KINOWASM_FEATURES_H
#define KINOWASM_FEATURES_H

/* SIMD (0xFD prefix, ~232 op)
 * OFF にすると v128 type も reject される。binary サイズへの影響大 (~25-50KB)。
 * CMake のデフォルトは OFF (CMakeLists の option 参照)。下記 #define はその gate を
 * 「ON にしたとき」の値で、CMake option=OFF 時は -DKINOWASM_ENABLE_SIMD=0 が優先される。
 * 注: core は 0xFD を実装していないため、ON にしてもパーサを通るだけで実行はできない
 * (SIMD 命令を含む関数は core build で失敗する)。 */
#ifndef KINOWASM_ENABLE_SIMD
#define KINOWASM_ENABLE_SIMD 1
#endif

#endif /* KINOWASM_FEATURES_H */
