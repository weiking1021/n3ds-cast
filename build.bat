@echo off
rem Builds with Docker only (see Dockerfile.build). Leaves no image behind.
rem
rem   build.bat                 build the .cia and .3dsx into out\
rem   build.bat test            run the test suite
rem   build.bat format-check    fail if a C file is not clang-formatted
rem   build.bat formatted       reformat the C files in place
setlocal
cd /d "%~dp0"

set TARGET=%~1
if "%TARGET%"=="" set TARGET=out
if "%TARGET%"=="out" goto plain_out
if "%TARGET%"=="formatted" goto plain_formatted
if "%TARGET%"=="test" goto tagged
if "%TARGET%"=="format-check" goto tagged
echo usage: build.bat [out^|test^|format-check^|formatted] 1>&2
exit /b 2

:plain_out
docker build -f Dockerfile.build --target out --output type=local,dest=out .
exit /b %errorlevel%

:plain_formatted
docker build -f Dockerfile.build --target formatted --output type=local,dest=. .
exit /b %errorlevel%

:tagged
rem These targets end in an image; tag it so that it can be removed again.
set TAG=n3ds-cast-%TARGET%:tmp
docker build -f Dockerfile.build --target %TARGET% -t %TAG% .
set CODE=%errorlevel%
docker rmi -f %TAG% >nul 2>&1
exit /b %CODE%
