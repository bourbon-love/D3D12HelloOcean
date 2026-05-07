# D3D12HelloOcean — 技術仕様書

DirectX 12 ベースのリアルタイム海洋レンダラーの技術仕様。

---

## 目次

1. [プロジェクト概要](#1-プロジェクト概要)
2. [アーキテクチャ](#2-アーキテクチャ)
3. [サブシステム詳細](#3-サブシステム詳細)
4. [レンダリングパイプライン](#4-レンダリングパイプライン)
5. [後処理パイプライン](#5-後処理パイプライン)
6. [シェーダー仕様](#6-シェーダー仕様)
7. [定数バッファレイアウト](#7-定数バッファレイアウト)
8. [ビルド方法](#8-ビルド方法)

---

## 1. プロジェクト概要

| 項目 | 内容 |
|------|------|
| エンジン | DirectX 12 (Agility SDK 618) |
| 言語標準 | C++20 |
| プラットフォーム | Windows 10/11 x64 |
| ターゲット解像度 | 1280×720（可変） |
| フレームバッファ形式 | R16G16B16A16_FLOAT (HDR) → R8G8B8A8_UNORM (LDR) |

---

## 2. アーキテクチャ

### 2.1 クラス構成

```
Win32Application
  └── D3D12HelloTriangle          (アプリ層：デバイス・交換チェーン・入力・ImGUI)
        ├── Renderer               (海洋メッシュ・カメラ)
        ├── SkyDome                (天球・太陽・月・雲)
        ├── OceanFFT               (GPU FFT 海洋高さマップ)
        ├── WeatherSystem          (天気状態機)
        ├── RainSystem             (雨粒パーティクル・水面波紋)
        ├── FloatingObject         (浮遊箱・水中箱)
        ├── FishSchool             (鱼群 Boids シミュレーション)
        └── PostProcessPipeline    (全後処理効果)
```

### 2.2 フレームループ

```
OnUpdate()
  ├── SkyDome::Update()          太陽・月位置更新、雲パラメータ更新
  ├── Renderer::Update()         カメラ・時間更新
  ├── WeatherSystem::Update()    天気状態遷移・パラメータ補間
  ├── RainSystem::Update()       雨粒・波紋シミュレーション
  ├── FloatingObject::Update()   浮力シミュレーション
  ├── FishSchool::Update()       Boids 群れシミュレーション
  └── ImGUI フレーム構築

OnRender()
  ├── OceanFFT::Dispatch()       GPU コンピュート：FFT 高さマップ生成
  ├── PostProcessPipeline::RenderShadowMap()   影マップパス
  ├── [シーンパス → HDR RT]
  │     ├── SkyDome::Render()    天球
  │     ├── SSR スカイスナップショット取得
  │     ├── FloatingObject::RenderUnderwater()  水下物体
  │     ├── FishSchool::Render()  鱼群
  │     ├── SSR 屈折スナップショット取得
  │     ├── Renderer::Render()    海洋メッシュ
  │     ├── Renderer::RenderWaterBox()  水体境界ボックス
  │     ├── FloatingObject::Render()    水面浮遊箱
  │     ├── RainSystem::Render()  雨粒・波紋
  │     ├── PostProcessPipeline::RenderLightning()  稲妻オーバーレイ
  │     └── PostProcessPipeline::RenderSSAO()       SSAO
  └── PostProcessPipeline::RenderPostProcess()
        ├── TAA
        ├── Bloom
        ├── God Rays
        ├── DOF
        ├── ToneMap + 水中エフェクト
        └── LensFlare
```

---

## 3. サブシステム詳細

### 3.1 OceanFFT

Phillips スペクトルに基づく GPU FFT 海洋シミュレーション。

| パス | シェーダー | 説明 |
|------|-----------|------|
| 初期化 | `PhillipsCS.hlsl` | Phillips スペクトル H₀(k) を生成（起動時1回のみ） |
| 時間発展 | `TimeEvolutionCS.hlsl` | 分散関係 ω(k)=√(g|k|) を使い H(k,t) を演算 |
| IFFT | `IFFTCS.hlsl` | Cooley-Tukey Radix-2 IFFT（X/Y 軸 各2回、ピンポンバッファ） |

**出力テクスチャ**

| テクスチャ | 形式 | 内容 |
|-----------|------|------|
| `m_heightMap` | R32G32B32A32_FLOAT | .x = 高さ変位、.z = X 水平変位 |
| `m_dztMap`    | R32G32B32A32_FLOAT | Z 水平変位 |

### 3.2 Renderer（海洋メッシュ）

- グリッドサイズ：512×512 頂点、400×400 世界単位
- 変位：FFT 高さマップ + 4 ウェーブ Gerstner 波の合成
- 法線：有限差分による動的計算
- カメラ：WASD + マウス操作 / 自動展示環状モード

**展示モードカメラ軌道**

```
位置Y = height + 8·sin(angle·0.35) + 2.5·sin(angle·1.10) + 0.8·sin(angle·2.80)
```

3 周波数の重ね合わせにより自然な海洋上下運動を再現。範囲は約 -6.3m〜+16.3m。

### 3.3 SkyDome

- 天球半径：遠クリップ面（カメラ内側にレンダリング）
- 太陽・月：方向ベクトルの日周運動シミュレーション
- 雲：3D Perlin/FBM ノイズによる手続き型生成
- **水平線カリング**：`sunVis = smoothstep(-0.04, 0.06, sunPosition.y)` により太陽・月が水平線以下では完全消去（カメラ位置に依存しない）

### 3.4 WeatherSystem

| 状態 | 説明 |
|------|------|
| Calm（平静） | 波高小、雲量少 |
| Windy（風） | 波高中、雲量中、軽雨 |
| Storm（嵐） | 波高大、雲量大、豪雨、稲妻 |

状態遷移はすべてパラメータの線形補間（移行時間：3〜60 秒）。

### 3.5 RainSystem

- 雨粒：最大 2000 個のビルボードパーティクル
- 波紋：最大 200 個（着水エフェクト）
- 風向きに連動した降下方向

### 3.6 FishSchool

CPU Boids アルゴリズム + GPU StructuredBuffer インスタンシング。

**Boids パラメータ**

| パラメータ | 値 | 説明 |
|-----------|-----|------|
| SEP_R | 2.5 m | 分離半径 |
| ALN_R | 8.0 m | 整列半径 |
| COH_R | 15.0 m | 結束半径 |
| ZONE_R | 70.0 m | 活動領域半径 |
| DMIN / DMAX | -18 / -5 m | 深度制限（ハード上限クランプ付き） |
| MINSPD / MAXSPD | 2.5 / 7.0 m/s | 速度制限 |

魚形状：6 頂点ダイヤモンド型（全長 3.5 m 相当）。  
描画：Beer-Lambert 吸収（RGB 各係数）＋ 環境光 0.35 ＋ ディフューズ＋リム。

### 3.7 FloatingObject

- **水面浮遊箱**：FFT 高さマップをサンプリングして波に乗る
- **水中箱**：XZ 座標固定 + 負の `dropOffset` で任意深度に配置
- **影レンダリング**：深度専用パス（front-face カリング、傾き深度バイアス）

---

## 4. レンダリングパイプライン

### 4.1 ディスクリプタヒープ構成

**RTV ヒープ（合計 11 スロット）**

| スロット | リソース |
|---------|---------|
| 0–1 | スワップチェーンバックバッファ（FrameCount=2） |
| 2 | bloomExtractRT |
| 3 | bloomBlurRT |
| 4 | hdrRT（HDR メインレンダーターゲット） |
| 5 | godRayRT（半解像度） |
| 6 | dofRT |
| 7 | taaRT |
| 8 | taaHistoryRT |
| 9 | ssaoRT（半解像度） |
| 10 | ssaoBlurRT（半解像度） |

**oceanSRVHeap（海洋シェーダー入力）**

| スロット | リソース |
|---------|---------|
| t0 | heightMap（FFT 高さ・X 変位） |
| t1 | dztMap（FFT Z 変位） |
| t2 | skySnapshotRT（SSR 天球反射） |
| t3 | shadowMap（2048×2048 深度） |
| t4 | refractionRT（水面透過・屈折） |

### 4.2 ルートシグネチャ（シーン共通）

| スロット | 内容 |
|---------|------|
| b0 | SceneCB（ビュー/プロジェクション行列、光源情報、Gerstner 波パラメータ） |
| t0–t4 | oceanSRVHeap（上記参照） |
| b1 | RippleCB（波紋定数バッファ） |
| b2 | ShadowSceneCB（影行列、水体パラメータ） |

### 4.3 影マップ

| 項目 | 値 |
|------|-----|
| 解像度 | 2048×2048 |
| 投影 | 正射影（200m×200m×400m） |
| 深度バイアス | 8000（固定）+ 3.0（傾き） |
| カリング | フロントフェース（ピーターパン防止） |
| 有効条件 | 太陽仰角 > 5° |

---

## 5. 後処理パイプライン

`PostProcessPipeline` クラスが一括管理。全パスの実行順序：

```
hdrRT (RT) → PSR 遷移
    ↓
TAA（テンポラル アンチエイリアシング）
    ↓
Bloom（輝度抽出 → H/V ブラー × 2 反復）
    ↓
God Rays（放射状ブラー、半解像度）
    ↓
DOF（被写界深度、深度バッファ参照）
    ↓
ToneMap（Reinhard + 水中エフェクト → スワップチェーン書き込み）
    ↓
LensFlare（加算ブレンド）
    ↓
ImGUI オーバーレイ
```

### 5.1 TAA

- ジッター：Halton(2,3) 列、16 フレームループ
- NDC オフセット：プロジェクション行列 row[2] を修正
- 履歴ブレンド：デフォルト 0.9（初回フレームは 0 でゴースト防止）

### 5.2 Bloom

- 輝度抽出閾値：デフォルト 1.0
- ブラー：水平パス → 垂直パス × 2 反復（ピンポンバッファ）
- 合成強度：デフォルト 0.8

### 5.3 God Rays

- 太陽スクリーン座標へ投影 → 放射状ブラー（64 ステップ）
- 条件：太陽仰角 > -4°、スクリーン内視認度 > 0

### 5.4 SSAO

- カーネル：半球 8 サンプル（事前計算済み）
- 出力：半解像度 R16_FLOAT → ボックスブラー
- 深度バッファ参照：DEPTH_WRITE ↔ PSR 遷移を手動管理

### 5.5 水中エフェクト（ToneMap 内）

カメラ Y 座標に基づき滑らかにブレンド（-2m で完全水中）。

| エフェクト | 実装 |
|-----------|------|
| 色吸収 | Beer-Lambert 法（R:0.18 / G:0.05 / B:0.02） |
| フォグ | 深度比例（色：0.01, 0.07, 0.18） |
| コースティクス | 重ね合わせ正弦波 × pow4 × 深度フェード |
| UV 歪み | スクリーンスペース sin/cos ワープ |
| ビネット強化 | uwBlend 比例 |

---

## 6. シェーダー仕様

| ファイル | タイプ | 用途 |
|---------|-------|------|
| `shaders.hlsl` | VS + PS | 海洋サーフェス（FFT + Gerstner + Phong + Fresnel） |
| `skyShaders.hlsl` | VS + PS | 天球（Perlin 雲・太陽・月・水中空置換） |
| `fish.hlsl` | VS + PS | 鱼群インスタンシング（尾ひれ揺れ・Beer-Lambert） |
| `tonemapping.hlsl` | VS + PS | トーンマッピング + 全水中エフェクト |
| `bloom_*.hlsl` | VS + PS | 輝度抽出・ガウスブラー |
| `godrays_*.hlsl` | VS + PS | 放射状ブラー |
| `lensflare_*.hlsl` | VS + PS | レンズフレア（加算ブレンド） |
| `dof_*.hlsl` | VS + PS | 被写界深度（CoC ベース） |
| `taa_*.hlsl` | VS + PS | テンポラル アンチエイリアシング |
| `ssao_*.hlsl` | VS + PS | SSAO + ボックスブラー |
| `shadowmap_*.hlsl` | VS | 深度専用シャドウパス |
| `lightning_*.hlsl` | VS + PS | 稲妻ポリライン（加算ブレンド） |
| `rain.hlsl` | VS + PS | 雨粒ビルボード |
| `waterbody.hlsl` | VS + PS | 水体境界ボックス（半透明） |
| `PhillipsCS.hlsl` | CS | Phillips スペクトル生成 |
| `TimeEvolutionCS.hlsl` | CS | 周波数領域時間発展 |
| `IFFTCS.hlsl` | CS | Cooley-Tukey Radix-2 IFFT |

---

## 7. 定数バッファレイアウト

### SceneCB（Renderer、毎フレーム更新）

```hlsl
cbuffer SceneCB : register(b0) {
    float4x4 view;           // ビュー行列
    float4x4 proj;           // プロジェクション行列
    float3   cameraPos;      // カメラ位置（ワールド空間）
    float    time;           // 経過時間（秒）
    float3   sunDir;         // 太陽方向ベクトル（正規化）
    float    sunIntensity;   // 太陽強度
    float3   sunColor;       // 太陽色
    float3   skyColor;       // 空色
    // Gerstner 波パラメータ × 4
    WaveParam waves[4];      // direction, amplitude, wavelength, speed
    // フォグ・その他
};
```

### SkyCB（SkyDome、256 バイトアライン）

```hlsl
cbuffer SkyCB : register(b0) {
    float4x4 viewProj;
    float3   topColor;
    float3   middleColor;
    float3   bottomColor;
    float3   sunPosition;    // 正規化方向
    float3   moonPosition;
    float    time;
    float    cloudDensity;
    float    cloudScale;
    float    cloudSharpness;
    float    weatherIntensity;
    float    cameraY;        // 水中エフェクト判定用
};
```

### ShadowSceneCB（PostProcessPipeline、256 バイトアライン）

```hlsl
cbuffer ShadowSceneCB : register(b2) {
    float4x4 lightViewProj;  // 光源ビュープロジェクション行列
    float    shadowBias;
    float    shadowStrength;
    float    shadowEnabled;
    float    screenW, screenH;
    float    waterBodyStr;   // 水体色強度
    float    waterRefract;   // 屈折 UV 歪み強度
    float    waterMinTrans;  // 最小透過率
};
```

---

## 8. ビルド方法

### 必要環境

- Visual Studio 2022（プラットフォームツールセット v143）
- Windows SDK 10.0 以上
- DirectX 12 Agility SDK（NuGet パッケージ自動取得）

### コマンドラインビルド

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" `
  D3D12HelloWorld.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal
```

### 実行

```powershell
Start-Process "HelloTriangle\bin\x64\Debug\D3D12HelloOcean.exe"
```

### 操作方法

| キー / 操作 | 機能 |
|------------|------|
| WASD | カメラ移動 |
| マウスドラッグ | カメラ回転 |
| Tab | ワイヤーフレーム切替 |
| V | 展示モード切替 |
| 1 / 2 / 3 | 天気（平静 / 風 / 嵐）切替 |
| 4 | 天気自動モード |
| ImGUI パネル | 全パラメータのリアルタイム調整 |

---

*生成日：2026-05-06*
