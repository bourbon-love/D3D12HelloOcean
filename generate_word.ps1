# DirectX12 Ocean Renderer — Word ドキュメント生成スクリプト
# 中国語版Word対応版
param([string]$out = "E:\Study\HelloDX12\TechnicalDocument_JP.docx")

$word = New-Object -ComObject Word.Application
$word.Visible = $false
$doc  = $word.Documents.Add()
$sel  = $word.Selection

# ---- ヘルパー関数 ----
function H1 { param($t)
    $sel.Style = $doc.Styles["标题 1"]
    $sel.TypeText($t); $sel.TypeParagraph() }

function H2 { param($t)
    $sel.Style = $doc.Styles["标题 2"]
    $sel.TypeText($t); $sel.TypeParagraph() }

function H3 { param($t)
    $sel.Style = $doc.Styles["标题 3"]
    $sel.TypeText($t); $sel.TypeParagraph() }

function Body { param($t)
    $sel.Style = $doc.Styles["正文"]
    $sel.ParagraphFormat.SpaceAfter = 4
    $sel.TypeText($t); $sel.TypeParagraph() }

function Bullet { param($t)
    $sel.Style = $doc.Styles["列表项目符号"]
    $sel.TypeText($t); $sel.TypeParagraph() }

function Placeholder { param($caption)
    $sel.Style = $doc.Styles["正文"]
    $sel.ParagraphFormat.Alignment   = 1   # 中央寄せ
    $sel.ParagraphFormat.SpaceBefore = 6
    $sel.ParagraphFormat.SpaceAfter  = 6
    $sel.Font.Bold   = $true
    $sel.Font.Color  = 7368816   # グレー
    $sel.Font.Size   = 10
    $b = $sel.ParagraphFormat.Borders
    $b.InsideLineStyle  = 1; $b.OutsideLineStyle = 1
    $sel.TypeText("[ スクリーンショット挿入箇所 ]"); $sel.TypeParagraph()
    $sel.Font.Bold = $false; $sel.Font.Size = 9
    $sel.TypeText($caption); $sel.TypeParagraph()
    # リセット
    $sel.Font.Bold  = $false; $sel.Font.Color = -16777216; $sel.Font.Size = 10.5
    $sel.ParagraphFormat.Alignment   = 0
    $sel.ParagraphFormat.SpaceBefore = 0
    $sel.ParagraphFormat.Borders.OutsideLineStyle = 0 }

function AddTable { param([string[][]]$rows, [int]$c1w = 130, [int]$c2w = 340)
    $nr = $rows.Count
    $rng = $sel.Range
    $tbl = $doc.Tables.Add($rng, $nr, 2)
    try { $tbl.Style = $doc.Styles["普通表格"] } catch {}
    $tbl.Columns(1).SetWidth($c1w, 2)   # 2=wdPreferExactSize
    $tbl.Columns(2).SetWidth($c2w, 2)
    for ($r = 1; $r -le $nr; $r++) {
        $tbl.Cell($r,1).Range.Text = $rows[$r-1][0]
        $tbl.Cell($r,2).Range.Text = $rows[$r-1][1]
        if ($r -eq 1) {
            $tbl.Cell($r,1).Range.Bold = $true
            $tbl.Cell($r,2).Range.Bold = $true
            $tbl.Cell($r,1).Shading.BackgroundPatternColor = 13421772  # 薄いグレー
            $tbl.Cell($r,2).Shading.BackgroundPatternColor = 13421772
        }
    }
    # 表の後ろへ移動
    $sel.Start = $tbl.Range.End
    $sel.TypeParagraph() }

# ==================================================================
# タイトルページ
# ==================================================================
$sel.Style = $doc.Styles["标题"]
$sel.Font.Size = 22
$sel.TypeText("DirectX 12 リアルタイム海洋レンダリングエンジン")
$sel.TypeParagraph()
$sel.Style = $doc.Styles["副标题"]
$sel.TypeText("技術説明書  ―  エンジンプログラマー職 応募用")
$sel.TypeParagraph(); $sel.TypeParagraph()

