# WaveGain コード精査と改善提案

調査日: 2026年10月16日  
対象コミット: `ef3efebc933ae075980608d232d48256b9b8bae7`  
対象読者: WaveGainの保守担当者

WaveGainには改善の余地がある。最優先は、元ファイルを置き換える前の出力検証と、WAVおよびAIFFのヘッダー処理である。有効なWAVがメタデータの内容によって破損した出力に置き換わる事例と、AIFF変換やログ出力でのメモリ破壊を再現した。ReplayGainフィルターの追加高速化より、これらの修正を先行させるべきである。

既存の回帰テストとFFmpeg相互運用テストは通過している。フィルターのスカラーとSIMDの一致、88.2 kHzの補正、大容量ファイル、PCM出力、DC補正などのテスト資産は維持する価値がある。ただし、その成功は以下の入力やエラー経路の安全性まで保証していない。

## 改善の実施状況

2026年10月16日、`fix/review-safety-2026-10-16`ブランチでR1からR11に対する修正を実施した。以下の指摘本文は、対象コミットに対する調査記録として保持している。

- WAVはチャンク境界を検証して更新し、メタデータと奇数長パディングを保持する。
- AIFF/AIFCはmono/stereoの16 bit PCMに限定し、長さを検証する。WAV変換時にはヘッダーを新規作成する。
- ログの固定長メッセージバッファを廃止し、gainの長さと数値を検証する。負のPCMの左シフトと量子化の型の別名参照を解消する。
- I/Oと最終化の失敗を伝播し、原本を保持する。ファイル処理失敗を終了コードへ反映し、意図的なスキップと区別する。
- Windowsは排他的なテンポラリ作成と、原本を先に削除しない置換方式へ変更した。R6のWindows実機検証は未完了である。

回帰テストは`test/review_regressions.py`と`test/review_io.c`に追加した。`make check-sanitize`ではASanとUBSanを有効にし、UBSan検出時も失敗として停止する。実施状況と残件は`TODO`にも記載している。gainの既存プラットフォーム別バイト順と通常入力の音声出力は維持し、極端な増幅では量子化前に飽和させる。トゥルーピークなどの追加機能は残件とした。

修正後のASan/UBSan付き`make check-sanitize`と、並列makeでの`make -j4 verified-release`は通過した。通常のWAV変換120ケースについて、修正前の対象コミットと出力がバイト単位で一致した。比較には44.1/48/88.2/96 kHz、mono/stereo、16/24/32 bit出力、5種類のディザ設定を含む。`make debug`に残る警告は、変更していないライセンスコメントの文字コードに関するものだった。

## 調査範囲と検証方法

ルートのCソースと関連ヘッダーを対象とし、`audio.c`、`wavegain.c`、`main.c`、`w64.c`、`gain_analysis.c`、`dither.c`、`recurse.c`、`misc.c`を中心に、同梱のgetopt実装、Makefile、テスト、TODO、既存の調査資料を確認した。

検証環境はmacOS arm64、Apple Clang 21.0.0。Makefile上の`gcc`もこの環境ではApple Clangである。Windows固有の指摘はコード読解に基づき、Windows上の実行検証とは区別する。

実施した検証:

- `make debug`によるビルド。
- `make check`による文書整合性、フィルター、portability、Wave64、TODO回帰テスト。
- `make check-interop`によるFFmpegとのWave64、88.2 kHz、RIFF相互運用テスト。
- AddressSanitizerとUndefinedBehaviorSanitizerを有効にした別バイナリによる、自作の小規模WAVとAIFFの検証。

追加検証用バイナリのビルド方法:

```sh
cc -D_FILE_OFFSET_BITS=64 -DHAVE_CONFIG_H -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -o /tmp/wavegain-review-san *.c -lm
```

初回調査ではソースコードの修正を行っていない。以下の指摘の行番号は、修正前の対象コミットに対応する。

## 優先度と証拠の一覧

P1はデータ破損またはメモリ安全性に関わり優先修正すべき事項、P2は互換性、移植性、異常検出の改善事項とする。「再現」は自作入力で実行確認済み、「読解」はコードから確認した問題である。

