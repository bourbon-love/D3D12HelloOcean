# DirectX 12 リアルタイム海洋レンダリングエンジン — 技術説明書

---

## プロジェクト概要

| 項目 | 内容 |
|------|------|
| **プロジェクト名** | D3D12 Ocean Renderer |
| **種別** | 個人開発・レンダリングエンジン研究 |
| **開発環境** | Visual Studio 2022、C++20、Windows 11 |
| **グラフィックスAPI** | DirectX 12（D3D12 Agility SDK 1.618） |
| **シェーダー言語** | HLSL（Shader Model 6.0 / DXC コンパイラ） |
| **対象プラットフォーム** | Windows 10/11（x64） |

DirectX 12 の低レベル GPU 制御機能を活用し、ゼロから設計・実装したリアルタイム海洋レンダリングエンジンです。GPU コンピュートシェーダーによる FFT 海洋物理シミュレーション、マルチパス HDR ポストプロセスパイプライン、手続き的大気システムを統合しています。ゲームエンジンのレンダリングサブシステムに相当する機能を独力で実装した実績です。

---

## エンジンアーキテクチャ設計

### モジュール構成

```
D3D12HelloTriangle（エンジンコア・フレームループ管理）
├── Renderer          シーン描画・定数バッファ・深度バッファ・カメラ
├── OceanFFT          GPUコンピュートパイプライン（FFT海洋シミュレーション）
├── SkyDome           大気レンダリングサブシステム
├── WeatherSystem     ゲームプレイ連動パラメータ管理（状態マシン）
├── RainSystem        GPUパーティクルシステム（雨・波紋）
└── Camera            入力処理・ビュー行列管理
```

各サブシステムはインターフェースを介して疎結合に接続されており、WeatherSystem が OceanFFT・SkyDome・RainSystem に対してパラメータをプッシュする一方向データフローを採用しています。

### フレームグラフ（パス依存関係）

```
[Compute]  OceanFFT::Dispatch      UAV書込 → バリア → SRV変換
     │
[Scene]    SkyDome::Render    ─→  HDR RT（R16G16B16A16F）
     │
     ├─[Copy] CopyResource   ─→  SkySnapshot RT（SSR用）
     │
[Scene]    Renderer::Render   ─→  HDR RT（FFTテクスチャt0/t1、SSRテクスチャt2）
[Scene]    RainSystem::Render ─→  HDR RT
     │
[Post1]   RenderBloom   [HDR→Extract→Blur×2→BlurRT]
[Post2]   RenderGodRays [HDR→GodRayRT（ハーフ解像度）]
[Post3]   RenderDOF     [HDR＋Depth→DOFRT]
[Post4]   RenderToneMap [DOFRT＋BlurRT＋GodRayRT→SwapChain]
[Post5]   RenderLensFlare [SwapChainへ加算合成]
[UI]      ImGui
```

### リソース管理方針

| リソース種別 | 管理方式 |
|---|---|
| 定数バッファ（Scene CB） | UPLOAD ヒープ永続マップ、1024スロット環形バッファ |
| テクスチャ（高さマップ等） | DEFAULT ヒープ、UAV/SRV 兼用（Typeless フォーマット活用） |
| ポストプロセス RT | DEFAULT ヒープ、機能別 R16G16B16A16F RT |
| 描画コマンド | 単一コマンドアロケータ＋フェンス同期（シングルスレッド） |

---

## GPU パイプライン詳細

### 1. FFT 海洋物理シミュレーション（コンピュートパイプライン）

**技術的背景：** 現代のゲームエンジン（Assassin's Creed IV、Sea of Thieves 等）で採用されている FFT ベースの海洋シミュレーション手法を DX12 で独自実装しました。

**パイプライン構成（3段）：**

```
[PhillipsCS]  N=256グリッドのパワースペクトル初期化（起動時1回）
     ↓
[TimeEvolutionCS]  分散関係 ω(k)=√(g|k|) による周波数域の毎フレーム更新
     ↓
[IFFTCS]  Cooley-Tukey Radix-2 IFFT（水平→垂直、ピンポンバッファ使用）
     ↓
出力：heightMap(.x=高さ, .z=Dx変位)、dztMap(.x=Dz変位)
```

**DX12 実装の要点：**
- コンピュート専用ルートシグネチャとUAVディスクリプタヒープを設計・分離
- `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` ↔ `NON_PIXEL_SHADER_RESOURCE` バリアをフレーム境界で正確に管理
- ピンポンバッファにより追加メモリ確保なしで IFFT の in-place 演算を実現

