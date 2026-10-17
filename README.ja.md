# WaveGain v1.5.0

[English](README) | 日本語

WaveGainは、モノラル・ステレオの音声ファイルのラウドネスを測定・調整するツールです。ReplayGainとBS.1770の統合ラウドネス（LUFS）に対応し、True Peakによるピーク制限も利用できます。ゲインは音声サンプルに直接適用します。

既定ではゲインを計算・表示するだけです。`-y`で入力ファイルに適用し、`--output`または`--output-dir`を併用すると原本を保持できます。ゲインの適用と取り消しは非可逆処理です。

## 主な機能

- ReplayGainまたは統合LUFSによる、曲単位・アルバム単位の音量調整
- True Peakの測定と出力上限の指定
- 原本を保持する別ファイル出力と、ディレクトリの再帰処理
- WAV、RF64、Sony Wave64への対応と、16-bit AIFF/AIFCの限定的なサポート
- 8〜32-bit整数PCMおよび浮動小数点音声、8〜192 kHzの14種類のサンプルレートへの対応
- 従来のディザと、サンプルレートに応じて係数を変えるディザの選択

## 使い方

ビルド後、リポジトリのルートで実行してください。PATHの通った場所へインストールした場合は、`./wavegain`を`wavegain`に読み替えます。

入力を変更せずにReplayGainを計算します。

```sh
./wavegain input.wav
```

原本を保持し、ReplayGainを適用した別ファイルを作成します。

```sh
./wavegain -y --output normalized.wav input.wav
```

目標を-23 LUFS、True Peakの上限を-1 dBTPに指定して調整します。

```sh
./wavegain -y --target-lufs -23 --peak-ceiling -1 --output out.wav in.wav
```

曲間の相対音量を保ち、アルバム全体に共通のゲインを適用します。

```sh
./wavegain -y -a --output-dir normalized track1.wav track2.wav
```

ディレクトリを再帰的に処理し、各フォルダーを1枚のアルバムとして扱います。

```sh
./wavegain -y -a -z --output-dir ../normalized ./album
```

24-bit整数PCMへ変換し、サンプルレート対応の中程度のノイズシェーピングを使用します。

```sh
./wavegain -y -b 3 -d 3 --dither-profile rate --output out.wav in.wav
```

適用したゲインを記録し、後で取り消します。この例は入力ファイルを書き換えます。取り消しても元の音声とビット単位では一致しません。

```sh
./wavegain -y -w recording.w64
./wavegain --undo-gain recording.w64
```

別ファイル出力では、既存の出力先を上書きしません。`--output`は明示的に指定した入力ファイル1個を対象とし、`--output-dir`は複数ファイルに対応します。再帰処理では相対ディレクトリ構造を保持します。出力ディレクトリは入力ディレクトリの外側に指定してください。

POSIXでは入力の通常のアクセス権を引き継ぎます。所有者・グループや特殊なモードビットはコピーしません。上書き禁止renameとハードリンクがどちらも使えない場合（一部のexFAT・SMB実装など）は、出力先を排他的に新規作成してコピーします。この経路ではコピー中のファイルが見える場合があり、一時ファイルと出力の両方の空き容量が必要です。失敗時は、不完全な出力と完成済みの一時ファイルを保持して通知します。

## ラウドネス・ピーク・ディザ

### ReplayGainとLUFS

既定の方式はReplayGainです。最大サンプル値だけで正規化するのではなく、重み付けフィルターと音声のエネルギーから知覚上の音量を推定します。トラックモードは各ファイルを個別に調整し、アルバムモードは共通のゲインを適用して曲間の相対音量を保ちます。

`--lufs`を指定すると、BS.1770の統合ラウドネスを使い、既定の目標は-23 LUFSになります。`--target-lufs`は目標値を変更すると同時にLUFSモードを選択します。アルバムのLUFSは各曲のLUFSを平均するのではなく、各曲のブロックエネルギーに共通のゲートを適用して求めます。