| ID | 優先度 | 指摘 | 証拠 |
| --- | --- | --- | --- |
| R1 | P1 | WAVメタデータ内の文字列で出力ヘッダーを誤更新し原本を置換 | 再現 |
| R2 | P1 | AIFFからWAVへの変換でNULL参照 | 再現 |
| R3 | P1 | AIFFのCOMM長を使う無制限のスタック確保 | 再現 |
| R4 | P1 | ログ出力の固定長バッファを超過 | 再現 |
| R5 | P1 | 8 bit AIFFの解釈誤りと出力時のヒープ超過 | 再現 |
| R6 | P1 | Windowsで原本を削除した後に置換する | 読解 |
| R7 | P2 | RIFFの奇数長チャンクを読み飛ばせない | 再現 |
| R8 | P2 | gainチャンクのサイズと数値の検証不足 | 再現と読解 |
| R9 | P2 | 通常の負のPCM読み取りに未定義動作がある | 再現 |
| R10 | P2 | 最終化時のI/O失敗を十分に伝播しない | 読解 |
| R11 | P2 | ファイル処理失敗でも成功の終了コードを返す | 再現と読解 |

## 優先修正すべき不具合

### R1 WAVヘッダーの誤更新と原本の破損

対象: [audio.c](../../audio.c) 1264行付近の`write_wav_header()`、[wavegain.c](../../wavegain.c) 621行付近。

`write_wav_header()`はチャンク構造をたどらず、バイト単位で`RIFF`、`fmt `、`gain`を探す。ヘッダーバッファの終端も探索条件に含まれない。メタデータのペイロードに`fmt `が含まれると、本来のフォーマットチャンクより先に一致し、その後のバイト列をフォーマットとして書き換える。

再現条件は、通常のmono 16 bit WAVの本物の`fmt `チャンクの前に、以下の有効なJUNKチャンクを置いて`-y`を実行すること。

```python
junk = b'JUNK' + struct.pack('<I', 8) + b'fmt ' + struct.pack('<I', 0x10000000)
```

出力では本物の`fmt `識別子が上書きされ、FFmpegの`ffprobe`が`no 'fmt ' or 'XMA2' tag found`として拒否した。WaveGainは終了コード0を返し、破損した出力で原本を置き換えた。値を0にするとUBSanはポインターのunsigned offset overflowも報告した。

修正方針は、読み取り時に検証したチャンクの位置と長さを保持し、その位置だけを更新すること。すべての参照と書き込みにバッファ境界検査を入れ、構造検証失敗時は原本を置換しない。Wave64で採用されているサイズに基づくチャンク走査が参考になる。

### R2 AIFFからWAVへの変換でクラッシュ

対象: [audio.c](../../audio.c) 565行付近、1140行付近、1288行付近。

AIFFの読み取りでは`opt->header = NULL`となる。一方、`-b 2`などでWAV出力を選ぶと、WAVライターが既存のRIFFヘッダーを前提としてNULLに対して`memcmp()`を実行する。

44.1 kHz mono 16 bit AIFFに対する次の操作でASanのNULL参照によるSEGVを再現した。

```sh
/tmp/wavegain-review-san -y -b 2 input.aif
```

WAVヘッダーを新規に構築する経路と、既存のWAVメタデータを引き継ぐ経路を用意する。入力コンテナ、PCM形式、出力コンテナを独立して扱うことで、変換時の前提を明確にできる。

### R3 AIFFのCOMM長によるスタック破壊

対象: [audio.c](../../audio.c) 505行付近の`alloca(len)`。

ファイルに記録された32 bitのCOMMチャンク長を、上限確認なしでスタック確保に使っている。実データを読む前にスタックを使い切る。

`FORM`、`AIFF`、`COMM`の各識別子と、COMM長`0x40000000`だけを含む短いファイルを解析すると、ASanがstack-overflowを報告した。

AIFFの解析に必要な固定長部分だけを小さなバッファへ読み、残りは検証してスキップする。チャンクの長さ、FORMの範囲、実ファイル長の関係を確認する。AIFFのエラー経路では先に確保した`aiff`構造体も解放する。

### R4 ログ出力のバッファオーバーフロー

対象: [misc.c](../../misc.c) 72行付近の`write_log()`、[main.c](../../main.c) 480行付近。

`write_log()`は1024バイトの配列に`vsprintf()`で書き込む。確認しているのは書式文字列の長さだけであり、`%s`から展開される引数の長さを制限できない。

ログを有効にし、`-g`へ2000文字の引数を渡すと、コマンドラインをログへ書く処理でASanがstack-buffer-overflowを報告した。別経路として、ログファイル名も512バイトのグローバル配列へ`strcpy()`される。