---

### 2. 海洋表面レンダリング（グラフィクスパイプライン）

512×512 グリッド（26万三角形以上）を Gerstner ウェーブ（4 波重畳）と FFT 変位で変形します。

**ヤコビアン泡沫生成（エンジン独自機能）：**
```hlsl
// 変位フィールドの偏微分から波頭の折り重なりを検出
float J = (1 + dDxdx) * (1 + dDzdz) - dDxdz * dDzdx;
float foam = pow(saturate(1.0 - J), sharpness) * foamIntensity;
// 2層のvalue noiseで有機的な泡沫形状を生成
foam *= (valueNoise(uv*60+drift) * 0.65 + valueNoise(uv*168+drift) * 0.35);
```

**スクリーンスペースリフレクション（SSR）：**
- 天空描画後に `CopyResource` で「天空スナップショット」を取得
- 海洋PSで反射ベクトルをビュープロジェクション行列でスクリーンUVに投影しサンプリング
- FFT SRV・SSR SRV を統合した専用ヒープ設計でバインド回数を最小化

---

### 3. HDR ポストプロセスパイプライン設計

すべてのシーン描画を `R16G16B16A16_FLOAT` オフスクリーン RT に対して行い、最終的に ACES トーンマップで LDR へ変換するパイプラインを設計しました。

#### パス別設計

| パス | 入力 | 出力 | 技術 |
|---|---|---|---|
| **Bloom 輝度抽出** | HDR RT | ExtractRT | 輝度閾値比較 |
| **Bloom ブラー×2** | ExtractRT | BlurRT | 9タップ分離ガウス、ピンポン |
| **体積光（God Rays）** | HDR RT | GodRayRT（½解像度） | 64サンプル放射状ブラー |
| **被写界深度** | HDR RT ＋ DepthSRV | DOFRT | Vogelディスク20サンプル |
| **ACES トーンマッピング** | DOFRT ＋ BlurRT ＋ GodRayRT | SwapChain | ACESフィルミック曲線 |
| **レンズフレア** | − | SwapChain（加算合成） | 完全手続き的、テクスチャ不使用 |

#### 自動露出システム
太陽高度から目標露出値を計算し、非対称速度（明→暗: 1.0/s、暗→明: 0.35/s）でスムーズな人眼順応をシミュレート。GPU 測光なしに CPU 側のみで実現。

---

### 4. 大気レンダリングサブシステム

**積雲レンダリング：** Worley ノイズ + FBM を 3 層合成し、天気強度・日没色・稲妻照明を動的に反映。

**巻雲（Cirrus）：** 非対称 FBM（繊維状テクスチャ）を高高度 UV 投影に適用した薄い筋雲。

**風向き連動ドリフト：**
```hlsl
// WeatherSystemの風向きをCPUから毎フレーム渡す
float3 movement = float3(cloudDriftX * time, 0.0, cloudDriftY * time);
// 嵐時に2.5倍速でドリフト
```

---

### 5. エンジンレベルの DX12 実装

#### ルートシグネチャ設計

機能ごとに独立したルートシグネチャを設計し、不要なバインドを排除しました。

| ルートシグネチャ | パラメータ構成 |
|---|---|
| Scene（海洋・天空共用） | CBV[b0] ＋ SRV table(t0/t1/t2) ＋ CBV[b1] |
| Bloom | SRV table(t0) ＋ 4 Root Constants |
| ToneMap | SRV table(t0/t1) ＋ SRV table(t2) ＋ 8 Root Constants |
| GodRay | SRV table(t0) ＋ 6 Root Constants |
| DOF | SRV table(t0/t1) ＋ 4 Root Constants |
| LensFlare | 6 Root Constants（テクスチャ不使用） |

#### ディスクリプタヒープ管理

DX12 の「同時バインド可能なCBV/SRV/UAVヒープは1つ」という制約を考慮し、パス間でのヒープ切り替えを最小化するために以下の統合設計を採用しました。

- **m_oceanSRVHeap**（3スロット）：FFT heightMap + dztMap + SkySnapshot → 1回のバインドで海洋パスのすべての SRV をカバー
- **m_bloomSRVHeap**（4スロット）：HDR RT + BloomExtract + BloomBlur + GodRay → ToneMap パスで利用
- **m_dofSRVHeap**（2スロット）：HDR RT + DepthBuffer → DOF パス専用

