@echo off
rem Uploads a build to a console over FTP through Docker (see Dockerfile.deploy). Leaves no image behind.
rem
rem   deploy.bat [folder]       folder holds n3ds-cast.3dsx and/or n3ds-cast.cia (default: out)
rem
rem The console is 3ds-ftpd.local, port 5000. Override with the N3DS_FTP_HOST and N3DS_FTP_PORT environment variables.
setlocal
cd /d "%~dp0"

set FOLDER=%~1
if "%FOLDER%"=="" set FOLDER=out
for %%I in ("%FOLDER%") do set FOLDER=%%~fI
if "%N3DS_FTP_HOST%"=="" set N3DS_FTP_HOST=3ds-ftpd.local
if "%N3DS_FTP_PORT%"=="" set N3DS_FTP_PORT=5000

set TAG=n3ds-cast-deploy:tmp
docker build -q -f Dockerfile.deploy -t %TAG% . >nul
if errorlevel 1 exit /b 1
docker run --rm -v "%FOLDER%:/build" -e N3DS_FTP_HOST -e N3DS_FTP_PORT %TAG%
set CODE=%errorlevel%
docker rmi -f %TAG% >nul 2>&1
exit /b %CODE%
