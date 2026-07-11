@echo off
setlocal
cd /d "%~dp0"
if not exist "node_modules\ws" (
    call npm ci || exit /b 1
)
call npm start
