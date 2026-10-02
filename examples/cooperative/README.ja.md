# メインスレッドでの協調実行

[English](README.md) | 日本語

KinoWASM はホスト関数からWASMの実行を中断し、アプリケーションへ制御を返せます。後から `kinowasm_resume` を呼ぶと、呼び出しスタック・ローカル変数・演算途中の値を保持したまま、そのホスト関数の呼び出し直後から再開します。次のフレーム、会話の選択、エンジン側の処理完了を、ホスト関数内でブロックせずに待つ用途に適しています。

WASM側の処理全体をフレーム単位の状態機械へ書き換える必要はなく、再開するタイミングをホスト側で決められます。ゲームエンジンではメインスレッドからinvoke/resumeすることで、ホスト関数からメインスレッド専用APIを扱えます。この例ではワーカースレッドやAsyncify等のゲストコード変換を使いません。

## 実行の契約

1. WASMが引数・戻り値なしの `env.next_frame()` を呼びます。
2. ホスト関数は `RES_SUCCESS` の代わりにアプリ独自の中断コード `0x2000` を返します。
3. `kinowasm_invoke` または `kinowasm_resume` がそのコードをホストへ返します。
4. 後のフレームで、ホストは `kinowasm_resume(store, &args)` を1回呼びます。
5. WASMは `next_frame()` の直後から続行します。中断した呼び出しのホスト関数をもう一度実行するわけではありません。

共通ブリッジは既知の中断コードを `1`、正常完了を `0`、失敗を `-1` に変換します。すべてのエラーを再開対象として扱わず、この例の中断コードだけを再開します。

`kinowasm_resume` の引数配列は、完了したexport関数の戻り値を受け取るためのものです。中断したホスト関数の戻り値を差し替える機能ではありません。ホスト関数の戻り値は中断時に保持されます。非同期処理の結果は、戻り値なしの待機importで中断し、再開後に別のホスト関数で取得する形にしてください。`kinowasm_callinfo_t` やその引数・戻り値ポインタは、コールバック終了後に保持しません。

協調実行なので、中断するホスト関数へ到達しない長時間の計算ループはフレームを止めます。各実行区間をフレーム予算内に収め、ホスト関数も処理開始・準備確認を行って速やかに返してください。共通ブリッジはグローバルなランタイム状態を使う**単一セッションのデモ**です。1インスタンス・同一スレッドで使用し、中断中に別の独立したinvokeを挟みません。汎用エンジンプラグインや複数インスタンス用スケジューラではありません。

## ネイティブ例のビルドと実行

Windows x64向けの通常のツールチェーンとwabtを用意し、ルートから実行します。

```bat
cmake --preset x64-Release -DKINORUNTIME_BUILD_EXAMPLES=ON
cmake --build out/build/x64-Release -j 1
ctest --test-dir out/build/x64-Release --output-on-failure
out\build\x64-Release\examples\cooperative\kino_cooperative_demo.exe out\build\x64-Release\examples\cooperative\cooperative.wasm
```

`out/build/<preset>/examples/cooperative/` に `kino_cooperative.dll`、importライブラリ、WASM、コンソールドライバを生成します。例のビルドは既定OFFです。DebugではプリセットとパスをDebugへ変更してください。

| ホストのフレーム | WASMの処理 | 状態 |
|---|---|---|
| 1 | 10を通知、ネストした関数内で待機 | 中断 |
| 2 | 再開、20を通知、待機 | 中断 |
| 3 | 再開、30を通知、待機 | 中断 |
| 4 | 再開、10 + 20 + 30を返す | 完了: 60 |

ドライバは4段階の実行、完了後の再オープン、中断中のキャンセルと再オープンを検証します。共通ブリッジはWASMバイト列をコピーし、store/moduleのバッファを `kino_example_close` まで保持します。store 100 MiB、module 20 MiBと参考ホストのシステムアリーナはデモ用の予算で、ランタイムの最低必要量ではありません。

## Unity: Update

[C#コンポーネント](unity/KinoCooperativeExample.cs) は `DllImport` でデモのC ABIを呼び、`Update` で1フレームに1回進めます。

1. Releaseでネイティブ例をビルドします。
2. `kino_cooperative.dll` を `Assets/Plugins/x86_64/` へ配置し、Plugin設定で必要なWindows x86_64 Editor/Standaloneを有効にします。
3. `cooperative.wasm` を `Assets/StreamingAssets/` へ配置します。
4. C#スクリプトをプロジェクトへコピーし、1つのGameObjectへ追加します。
5. シーンを実行すると、10・20・30の進捗と完了値60を出力します。コンポーネントの無効化でセッションを終了・キャンセルし、再有効化で開始し直します。

Windowsデスクトップ向けの組み込みテンプレートです。他のプラットフォームではStreamingAssetsの読み込みやネイティブリンク方法が異なります。Unity Editor/player上の動作はこの環境では未検証です。[Unityのネイティブプラグイン資料](https://docs.unity.com/en-us/engine/6000.0/manual/scripting/compilation-and-code-reload/plug-ins/native/overview)も参照してください。

## Unreal Engine: Tick

[Actor](unreal/KinoCooperativeActor.h) と[実装](unreal/KinoCooperativeActor.cpp) は同じC ABIを使います。ゲームモジュールへコピーし、`cooperative_bridge.h` をincludeパスへ配置します。Releaseのimportライブラリをリンクし、対応するDLLを `BeginPlay` より前に読み込めるよう配置してください。

例えばモジュール内の `ThirdParty/KinoCooperative/` にファイルを配置し、既存の `.Build.cs` コンストラクタへ次を追加します（`System.IO` が必要です）。

```csharp
if (Target.Platform == UnrealTargetPlatform.Win64)
{
    string kino_dir = Path.Combine(ModuleDirectory, "ThirdParty", "KinoCooperative");
    PublicIncludePaths.Add(Path.Combine(kino_dir, "include"));
    PublicAdditionalLibraries.Add(Path.Combine(kino_dir, "lib", "kino_cooperative.lib"));
    PublicDelayLoadDLLs.Add("kino_cooperative.dll");
    RuntimeDependencies.Add("$(TargetOutputDir)/kino_cooperative.dll",
        Path.Combine(kino_dir, "bin", "kino_cooperative.dll"));
}
```

ヘッダーを `include/`、importライブラリを `lib/`、DLLを `bin/` に置きます。EditorではDLLをプロジェクトの `Binaries/Win64/` のEditorターゲットの隣にも配置するか、モジュール起動時に明示的にロードしてください。WASMは `Content/KinoWASM/cooperative.wasm` へ置き、レベルに1つのActorを配置してPIEを実行します。`Tick` で1回ずつinvoke/resumeし、`EndPlay` で終了します。

Actorは通常のファイルとしてWASMを読みます。パッケージ版では非アセットファイルとして配置するか、プロジェクトのアセット読み込み経路へ変更してください。Unreal Build Tool・PIE・パッケージ版の動作はこの環境では未検証です。Epicの[Actor ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine)と[外部ライブラリの組み込み資料](https://dev.epicgames.com/documentation/en-us/unreal-engine/integrating-third-party-libraries-into-unreal-engine)も参照してください。

これらは各エンジンのメインループへ接続する例で、公式Unity/Unreal対応や他の全WebAssemblyランタイムとの比較を表明するものではありません。製品へ組み込む際のホストバックエンドとライフサイクルは[利用者ガイド](../../docs/ja/QuickGuide.md)から確認してください。