ログは`vfprintf()`でファイルへ直接書くか、必要長を計算したバッファへ書く。ログファイル名には長さの検証または動的確保を導入する。固定長バッファを使う場合は、黙って切り詰めるかエラーにするかも定義する。

### R5 8 bit AIFFの誤解釈と出力時のメモリ破壊

対象: [audio.c](../../audio.c) 560行付近、890行付近、1556行付近、[wavegain.c](../../wavegain.c) 354行付近。

AIFFリーダーは8 bitを受け付けるが、共有の`wav_read()`は8 bitデータから128を引く。符号付きの8 bit AIFFの無音である全ゼロが、負のフルスケールとして扱われる。実行結果でも無音に対して左DC補正値32768が表示された。

さらに、8 bit AIFFを既定の形式で`-y`処理すると、AIFF出力を16 bitの変換経路へ送る一方、`samplesize`は8のままになる。`write_audio_16bit()`が1サンプルあたり1バイトで確保された領域へ2バイトを書き、ASanがheap-buffer-overflowを報告した。

現行のヘルプは16 bit AIFFのみを案内している。まず未対応の8 bit入力を明示的に拒否する修正が小さい。対応を継続する場合は、入力の符号、出力のビット数、バッファ幅、SSNDの長さを一貫させる必要がある。

### R6 Windowsの原本置換手順

対象: [wavegain.c](../../wavegain.c) 596行付近、[audio.c](../../audio.c) 63行付近。

Windowsでは`remove(filename)`で原本を削除してから`rename()`する。削除後のrename失敗やプロセス停止で、元のパスからファイルが失われる。テンポラリ作成も`_mktemp_s()`と`fopen()`の組み合わせであり、名前の決定と作成の間に競合がある。

排他的なファイル作成と、バックアップを伴う置換処理を設計する。候補となる`ReplaceFileW`相当のAPIには失敗状態ごとの取り扱いが必要であり、単にAPIを置き換えるだけで原本保存が保証されるとは限らない。[Microsoft ReplaceFile仕様](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilea)

Windowsでの権限不足、ファイルロック、置換失敗、非ASCIIのパスを含む実機検証を受け入れ条件にする。

## 互換性とCの安全性に関する改善

### R7 RIFFの奇数長チャンク

対象: [audio.c](../../audio.c) 359行付近の`find_wav_chunk()`。

未知のチャンクを読み飛ばす際、ペイロード長だけをスキップし、奇数長の後ろにあるパディングを飛ばしていない。1バイトのJUNKと1バイトのパディングを`fmt `の前に置いた有効なWAVが、`Failed to find fmt chunk`として拒否された。

RIFFではチャンクデータをWORD境界にそろえ、パディングはチャンク長に含めない。スキップ量を検証済みの`length + (length & 1)`にする。[Microsoft RIFF仕様](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-)

出力側も、ビット数変換によってdataの奇偶が変化する場合にパディングを再構築する必要がある。これは今回の入力側の再現とは別の確認項目である。

### R8 gainチャンクの検証と移植性

対象: [audio.c](../../audio.c) 107行付近、153行付近、675行付近。

WAVではgainチャンクの長さが8かを確認せず8バイトを読み、短い読み取りでも警告後にデコードを続ける。macOSではbig endian、それ以外ではlittle endianという既知の非互換性も残っている。

`read_d64_be()`と`read_d64_le()`は指数の適用に`1 << exponent`を使う。gainとして`2^40`を与えるとUBSanが「32 bit intに対してシフト量40」と報告した。長さ0のgainチャンクでも、後続チャンクのバイトをdoubleとして解釈し、過大なシフトを再現した。

長さと読み取り結果を必須条件にし、IEEE-754のビット復元または検証された数値変換へ置き換える。適用するscaleは正かつ有限、逆数も表現可能であることを確認する。既存のmacOS形式を扱う移行方針は、曖昧な自動判定を避けて定義する。Wave64のgain処理はサイズと値の検証を持ち、改善の参考になる。

### R9 PCM読み取りの未定義動作

対象: [audio.c](../../audio.c) 908行付近、915行付近、927行付近、948行付近、[wavegain.c](../../wavegain.c) 68行付近。

16 bit PCMの復元で、負になった`signed char`を左シフトしている。通常の正負のサイン波を読むだけでUBSanが`left shift of negative value -1`を報告した。24 bitと32 bitにも同型の式がある。

バイトの結合は`uint32_t`などの符号なし型で行い、その後に値域を明示して符号を復元する。端点の`INT16_MIN`、24 bit最小値、`INT32_MIN`を含むテストを追加する。