Placeholder "全景スクリーンショット推奨：日中の海洋シーン（Bloom・God Ray・SSR効果が確認できる画像）"

$sel.InsertBreak(7)   # ページ区切り

# ==================================================================
# 1. プロジェクト概要
# ==================================================================
H1 "1. プロジェクト概要"
Body "DirectX 12 の低レベル GPU 制御機能を活用し、ゼロから設計・実装したリアルタイム海洋レンダリングエンジンです。GPU コンピュートシェーダーによる FFT 海洋物理シミュレーション、マルチパス HDR ポストプロセスパイプライン、手続き的大気システムを統合しています。"

AddTable @(
    @("項目", "内容"),
    @("種別", "個人開発・レンダリングエンジン研究"),
    @("開発環境", "Visual Studio 2022、C++20、Windows 11"),
    @("グラフィックスAPI", "DirectX 12（D3D12 Agility SDK 1.618）"),
    @("シェーダー言語", "HLSL（Shader Model 6.0 / DXC コンパイラ）"),
    @("グリッド解像度", "512 × 512（約 52 万インデックス）"),
    @("FFT 解像度", "256 × 256"),
    @("レンダーパス数", "9 パス（Scene×3、Bloom×4、GodRay、DOF、ToneMap、LensFlare）"),
    @("ターゲット FPS", "60 FPS（1080p、GeForce RTX 系）")
)

# ==================================================================
# 2. エンジンアーキテクチャ
# ==================================================================
H1 "2. エンジンアーキテクチャ設計"

H2 "2-1. モジュール構成"
Body "各サブシステムはインターフェースを介して疎結合に接続されており、WeatherSystem が OceanFFT・SkyDome・RainSystem に対してパラメータをプッシュする一方向データフローを採用しています。"

AddTable @(
    @("モジュール", "担当機能"),
    @("D3D12HelloTriangle", "エンジンコア・フレームループ・DX12 デバイス管理"),
    @("Renderer", "海洋メッシュ描画・シーン CB・カメラ・深度バッファ"),
    @("OceanFFT", "GPU コンピュートパイプライン（Phillips → TimeEvo → IFFT）"),
    @("SkyDome", "大気レンダリング・日周運動・稲妻ステートマシン"),
    @("WeatherSystem", "天気状態マシン・全サブシステムへのパラメータ配信"),
    @("RainSystem", "GPU パーティクル（雨・波紋）"),
    @("Camera", "入力処理・ビュー行列生成・ショーケースモード")
)

H2 "2-2. リソース管理方針"
AddTable @(
    @("リソース種別", "管理方式"),
    @("定数バッファ（Scene CB）", "UPLOAD ヒープ永続マップ、1024 スロット環形バッファ"),
    @("FFT テクスチャ", "DEFAULT ヒープ、UAV/SRV 兼用（R32_TYPELESS 活用）"),
    @("ポストプロセス RT", "DEFAULT ヒープ、R16G16B16A16_FLOAT × 9 RT"),
    @("DescriptorHeap", "機能別分離（oceanSRV / bloomSRV / dofSRV 等）")
)

# ==================================================================
# 3. GPU パイプライン詳細
# ==================================================================
H1 "3. GPU パイプライン詳細"

H2 "3-1. FFT 海洋物理シミュレーション"
Body "Phillips パワースペクトルを初期値として、3 段のコンピュートシェーダーパイプラインを毎フレーム実行します。"
Bullet "PhillipsCS：風速・風向に基づく N=256 パワースペクトル生成（起動時 1 回）"
Bullet "TimeEvolutionCS：分散関係 ω(k)=√(g|k|) を用いた周波数域の毎フレーム更新"
Bullet "IFFTCS：Cooley-Tukey Radix-2 IFFT（水平・垂直軸、ピンポンバッファ使用）"
Bullet "出力：heightMap（.x=高さ, .z=Dx 変位）および dztMap（.x=Dz 変位）"

