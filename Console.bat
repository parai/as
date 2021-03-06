@echo off

set ASPATH=%~dp0
set astmp=%ASPATH%
set ASDISK=%astmp:~1,2%
set MSYS2=C:\msys64

%ASDISK%
cd %ASPATH%

if NOT EXIST "%ASPATH%\Console.bat" goto perror
if NOT EXIST %MSYS2%\usr\bin goto install_msys2

REM base env PATH
set PATH=C:\WINDOWS\system32;C:\WINDOWS;C:\WINDOWS\System32\Wbem;C:\WINDOWS\System32\WindowsPowerShell\v1.0

set PATH=C:\Anaconda3;C:\Anaconda3\Scripts;%MSYS2%\mingw64\bin;%MSYS2%\usr\bin;%MSYS2%\mingw32\bin;%PATH%

if NOT EXIST "%ASPATH%\release\download" mkdir %ASPATH%\release\download

set CZ=%ASPATH%\release\download\ConsoleZ\Console.exe
set ConEmu=%ASPATH%\release\download\ConEmu\ConEmu64.exe

if EXIST %ConEmu% goto prepareEnv
cd %ASPATH%\release\download
mkdir ConEmu
cd ConEmu
wget https://github.com/Maximus5/ConEmu/releases/download/v18.06.26/ConEmuPack.180626.7z
"C:\Program Files\7-Zip\7z.exe" x ConEmuPack.180626.7z
cd %ASPATH%

if EXIST %CZ% goto prepareEnv
cd %ASPATH%\release\download
wget https://github.com/cbucher/console/releases/download/1.18.3/ConsoleZ.x64.1.18.3.18143.zip
mkdir ConsoleZ
cd ConsoleZ
unzip ..\ConsoleZ.x64.1.18.3.18143.zip
cd %ASPATH%

:prepareEnv
set MSYS=winsymlinks:nativestrict
if EXIST "%ASPATH%\.setup.done" goto launchConsole
REM pacman -Syuu
pacman -S unzip wget git mingw-w64-x86_64-gcc mingw-w64-x86_64-glib2 mingw-w64-x86_64-gtk3
pacman -S mingw32/mingw-w64-i686-gcc mingw-w64-x86_64-diffutils
pacman -S ncurses-devel gperf scons curl make cmake automake-wrapper libtool
pacman -S unrar mingw-w64-x86_64-pkg-config mingw-w64-x86_64-binutils
pacman -S msys2-runtime-devel
conda install scons pyserial
echo 0 > .setup.done

:launchConsole
REM env.asc in format "tokens=value" such as "PACKET_LIB_ADAPTER_NR=2" to set some environment
if EXIST "%ASPATH%\env.asc" for /F "tokens=*" %%I in (%ASPATH%\env.asc) do set %%I
cd %ASPATH%
set ASROOT=%ASPATH%
set PYTHONPATH=%ASPATH%/com/as.tool/config.infrastructure.system;%ASPATH%/com/as.tool/config.infrastructure.system/third_party;%PYTHONPATH%
set ISMSYS2=YES
REM check CI parameter
IF "%1"=="ci" goto runCI
if EXIST %ConEmu% goto launchConEmu
if EXIST %CZ% goto launchCZ

:launchConEmu
start %ConEmu% -title aslua-ascore-asboot-asone ^
	-runlist -new_console:d:"%ASPATH%":t:astool ^
	^|^|^| -new_console:d:"%ASPATH%":t:ascore ^
	^|^|^| -new_console:d:"%ASPATH%":t:asboot ^
	^|^|^| -new_console:d:"%ASPATH%\com\as.tool\as.one.py":t:asone
exit 0

:launchCZ
start %CZ% -ws %ASPATH%\ConsoleZ.workspace
exit 0

:runCI
%ASDISK%
cd \
mkdir asci
cd asci
git clone https://github.com/parai/as.git
cd as
rm release/download build -fr
mkdir -p release/download
git pull

set BOARD=posix
set RELEASE=ascore
scons
set RELEASE=asboot
scons

set BOARD=versatilepb
set RELEASE=ascore
scons
set RELEASE=asboot
scons

set BOARD=any
set ANY=lua
scons
set ANY=pyas
scons
set ANY=aslib
scons
goto exitPoint

:install_msys2
set msys2="www.msys2.org"
echo Please visit %msys2% and install msys2 as c:\msys64
pause
exit -1

:perror
echo Please fix the var "ASDISK" and "ASPATH" to the right path!
pause

:exitPoint
