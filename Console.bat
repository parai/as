@echo off

set ASPATH=%~dp0
:set tmp="%ASPATH%"
set ASDISK=%tmp:~1,2%
set MSYS2="C:\msys64"
:compile lwext4
set CMAKE_LEGACY_CYGWIN_WIN32=1

%ASDISK%
cd %ASPATH%

if NOT EXIST "%ASPATH%\Console.bat" goto perror
if NOT EXIST %MSYS2%\usr\bin goto install_msys2

set PATH=C:\Anaconda3;C:\Anaconda3\Scripts;%MSYS2%\mingw64\bin;%MSYS2%\usr\bin;%MSYS2%\mingw32\bin;%PATH%

if NOT EXIST "%ASPATH%\release\download" mkdir %ASPATH%\release\download

set CZ=%ASPATH%\release\download\ConsoleZ\Console.exe
set ConEmu=%ASPATH%\release\download\ConEmu\ConEmu64.exe

if EXIST %ConEmu% goto prepareEnv
cd %ASPATH%\release\download
mkdir ConEmu
cd ConEmu
wget https://github.com/Maximus5/ConEmu/releases/download/v18.06.17/ConEmuPack.180617.7z
"C:\Program Files\7-Zip\7z.exe" x ConEmuPack.180617.7z
cd ..

if EXIST %CZ% goto prepareEnv
cd %ASPATH%\release\download
wget https://github.com/cbucher/console/releases/download/1.18.2/ConsoleZ.x64.1.18.2.17272.zip
mkdir ConsoleZ
cd ConsoleZ
unzip ..\ConsoleZ.x64.1.18.2.17272.zip

:prepareEnv
set MSYS=winsymlinks:nativestrict
if EXIST "%ASPATH%\scons.bat" goto launchConsole
REM pacman -Syuu
pacman -S unzip wget git mingw-w64-x86_64-gcc mingw-w64-x86_64-glib2 mingw-w64-x86_64-gtk3
pacman -S ncurses-devel gperf scons curl
conda install scons pyserial
echo @echo off > scons.bat
echo @echo !!!SCONS on MSYS2!!! >> scons.bat
echo %MSYS2%\usr\bin\python2.exe %MSYS2%\usr\bin\scons %%* >> scons.bat

:launchConsole
cd %ASPATH%
set ASROOT=%ASPATH%
set PYTHONPATH=%ASPATH%/com/as.tool/config.infrastructure.system;%ASPATH%/com/as.tool/config.infrastructure.system/third_party;%PYTHONPATH%
if EXIST %ConEmu% goto launchConEmu
if EXIST %CZ% goto launchCZ

:launchConEmu
start %ConEmu% -title aslua-ascore-asboot-asone ^
	-runlist -new_console:d:"%ASPATH%\release\aslua":t:aslua ^
	^|^|^| -new_console:d:"%ASPATH%\release\ascore":t:ascore ^
	^|^|^| -new_console:d:"%ASPATH%\release\asboot":t:asboot ^
	^|^|^| -new_console:d:"%ASPATH%\com\as.tool\as.one.py":t:asone
exit 0

:launchCZ
start %CZ% -ws %ASPATH%\ConsoleZ.workspace
exit 0

:install_msys2
set msys2="www.msys2.org"
echo Please visit %msys2% and install msys2 as c:\msys64
pause
exit -1

:perror
echo Please fix the var "ASDISK" and "ASPATH" to the right path!
pause