Placeholder "嵐シーン推奨：高波・ヤコビアン白沫・稲妻の同時表示（ImGui: Weather=Storm）"

H2 "3-2. ヤコビアン泡沫生成"
Body "変位フィールドの偏微分からヤコビアン行列式 J を計算し、J < 1 の波頭折り重なり領域に泡沫を生成します。2 層の value noise で有機的な形状を実現し、天気強度に連動して密度を変化させます。"

H2 "3-3. スクリーンスペースリフレクション（SSR）"
Body "天空描画後に CopyResource で天空スナップショットを取得し、海洋ピクセルシェーダー内で反射ベクトルをスクリーン UV に投影してサンプリングします。スクリーン外は手続き的天空関数にフォールバックし、エッジをスムーズにフェードします。"

Placeholder "SSR 比較推奨：SSR ON / OFF の水面反射の違い（ImGui: SSR Strength 0→1）"

H2 "3-4. HDR ポストプロセスパイプライン"
Body "シーン全体を R16G16B16A16_FLOAT オフスクリーン RT に描画し、以下のパスを順次適用します。"
AddTable @(
    @("パス", "技術概要"),
    @("Bloom 輝度抽出", "閾値超過ピクセル抽出 → ExtractRT"),
    @("Bloom ブラー × 2", "9 タップ分離ガウス、ピンポン合成"),
    @("体積光（God Rays）", "64 サンプル放射状ブラー、½ 解像度 RT"),
    @("被写界深度（DOF）", "Vogel ディスク 20 サンプル + 深度 SRV"),
    @("ACES トーンマッピング", "ACESFilmic + ビネット + フィルムグレイン"),
    @("レンズフレア", "完全手続き的・7 ゴースト・加算合成")
)

Placeholder "夕暮れシーン推奨：大気散乱・God Ray・Lens Flare が確認できる画像"
Placeholder "夜景シーン推奨：星空・月牙・海面への月光 SSR 反射"

H2 "3-5. 大気レンダリングシステム"
Bullet "積雲：Worley + FBM 3 層合成、天気強度・日没色・稲妻照明に動的連動"
Bullet "巻雲（Cirrus）：非対称 FBM による高高度の繊維状薄雲、嵐時は消滅"
Bullet "風向き連動ドリフト：WeatherSystem の windDir を毎フレーム SkyDome に転送"
Bullet "星空・月牙・稲妻・自動露出など多数の大気エフェクトを統合"

# ==================================================================
# 4. DX12 実装詳細
# ==================================================================
H1 "4. DirectX 12 実装詳細"

H2 "4-1. ルートシグネチャ設計"
Body "機能ごとに独立したルートシグネチャを設計し、不要なディスクリプタバインドを排除しました。"
AddTable @(
    @("ルートシグネチャ", "パラメータ構成"),
    @("Scene（海洋・天空共用）", "CBV[b0] + SRV table(t0/t1/t2) + CBV[b1]"),
    @("Bloom", "SRV table(t0) + 4 Root Constants"),
    @("ToneMap", "SRV table(t0/t1) + SRV table(t2) + 8 Root Constants"),
    @("DOF", "SRV table(t0/t1) + 4 Root Constants"),
    @("LensFlare", "6 Root Constants（テクスチャ不使用）")
)

H2 "4-2. ディスクリプタヒープ管理"
Body "DX12 の「同時バインド可能な CBV/SRV/UAV ヒープは 1 つ」という制約を考慮し、統合ヒープ設計でパス間の切り替えを最小化しました。"
AddTable @(
    @("ヒープ", "スロット構成・用途"),
    @("m_oceanSRVHeap（3スロット）", "FFT heightMap + dztMap + SkySnapshot → 海洋パスで 1 バインドカバー"),
    @("m_bloomSRVHeap（4スロット）", "HDR RT + BloomExtract + BloomBlur + GodRay → ToneMap パス"),
    @("m_dofSRVHeap（2スロット）", "HDR RT + DepthBuffer（R32_FLOAT ビュー）→ DOF パス専用")
)

