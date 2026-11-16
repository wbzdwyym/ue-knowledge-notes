@echo off
rem 用本机 Visual Studio 2022 的 cl.exe 编译并运行示例
rem 用法: 双击本文件，或在 cmd 里执行 build_and_run.bat
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [ERR] 未找到 vcvars64.bat，请改成你本机的 VS 路径
    exit /b 1
)
call "%VCVARS%" >nul
cd /d "%~dp0"
rem /utf-8 必加：源码含中文，MSVC 默认按系统代码页(936)读会报 C2001
cl /nologo /EHsc /std:c++17 /W4 /utf-8 opaque_error_demo.cpp /Fe:opaque_error_demo.exe
if errorlevel 1 exit /b 1
echo.
opaque_error_demo.exe
endlocal
