# tsurugi-graph

[English](README_en.md) | 日本語

TsurugiDB 向けグラフデータベースエンジン。Shirakami KVS 上で Cypher クエリサブセットを ACID トランザクション付きで実行します。

## 特徴

- **Cypher サブセット対応** — CREATE / MATCH / WHERE / SET / DELETE / DETACH DELETE / RETURN / UNWIND
- **有向エッジ** — `(a)-[r:KNOWS]->(b)` パターンの作成・検索
- **プロパティインデックス** — 等値検索 O(1)、範囲検索 O(V) の転置インデックス
- **ラベルインデックス** — ラベルベースのノード検索
- **クエリテンプレートキャッシュ** — AST ディープコピーとリテラルバインドによる LRU キャッシュ
- **ACID トランザクション** — Shirakami OCC 上のリトライロジック (最大5回、指数バックオフ)
- **バッチ操作** — UNWIND 一括挿入、バッチプロパティ読み出し
- **ストリーミング実行** — BATCH_SIZE=1024 のチャンク処理で 10M+ ノードに対応

## アーキテクチャ

```
クライアント (Tateyama プロトコル)
    │
    ▼
service.cpp ── リクエストルーティング、リトライ、クエリキャッシュ
    │
    ▼
core/parser.cpp ── Cypher 字句解析 + 再帰下降構文解析 → AST
    │
    ▼
core/executor.cpp ── AST 実行エンジン (シンボルテーブル管理)
    │
    ▼
storage.cpp ── ノード/エッジ CRUD、6つの KVS インデックス
    │
    ▼
Shirakami KVS ── Masstree B+tree、MVCC、WAL
```

### ストレージスキーマ (6 KVS インデックス)

| ストレージ | キー | 値 | 用途 |
|:--|:--|:--|:--|
| `graph_nodes` | node_id (8B BE) | JSON プロパティ | ノード本体 |
| `graph_edges` | edge_id (8B BE) | from+to+label+props | エッジ本体 |
| `graph_label_index` | label + node_id | (空) | ラベル→ノード |
| `graph_out_index` | from_id + edge_id | to_id | 出力エッジ |
| `graph_in_index` | to_id + edge_id | from_id | 入力エッジ |
| `graph_prop_index` | label\0key\0value | packed node IDs | 転置プロパティインデックス |

## 対応 Cypher 構文

| 句 | 例 |
|:--|:--|
| CREATE (ノード) | `CREATE (n:Person {name: 'Alice', age: 30})` |
| CREATE (エッジ) | `CREATE (a:Person)-[r:KNOWS]->(b:Person)` |
| MATCH | `MATCH (n:Person) RETURN n` |
| MATCH (エッジ) | `MATCH (a)-[r:KNOWS]->(b) RETURN a, b` |
| WHERE (=, >, <, >=, <=, <>) | `MATCH (n) WHERE n.age >= 30 RETURN n` |
| SET | `MATCH (n) SET n.age = 31 RETURN n` |
| DELETE | `MATCH (n:Person) DELETE n` |
| DETACH DELETE | `MATCH (n:Person) DETACH DELETE n` |
| RETURN AS | `MATCH (n) RETURN n.name AS name` |
| UNWIND | `UNWIND ['a','b'] AS x CREATE (n:Item {name: x})` |

## ビルド

### 前提条件

- CMake 3.20+
- C++17 対応コンパイラ (GCC 11+ / Clang 14+)
- Protobuf, glog
- TsurugiDB (Shirakami, tateyama) — フルビルド時

### フルビルド (サーバ統合)

```bash
# tsurugidb ルートから
./install.sh --prefix=$HOME/tsurugi
```

ビルド順序: takatori → yugawara → shirakami → sharksfin → jogasaki → **tsurugi-graph** → tateyama-bootstrap

### スタンドアロンビルド (テスト用)

```bash
cd graph-service
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/tsurugi/install -DBUILD_TESTING=ON
cmake --build .
ctest
```

### テスト直接実行 (サーバ依存なし)

```bash
cd graph-service
g++ -std=c++17 -I include -I test -I test/tateyama/framework/graph \
  test/tateyama/framework/graph/executor_test.cpp \
  src/tateyama/framework/graph/storage.cpp \
  src/tateyama/framework/graph/core/parser.cpp \
  src/tateyama/framework/graph/core/executor.cpp \
  -o executor_test -lpthread && ./executor_test
```

