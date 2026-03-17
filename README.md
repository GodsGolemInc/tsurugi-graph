 TsurugiDBのビルドツールおよびデータベースプロセスの起動手順について、プロジェ
  クトの標準的な作法（docsおよびスクリプトに基づく）を解説します。


  1. 必要なビルドツールと依存ライブラリ
  TsurugiDBはC++17/20で書かれており、以下のツールチェーンが必要です。


   * ビルドシステム: cmake (3.20以上), ninja-build
   * コンパイラ: gcc (11以上) または clang (14以上)
   * 主要な依存関係:
       * Protobuf, glog, gflags, Boost (1.70以上)
       * Intel TBB, rocksdb, libmpdec++-dev
       * OpenJDK 17 (Tsubakuro等のJavaクライアント用)


  現在の環境であれば、プロジェクト直下の apt-install.sh
  を実行することでこれらを一括インストールできます（root権限が必要）。


  2. ビルド・インストール手順
  TsurugiDBは多くのサブモジュールで構成されているため、個別にビルドするよりも、
  ルートにある install.sh を使用するのが最も確実です。

   1 # 全コンポーネントのビルドとインストール
   2 # --prefix でインストール先を指定（例: $HOME/tsurugi）
   3 ./install.sh --prefix=$HOME/tsurugi


  このスクリプトは内部で以下の順序でビルドを実行します：
   1. takatori / yugawara (クエリ最適化)
   2. shirakami / sharksfin (トランザクション・ストレージ)
   3. jogasaki (SQLエンジン)
   4. tsurugi-graph (今回追加したグラフエンジン)
   5. tateyama-bootstrap (サーバ本体)

  3. データベースプロセスの起動・停止
  データベースの管理には、標準ツールである tgctl を使用します。


  起動手順
   1. 環境変数の設定:
   1     export PATH=$HOME/tsurugi/bin:$PATH
   2. 設定ファイルの準備: デフォルトの設定ファイル（tsurugi.ini）が
      $HOME/tsurugi/etc に生成されます。
   3. プロセスの起動:
   1     tgctl start
       * このコマンドにより、バックグラウンドで tsurugidb (サーバプロセス)
         が起動します。
       * 今回実装した graph-service は、サーバ起動時に自動的にロードされます。


  状態確認
   1 tgctl status

  停止手順
   1 tgctl shutdown


  4. クライアントからの接続（グラフクエリの実行）
  今回実装した graph-service
  は、TsurugiDBの標準的なセッションプロトコル（Tateyama）を通じて待ち受けていま
  す。


   1. プロトコル: IPC（プロセス間通信）またはTCPを使用。
   2. 接続先: デフォルトでは ipc:tsurugi で待ち受けます。
   3. クエリ実行: Javaクライアント（Tsubakuro）や、今回作成した graph_bench
      バイナリを使用して、定義したサービスID（13）に対してCypherクエリを送信しま
      す。


  実データベースでのテストを行う際は、まず ./install.sh で全環境を構築し、tgctl
  start
  でサーバを立ち上げた状態で、ベンチマークやテストコードを実行する流れになります
  。