LUFSの測定には400 ms以上の音声が必要です。無音や絶対ゲートを下回る音声には、指定した手動ゲインだけを適用します。正確なブロックエネルギー履歴を保持するため、履歴のメモリ使用量は音源の長さに応じて増加します。

### True PeakとDCオフセット

`--true-peak`はTrue Peak測定を有効にし、既定で-1 dBTPを上限にします。`--peak-ceiling`は上限値を変更し、True Peak測定も有効にします。指定したTrue Peak上限は、`-n`（`--noclip`）でも有効です。

量子化後の出力も再測定し、上限を超えた場合は出力ファイルの公開・置換を行いません。ピーク制限によって目標LUFSに届かない場合があります。`--stdout`では送信済みの音声を取り消せないため、最終検査の失敗は非ゼロの終了コードで通知します。

DCオフセット補正は既定でトラック単位です。`-q`でアルバム単位に変更し、`-p`で無効にします。`-p`の場合、LUFSとTrue Peakは最初の解析読み込みで同時に測定します。DC補正後の値を測定する場合は、追加の読み込みが必要です。

### ハイレゾ入力とディザ

176.4/192 kHz入力のReplayGain解析では、ローパスFIRと2:1の間引きを使用します。LUFSとTrue Peakは元のサンプルレートで測定します。出力のサンプルレートとフレーム数は維持します。

ディザの既定プロファイルは`legacy`です。`--dither-profile rate`には`-d 1..4`と整数PCM出力が必要で、TPDFノイズと、サンプルレートに応じた一次誤差帰還を使用します。`-d 1`はノイズシェーピングなしです。新しいオプションを選択しない場合、従来の対応レートのReplayGainと従来のディザは元の処理経路を使用します。

### 測定エンジン

MITライセンスのlibebur128を同梱し、R128Netを参考にしたCの最適化、ステレオのブロックエネルギー計算のSIMD化、ピーク測定専用の処理経路を追加しています。libebur128の別途インストールや.NETランタイムは不要です。

実装、ライセンス、性能測定、精度の範囲については[実装・検証レポート](doc/loudness-implementation.md)を参照してください。

## 対応形式と大容量ファイル

WAVとWave64は、モノラル・ステレオの整数PCMと浮動小数点音声に対応します。対応サンプルレートは次の14種類です。

```text
8000, 11025, 12000, 16000, 22050, 24000, 32000,
44100, 48000, 64000, 88200, 96000, 176400, 192000 Hz
```

圧縮コーデックには対応していません。16-bit整数のAIFF/AIFC入力にも対応します。`-b`で出力サンプル形式を選択でき、`-b 6`は16-bit AIFF出力です。AIFFには32-bitのコンテナーサイズ制限が残ります。

### WAV・RF64

- POSIXビルドでは64-bitのファイルオフセットを使用し、4 GiBを超えるファイルを処理できます。
- 4 GiBを超える通常のWAV出力は、RIFF/dataサイズを`0xFFFFFFFF`に設定し、警告を表示します。
- `--rf64`ではJUNKチャンクを予約し、必要になった場合だけ64-bitサイズを持つds64チャンクへ変換します。小さい出力は通常のRIFF/WAVのままです。
- `--rf64`にはシーク可能な出力先が必要です。`--stdout`では無視します。RF64入力にも対応し、書き換え時は同じ出力サイズ規則に従います。
- シーク可能な入力では、サイズが未確定値の場合やdataがEOFを超える場合、ファイル末尾まで読み込みます。標準入力では厳密にサイズを検査します。

### Sony Wave64