## サーバ起動

```bash
export PATH=$HOME/tsurugi/bin:$PATH
tgctl start     # graph-service はサービスID 13 で自動ロード
tgctl status    # 稼働確認
tgctl shutdown  # 停止
```

接続は Tateyama プロトコル (IPC/TCP) 経由。サービスID **13** に Cypher クエリを送信します。

## テスト

17 テストファイル、148 テストケース。モック Shirakami バックエンド使用。100% 通過率。

| コンポーネント | テストファイル | ケース数 |
|:--|:--|---:|
| パーサ | parser_standalone, parser_full, parser_edge, parser_property, parser_coverage | 40 |
| ストレージ | storage_standalone, storage_label, storage_navigation, storage_property, storage_iterator, storage_coverage | 36 |
| エグゼキュータ | executor, executor_advanced, executor_batch, executor_coverage | 36 |
| クエリキャッシュ | query_cache | 35 |
| ラベルマッチ | match_label | 1 |

## 性能

### 実 Shirakami バックエンド (100K ノード)

| 操作 | スループット |
|:--|---:|
| create_node | 47,761 ops/s |
| プロパティインデックス検索 | 265,460 ops/s |
| MATCH+WHERE = (インデックス) | 46,060 ops/s |
| エッジ探索 | 120,171 ops/s |
| パイプライン全体 | 10,719 ops/s |
| **8スレッド読み取り** | **953K ops/s** |

### 他 DB との比較 (100K ノード)

| 操作 | tsurugi-graph | Neo4j | Memgraph | FalkorDB |
|:--|---:|---:|---:|---:|
| CREATE | 47.8K | 1.9K | 7.9K | 768 |
| 等値検索 | 265K | 6.3K | 4.3K | 6.0K |
| エッジ探索 | 120K | 2.8K | 889 | 3.3K |

詳細は [BENCHMARK.md](BENCHMARK.md) を参照。

## ADR (設計記録)

| # | タイトル | 主な効果 |
|:--|:--|:--|
| 0001 | グラフエンジンアーキテクチャ | 5 層設計、6 KVS インデックス |
| 0002 | セカンダリプロパティインデックス | O(N)→O(log N) (0007 で置換) |
| 0003 | クエリキャッシュ | パース 4μs 削減 |
| 0004 | バッチプロパティ読み出し | 大結果セットで 2-3x 改善 |
| 0005 | 並列クエリ実行 | マルチスレッド WHERE |
| 0006 | UNWIND 一括挿入 | 個別 CREATE 比 25% 高速 |
| 0007 | 転置プロパティインデックス | O(1) 等値検索、57x 高速化 |
| 0008 | エポック長チューニング | デフォルト 40ms 最適 |
| 0009 | 楽観的インデックス書き込み | ユニーク値で 1 KVS コール |
| 0010 | 範囲プロパティインデックス | 不等号 WHERE O(V) |
| 0011 | クエリテンプレートキャッシュ | リテラル正規化で命中率向上 |
| 0012 | ストリーミングエグゼキュータ | ピークメモリ 5GB→100MB |

## ディレクトリ構成

```
tsurugi-graph/
├── README.md              # 本ファイル (日本語)
├── README_en.md           # English version
├── BENCHMARK.md           # 性能レポート
├── graph-service/
│   ├── CMakeLists.txt
│   ├── README.md          # graph-service 詳細
│   ├── proto/             # Protobuf 定義 (request/response)
│   ├── include/           # 公開ヘッダ
│   │   └── tateyama/framework/graph/
│   │       ├── service.h, resource.h, storage.h
│   │       └── core/ (ast.h, parser.h, executor.h, query_cache.h)
│   ├── src/               # 実装 (~2,400行)
│   │   └── tateyama/framework/graph/
│   │       ├── service.cpp, resource.cpp, storage.cpp
│   │       └── core/ (parser.cpp, executor.cpp)
│   ├── test/              # テスト (17ファイル, 148ケース)
│   ├── bench/             # ベンチマーク
│   └── docs/adr/          # ADR (0001-0012)
└── build/
```

## ライセンス

Apache License 2.0 (TsurugiDB プロジェクトに準拠)
