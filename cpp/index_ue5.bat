@echo off
REM ============================================================
REM  Start UE5 source indexing via WAX RAG Server
REM  Requires: WAX server running on 127.0.0.1:8080
REM
REM  enrich_regex = true   (UCLASS/UPROPERTY/UFUNCTION/includes)
REM  enrich_llm   = false  (355K chunks — too slow for LLM)
REM  resume       = false  (full re-index to pick up enriched facts)
REM ============================================================

echo ============================================================
echo  UE5 Indexing with Regex Enrichment
echo  Target: j:\UE5.2SRC\Engine\Source
echo  Server: http://127.0.0.1:8080
echo ============================================================
echo.

REM --- index.start: begins background indexing ---
curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"j:/UE5.2SRC/Engine/Source\",\"resume\":false,\"flush_every_chunks\":131072,\"ingest_batch_size\":1,\"include_extensions\":[\".h\",\".hpp\",\".cpp\",\".inl\",\".inc\"],\"enrich_regex\":true,\"enrich_llm\":false}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   index_status.bat
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