- GUIDで入力を判別します。AIFF出力を明示しない限り、ゲインの適用やPCM形式の変更後もWave64を維持します。
- コンテナー・チャンクのサイズとサンプル数は64-bitです。単一の音声データチャンクより前に、形式チャンクが必要です。
- 音声の前後にある未知のチャンクは、メモリ使用量を制限してコピーします。`fact`と`gain`は音声より前に書き直し、古くなった`levl`ピーク情報は削除します。
- シーク可能な入力で未確定・切り詰められたサイズを検出した場合は、警告して読み込みます。標準入力の検査は厳密です。
- WAVからWave64への出力変換オプションはありません。最初の`.w64`ファイルは外部の変換ツールで作成してください。

### ゲイン記録と標準入力

任意の`gain`チャンクには、`--undo-gain`用に適用済みの倍率を記録します。これはWaveGain独自の拡張で、プレーヤー向けのラウドネスタグではありません。Wave64では独自GUID（`e217d848-96be-4ac0-b56a-8b46289c71e0`）と、リトルエンディアンのIEEE倍精度浮動小数点数を使用します。

macOSでは、上流実装の仕様によりWAVの`gain`倍率をビッグエンディアンで保存するため、他のプラットフォームとの間でWAVの取り消し用メタデータに互換性がありません。

計算モードで`-`を指定すると標準入力を読み込みます。LUFS・True Peakの標準入力測定には、さらに`-p`と確定済みのコンテナーサイズが必要です。ゲインの適用にはファイル入力が必要です。

## ビルドとインストール

Cコンパイラーとmakeを用意し、リポジトリのルートで実行します。

```sh
make
```

既定のリリースビルドは、64-bit環境を含むコンパイラーのネイティブアーキテクチャを使用します。PCMの格納には固定幅の32-bit型を使用します。x86で32-bitビルドを明示する場合は`make CFLAGS=-m32`を使用します。Debianの`gcc-multilib`など、追加のヘッダーとライブラリが必要になる場合があります。

既定のインストール先は`/usr/local/bin`です。`prefix`で変更できます。

```sh
sudo make install
# /usr/binへインストールする場合
sudo make install prefix=/usr
```

インストールターゲットはGNU形式の`install`オプションを使用します。macOSでは、ビルドした実行ファイルをPATHの通ったディレクトリへコピーする方法も利用できます。

Windowsでは、MSYS2のMINGW64シェルでMinGW-w64 GCCと同じMakefileを使用するか、`mswindows/`内のプロジェクトファイルを利用できます。MinGWのリリースビルドは`_FORTIFY_SOURCE`用のlibsspを静的リンクし、システムDLLだけに依存します。以前のMinGWビルドはWineで確認されていますが、新しい出力・測定モードのWindows実機検証は未実施です。

`debian/`のパッケージ設定は過去のもので、保守されていません。古いdebuild手順には依存せず、ソースからビルドしてください。

## テストと開発

| コマンド | 内容 |
| --- | --- |
| `make debug` | 追加のコンパイラー警告を有効にしてビルド |
| `make check` | 回帰テスト一式を実行（Python 3が必要） |
| `make check-sanitize` | ASan/UBSanで再ビルドし、テストを実行 |
| `make check-interop` | FFmpegとの比較（ffmpeg/ffprobeが必要） |
| `make verified-release` | ReplayGain・ラウドネスの一致検証付きリリースビルド |
| `make check-loudness` | ラウドネス測定処理をネイティブの参照実装と比較 |

PCM・浮動小数点変換、ゲインと取り消し、メタデータ、不正ファイル、5 GiBのWave64を含むスパース大容量ファイル、出力パス、ラウドネス、True Peak上限、リサンプリング、ディザの信号特性を検証します。未完了の検証項目は[TODO](TODO)に記録しています。

`verified-release`は、同じコンパイラー・フラグで作成したスカラー版とReplayGainのフィルター状態・変換出力を比較し、不一致なら実行ファイルを削除します。`VERIFY_SIMD=auto`が既定で、どちらのバックエンドも許可します。`VERIFY_SIMD=1`はステレオSIMD版、`VERIFY_SIMD=0`はスカラー版を要求します。