量子化側の`ROUND64`にはdoubleへのポインターを`Int64_t*`として参照する型の別名参照も残る。こちらは今回の実行で誤出力を立証したものではないが、Cの移植性上は修正対象となる。ビット取り出しは`memcpy()`で行い、既存の丸め結果と出力バイトを比較する。`lrint()`などへの変更は丸めとディザの意味を確認してから行う。

### R10 最終化時の失敗検出とメモリ使用量

対象: [audio.c](../../audio.c) 1188行付近、1213行付近、1341行付近。

PCM書き込みの失敗は確認されているが、RIFFの最終化には検査不足が残る。末尾データを一括`malloc()`して成否を確認せず読み込み、短い読み取りでも警告だけで要求長を書き込む。ヘッダー再書き込みの戻り値や複数のseek結果も呼び出し元へ十分に伝播しない。`fclose()`成功だけでは、先行する読み取り、seek、書き込みの失敗をすべて検出できない。

また、gain挿入用の確保に失敗した場合、未初期化の`r`で`memcpy()`へ進む。`make debug`でもこの警告が出た。`write_gains()`冒頭の構造体およびPCM配列の確保にも未確認箇所がある。

末尾メタデータは固定サイズのバッファでコピーし、入力と出力のI/O結果を毎回確認する。失敗を単一の後始末経路へ伝え、原本置換を禁止する。Wave64の`copy64()`は有用な先例である。

### R11 ファイル処理失敗と終了コード

対象: [main.c](../../main.c) 169行付近の`process_files()`、647行付近。

解析失敗はファイルをリストから除外して続行し、書き込み失敗もメッセージを表示して続行する。最終的に`process_files()`は0を返す。今回、奇数長チャンクで解析に失敗したWAVも終了コード0だった。

一部失敗と全件失敗を集計し、少なくとも失敗があった実行には非ゼロを返す。意図的な「処理済みのためスキップ」は別に扱う。バッチ処理側が失敗を検出できるようになり、完了メッセージも実際の結果に一致する。

## 音響処理の改善提案

以下は、クラッシュなどの再現済み不具合と区別した設計改善である。既定の音声出力を変える場合は、互換性の扱いを明示する。

### ディザとノイズシェーピングの測定

[dither.c](../../dither.c)の係数は`F44_0`から`F44_3`の4種類で、`F44_0`はノイズシェーピングなしの場合に使われる。係数は固定され、`Init_Dither()`はサンプルレートを受け取らない。同じ離散時間フィルターを8 kHzから96 kHzまで使うため、Hz単位で見たノイズ配分が変わる。現状から全サンプルレートで同等の聴感上の効果が得られるとは判断できない。

無音、低レベル正弦波、DC、フルスケール近傍について、8/16/24/32 bitと全対応サンプルレートのノイズスペクトル、平均誤差、RMS、クリップ数を測定する。結果に応じ、適用レートを制限するか、レート別の係数を導入する。

非整形ディザの`LastRandomNumber`は`Init_Dither()`で初期化されず、乱数状態もプロセス内で継続する。この挙動を仕様として定め、ファイル単位の再現性が必要なら状態を明示的に管理する。今回、これによる可聴な問題は測定していない。

### サンプルピークとトゥルーピーク

