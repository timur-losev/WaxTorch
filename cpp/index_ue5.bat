@echo off
REM ============================================================
REM  Start UE5 source indexing via WAX RAG Server
REM  Requires: WAX server running on 127.0.0.1:8080
REM ============================================================

echo Starting UE5 indexing: j:\UE5.2SRC\Engine\Source
echo.

REM --- index.start: begins background indexing ---
curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"j:/UE5.2SRC/Engine/Source\",\"resume\":true,\"flush_every_chunks\":128,\"ingest_batch_size\":1}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.status\",\"params\":{}}"
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