#### リソースバリア管理

各フレームで発生するバリア遷移を列挙し、不要な GPU ストールを回避しました。

```
HDR RT:  RT → [Bloom] COPY_SOURCE → RT → PSR → [ToneMap] RT
GodRay:  RT → [Clear] RT → [Draw] PSR → [ToneMap] RT
Depth:   DEPTH_WRITE → [DOF] PSR → DEPTH_WRITE
SkySnap: COPY_DEST ↔ PSR（毎フレーム）
```

---

## 技術的課題と解決策

### 課題1：ディスクリプタヒープの同時バインド制約

SSR 実装時、FFT テクスチャ（既存ヒープ）と天空スナップショット（新規テクスチャ）を同一パスで参照する必要があったが、DX12 では CBV/SRV/UAV ヒープは同時に 1 つしかバインドできない。

**解決策：** 両者のSRVを収容する統合ヒープ（m_oceanSRVHeap）を新設。同一リソースに複数のSRVを作成できる仕様を活用し、既存ヒープへの変更なしに対応。

### 課題2：深度バッファの Typeless 化

DOF パス実装時、深度バッファをシェーダーリソースとして読み取る必要があったが、`D32_FLOAT` リソースは SRV 作成不可。

**解決策：** リソースフォーマットを `R32_TYPELESS` に変更し、DSV は `D32_FLOAT` ビュー、SRV は `R32_FLOAT` ビューとして同一リソースから別々に作成。clearValue は DSV フォーマットに合わせて設定。

### 課題3：HDR ブルームの不可視問題

LDR（R8G8B8A8_UNORM）パイプラインでは 1.0 クランプにより太陽や稲妻のブルームが不可視。

**解決策：** オフスクリーン RT を R16G16B16A16_FLOAT に変更し HDR パイプライン化。太陽・月・稲妻の HDR 値（1.0超）が ACES トーンマップを通じてブルームとして視覚化される設計に。

### 課題4：ポストプロセスの RT 状態管理の複雑化

複数のポストプロセスパスが同一RT を入出力として共有するため、RT/PSR/COPY_SOURCE/DEST 状態遷移が複雑化しフレーム間のバリア漏れによるGPU バリデーションエラーが発生。

**解決策：** 各パスの開始・終了時の「期待状態」を仕様として明文化し、パスをまたぐ状態遷移をパス側の責任として一元管理。`RenderToneMap()` にクリーンアップバリアをまとめることで次フレームの初期状態を保証。

---

## 実装規模

| 指標 | 数値 |
|------|------|
| C++ ファイル | 約 12 ファイル / 約 3,000 行 |
| HLSL シェーダーファイル | 12 ファイル / 20+ エントリポイント / 約 1,500 行 |
| レンダーパス数 | 9 パス（Scene×3、Bloom×4、GodRay、DOF、ToneMap、LensFlare） |
| レンダーターゲット数 | 9 RT（SwapChain×2、Bloom×2、HDR、GodRay、DOF、SkySnapshot） |
| 定数バッファ | 6 種（SceneCB・SkyCB・RippleCB・RainCB・各ポストCB） |
| グリッド解像度 | 512×512（約 52 万インデックス） |
| FFT 解像度 | 256×256 |
| ターゲットフレームレート | 60 FPS（1080p、GeForce RTX 系） |

---

## アピールポイント

1. **DX12 の深い実装経験** — ルートシグネチャ・ディスクリプタヒープ・リソースバリア・フェンス同期をすべて手書きで実装し、抽象化レイヤーに依存しない低レベル GPU 制御を習得

2. **レンダリングパイプラインの設計力** — 9 パスのフレームグラフを依存関係を考慮しながら設計し、パス間のリソース状態遷移を体系的に管理

3. **GPU コンピュートプログラミング** — FFT・物理ベースシミュレーションをコンピュートシェーダーで実装し、UAV・ピンポンバッファ・ディスパッチ管理を実践

4. **エンジン設計パターンの実践** — サブシステム分離・一方向データフロー・定数バッファの環形管理など、実際のゲームエンジンで採用される設計手法を独自実装

5. **拡張可能なアーキテクチャ** — 各サブシステムが独立したルートシグネチャと PSO を持ち、新機能追加時のパイプライン変更を局所化できる構造

---

*本ドキュメントに記載の機能はすべて実装・動作検証済みです。*