[wavegain.c](../../wavegain.c)の`corrected_peak()`はサンプル値の最大振幅を測る。サンプル間を再構成した波形の最大値を測るトゥルーピークとは目的が異なる。配信や後段変換向けに、任意のトゥルーピーク測定とヘッドルーム指定を追加する余地がある。[ITU-R BS.1770](https://www.itu.int/rec/R-REC-BS.1770-4-201510-S)

これは現行ReplayGainとの互換性を保つため追加モードとし、既存のピーク計算を無条件に置き換えない。補間フィルターと測定誤差を規定し、既知のサンプル間ピークを持つ信号で検証する。

### リミッター適用後の結果表示

現行のNew Peakはリミッター前の予測値として文書化されている。この意味は維持できる。追加情報として、出力時の実ピーク、飽和したサンプル数、量子化前後の差分を集計すると、`--noclip`やリミッター使用時の判断材料になる。

半フルスケールを超えるたびにtanhを適用する現在のリミッターは波形を変形する。ReplayGainによるレベル調整と、波形を変える処理の結果をそれぞれ確認できる表示が望ましい。

## 保守性と性能の改善提案

- 入力コンテナ、PCMの形式、出力コンテナをそれぞれ表現する。現在の`format`と`samplesize`の組み合わせによる暗黙の前提を減らす。
- 音声解析とディザのグローバル状態をコンテキスト構造体へ移す。将来の複数ファイル並列化やライブラリ利用に先立って、状態の所有者を明確にする。
- ディレクトリ再帰で`stat()`がシンボリックリンクをたどるため、循環を検出する。POSIXの`open_dir()`のエラー経路には`free(result)`後の参照もあり、併せて修正する。
- `process_directory()`は処理に失敗すると作業ディレクトリを戻さない経路がある。復帰処理を常に実行し、`readdir()`の直前に`errno = 0`を設定して読み取りエラーを判定する。
- `make debug`の警告を解消する。今回のビルドでは、未初期化変数、符号比較、コメントの文字コード、getoptの未使用引数に関する警告が出ており、現状はwarning-cleanではない。
- `debug`、`release`、`asan`の`clean`とビルドが並列makeで競合しないよう依存関係を整理する。`verified-release`の逐次サブmakeが参考になる。
- 性能改善はディザとPCM変換を計測してから進める。丸め、乱数列、フィルター演算順序を変えずに改善できるものを優先し、既存のSIMD一致テストを継続する。

## 修正順序と受け入れ条件

1. R1、R2、R3、R4、R5を修正する。正常なメタデータを含むファイル、短い破損ヘッダー、長いCLI引数でASanとUBSanを通し、失敗時に原本が変わらないことを確認する。
2. R6、R10を修正し、作成、読み取り、書き込み、seek、最終化、置換の各失敗で原本保存を検証する。Windowsは実機テストを含める。
3. R7、R8、R9、R11を修正する。奇数長チャンク、gainの異常値、PCMの負の端点、部分失敗の終了コードを回帰テストに追加する。
4. ディザのスペクトル測定と結果表示を整備した上で、音響面の機能拡張と高速化を検討する。

各修正では`make check`を必須とし、コンテナ処理やPCMに関わる変更では`make check-interop`も実行する。ASanだけでは負の整数のシフトを検出できないため、UBSanを含む検証を追加する。正常入力のテストに加え、チャンク列を対象とするファジングとI/O失敗の注入が有効である。

## 再現用入力の最小構成

以下はR1、R2、R3、R7の入力を作るPython 3標準ライブラリのみの例である。一時ディレクトリ内で実行する。R1は入力ファイル自体が破損した出力で置換されるため、保存が必要な音声を使わない。

```python
import math
from pathlib import Path
import struct

def riff_chunk(tag, payload):
    return (tag + struct.pack('<I', len(payload)) + payload
            + (b'\0' if len(payload) & 1 else b''))

pcm = [int(3000 * math.sin(i * .2)) for i in range(4410)]
fmt = struct.pack('<HHIIHH', 1, 1, 44100, 88200, 2, 16)
data = struct.pack('<4410h', *pcm)
for name, metadata in (
    ('marker.wav', riff_chunk(b'JUNK', b'fmt ' + struct.pack('<I', 0x10000000))),
    ('odd.wav', riff_chunk(b'JUNK', b'x')),
):
    body = b'WAVE' + metadata + riff_chunk(b'fmt ', fmt) + riff_chunk(b'data', data)
    Path(name).write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)

def aiff_chunk(tag, payload):
    return tag + struct.pack('>I', len(payload)) + payload

comm = struct.pack('>hIh', 1, 4410, 16) + bytes.fromhex('400eac44000000000000')
body = (b'AIFF' + aiff_chunk(b'COMM', comm)
        + aiff_chunk(b'SSND', bytes(8) + struct.pack('>4410h', *pcm)))
Path('input.aif').write_bytes(b'FORM' + struct.pack('>I', len(body)) + body)
Path('large-comm.aif').write_bytes(
    b'FORM' + struct.pack('>I', 0xffffffff) + b'AIFFCOMM' + struct.pack('>I', 0x40000000))
```

```sh
# R1 出力の破損を確認
/tmp/wavegain-review-san -y marker.wav
ffprobe -v error marker.wav

# R2 NULL参照を確認
/tmp/wavegain-review-san -y -b 2 input.aif

# R3 スタック超過を確認
/tmp/wavegain-review-san -c large-comm.aif

# R7 有効なチャンク列の拒否を確認
/tmp/wavegain-review-san -c odd.wav
```