CLIヘルプとマニュアルの正本は[doc/help.json](doc/help.json)です。変更後は`make docs`で生成し、`make check-docs`で英語READMEのヘルプ、`help_generated.h`、manページ、HTMLマニュアルの同期を検査します。日本語READMEは手動翻訳のため、オプションや挙動の変更時にはこちらも更新してください。

Clangでは`make verified-release CC=clang RELEASE_OPT=-O3 LTO_FLAGS=-flto=thin`で最適化ビルドを検証できます。GCCのLTOは`LTO_FLAGS=-flto`を指定します。通常ビルドは従来どおり`-O2`です。`verified-release`はラウドネス処理と参照実装の一致も検証します。非有限値の検査と演算の正確性を保つため、fast-mathは使用しないでください。

Windowsでは`python tools/check_windows.py --cc aarch64-w64-mingw32-clang --optimized`でネイティブの回帰試験を実行できます。`--optimized`を省略すると`-O2`、コンパイラーを`x86_64-w64-mingw32-clang`に変えるとx64向けになります。ARM64上でのx64試験はWindowsのエミュレーションを利用します。

旧版との出力比較には`test/pcm_equivalence.py`、速度と出力一致の同時測定には`tools/benchmark_native.py`を使用できます。どちらも`--reference`と`--candidate`で実行ファイルを指定し、ベンチマークでは`--output timings.json`も指定します。Windowsでは`.exe`を含むパスを指定してください。測定条件と結果は[doc/pcm-optimization.txt](doc/pcm-optimization.txt)に記載しています。

## 動作の変更点

### 未リリース

- Windowsの再帰探索では、途中で見つかったジャンクションなどのディレクトリリンクをたどりません。明示指定したリンク先は入力に使えます。出力先は実際の場所で判定し、別名でも入力内にある場所や、場所を確認できない場合は拒否します。
- 整数PCM入力と高サンプルレート用FIRの履歴参照を高速化しました。復号後のサンプル値とフィルターの加算順序を維持します。
- ディザ・リミッターなしの整数ゲイン適用を専用化しました。従来の量子化、DC補正、飽和境界を維持します。
- Windows x64で、gainメタデータ内の極小のbinary64値が正しく書き込まれるようにしました。
- Windowsのディレクトリエラー表示で元のエラー番号を保持し、メッセージ中の`%`やシステムメッセージ取得失敗を安全に処理します。

### バージョン1.5.0

- 別ファイル出力を追加し、原本を保持しながら既存の出力先への上書きを防止します。
- POSIXの別ファイル出力は入力の通常のアクセス権を引き継ぎます。上書き禁止renameやハードリンクに非対応のファイルシステムでは、排他的な新規作成とコピーを使用します。
- ReplayGainの代わりにLUFSを選択でき、アルバムでは共通のゲーティングを使用します。
- True Peak上限は`--noclip`でも有効で、量子化後も検査します。目標LUFSに届かない場合があります。
- 176.4/192 kHz入力と、選択式のサンプルレート対応ディザを追加しました。

### 過去の変更と互換性