# ==================================================================
# 5. 技術的課題と解決策
# ==================================================================
H1 "5. 技術的課題と解決策"

H2 "5-1. ディスクリプタヒープの同時バインド制約"
Body "【課題】SSR 実装時、FFT テクスチャ（既存ヒープ）と天空スナップショット（新規）を同一パスで参照する必要があったが、DX12 では CBV/SRV/UAV ヒープは 1 つしかバインドできない。"
Body "【解決】両 SRV を収容する統合ヒープ（m_oceanSRVHeap）を新設。同一リソースに複数の SRV を作成できる DX12 仕様を活用し、既存ヒープへの変更なしに 3 スロット構成を実現。"

H2 "5-2. 深度バッファの Typeless 化"
Body "【課題】DOF パスで深度バッファを SRV として読み取る必要があったが、D32_FLOAT リソースは SRV 作成不可。"
Body "【解決】リソースを R32_TYPELESS に変更。DSV は D32_FLOAT ビュー、SRV は R32_FLOAT ビューとして同一リソースから作成。clearValue は DSV フォーマットに合わせて設定。"

H2 "5-3. HDR ブルームの不可視問題"
Body "【課題】LDR パイプライン（R8G8B8A8_UNORM）では 1.0 クランプにより太陽・稲妻のブルームが視認不可。"
Body "【解決】オフスクリーン RT を R16G16B16A16_FLOAT に変更し HDR パイプライン化。ACES トーンマップ前に HDR 値を保持することで、輝度の高い箇所がブルームとして視覚化される設計に変更。"

H2 "5-4. ポストプロセス RT の状態管理"
Body "【課題】9 つの RT が複数パスで共有され、RT ↔ PSR ↔ COPY_SOURCE/DEST の遷移管理が複雑化。フレーム間のバリア漏れによりGPU バリデーションエラーが発生。"
Body "【解決】各パスの「終了時保証状態」を仕様化し、RenderToneMap() にクリーンアップバリアを集約することで次フレームの初期状態を一元保証。"

# ==================================================================
# 6. 実装規模・アピールポイント
# ==================================================================
H1 "6. 実装規模"
AddTable @(
    @("指標", "数値"),
    @("C++ ファイル", "約 12 ファイル / 約 3,000 行"),
    @("HLSL シェーダーファイル", "12 ファイル / 20+ エントリポイント / 約 1,500 行"),
    @("レンダーパス数", "9 パス"),
    @("レンダーターゲット数", "9 RT"),
    @("定数バッファ種別", "6 種（SceneCB・SkyCB・RippleCB 他）"),
    @("グリッド解像度", "512 × 512（約 52 万インデックス）"),
    @("FFT 解像度", "256 × 256")
)

H1 "7. アピールポイント（エンジンプログラマー志望）"
Bullet "DX12 の深い実装経験 ― ルートシグネチャ・ヒープ・バリア・フェンスを抽象化なしで手書き実装"
Bullet "レンダリングパイプライン設計力 ― 9 パスのフレームグラフを依存関係を考慮して設計・管理"
Bullet "GPU コンピュートプログラミング ― FFT・物理シミュレーションをコンピュートシェーダーで独自実装"
Bullet "エンジン設計パターンの実践 ― 疎結合サブシステム・一方向データフロー・CB 環形管理を独自実装"
Bullet "問題解決能力 ― バリデーションエラー・ヒープ制約・Typeless リソース等 DX12 固有の課題を自力解決"
Body " "
Body "本ドキュメントに記載の機能はすべて実装・動作検証済みです。"

# ==================================================================
# 保存
# ==================================================================
$doc.SaveAs2($out, 16)   # 16 = wdFormatDocumentDefault (.docx)
$doc.Close()
$word.Quit()
Write-Host "生成完了: $out"