- WAVヘッダーは検証済みのチャンク境界で更新します。`fmt `などの文字列を含む不透明なメタデータを保持し、奇数長RIFFチャンクとデータのパディングを扱います。
- 16-bit AIFF/AIFCからWAVへの変換に対応します。未対応の8-bit AIFF/AIFC、不正なヘッダーやgainチャンクは、入力を置換する前に拒否します。
- 空の音声や一括処理の一部失敗を含め、解析・変換失敗は非ゼロで終了します。処理済みとして意図的にスキップしたファイルは成功扱いです。出力確定時の書き込み・コピーを検査し、失敗時は原本を保持します。Windowsでも原本を先に削除せず、一時ファイルは排他的に作成します。
- 極端な浮動小数点値や取り消し時の増幅は、整数化の前に飽和させ、量子化器やノイズシェーピング履歴のオーバーフローを防ぎます。
- クリッピング防止と表示される`Peak`には、選択したDC補正後のピークを使用します。
- どちらかのチャンネルのDCオフセットがフルスケールの1/32768を超えた場合に補正します。以前のしきい値は2/32768でした。
- `-t`（`--limiter`）では、ほかのゲイン変更やDC補正が不要でも全ファイルを書き換えます。
- 88.2 kHzでは、以前の不安定な係数を置き換えたためゲイン値が変化します。従来のSSE2フィルター（`HAVE_SSE2`）を使用するMSVCビルドも、最後のYule係数を無視していた問題の修正により、全レートで変化します。
- ReplayGainフィルターではFMAを無効にしています。AArch64や`-march=x86-64-v3`以降のx86_64など、以前はFMAで計算していたビルドでは丸めが変わり、0.01 dBのヒストグラム境界に近いゲインが1段階動く場合があります（例：48 kHzで-0.44 dBから-0.45 dB）。FMAを使用できないビルドは従来と同じ結果です。同一ビルドのスカラー版とSIMD版はビット単位で一致しますが、RMS集計やlog10まで含めた、全コンパイラー・全アーキテクチャ間の一致を保証するものではありません。

## プロジェクトの経緯と参考資料

John EdwardsによるWaveGainと、Rodrigo Silva（MestreLion）によるDebianパッケージ作成作業を基にしています。過去のソース・バイナリーのアーカイブは次の場所にあります。

- [RareWaresの配布ページ](http://www.rarewares.org/others.php)
- [RareWaresのファイルアーカイブ](http://www.rarewares.org/files/others/)
- [過去のDebianパッケージ](http://www.rarewares.org/debian/packages/unstable/index.php)
- [Linux Mintのアーカイブ](http://packages.linuxmint.com/pool/import/w/wavegain/)
- [MestreLion版の過去のダウンロード](https://github.com/MestreLion/wavegain/downloads)

これらにはWaveGain 1.2.6や1.2.8などの古いWindowsバイナリー・Debian/Linux Mintパッケージが含まれます。現在のビルドを示すものではありません。

ReplayGainの背景資料：

- [Wikipedia：ReplayGain](https://en.wikipedia.org/wiki/ReplayGain)
- [Hydrogenaudio：ReplayGain](https://wiki.hydrogenaudio.org/index.php?title=ReplayGain)
- [Hydrogenaudio：ReplayGain specification](https://wiki.hydrogenaudio.org/index.php?title=ReplayGain_specification)

## ライセンスと著作権

WaveGainの著作権表記はCopyright (c) 2002-2010 John Edwards <john.edwards33@ntlworld.com>（John33）です。ほかの共同開発者・貢献者は各ソースファイルに記載されています。元の配布ライセンスはLGPL-2.1以降で、[COPYING](COPYING)にLGPL-2.1の本文があります。一部ファイルには過去のGPL/GPL2+表記が残り、ヘッダーの整理はTODOに記録しています。

DebianパッケージとREADME・Makefileなどの追加資料の著作権表記はCopyright (c) 2012 Rodrigo Silva（MestreLion）です。Rodrigo Silvaは、自身のパッチを含むCソースコードについて著作権を主張していません。

同梱libebur128とR128Net由来の最適化のMITライセンス表記は[vendor/libebur128/](vendor/libebur128/)に保持しています。queueヘッダーにはBSDライセンス表記を保持しています。

## オプションの参照

全オプションの正式なヘルプは、次のコマンドで表示できます。英語README末尾にも同じヘルプを掲載しています。

```sh
./wavegain --help
```

- [英語README・全オプション](README)
- [HTMLマニュアル](doc/manual/index.html)
- [ヘルプの正本](doc/help.json)

日本語版は上記の使い方・動作説明の翻訳です。CLIメッセージと自動生成のオプションリファレンスは英語です。
