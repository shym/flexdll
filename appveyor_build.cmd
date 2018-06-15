@rem ***********************************************************************
@rem *                                                                     *
@rem *                               FlexDLL                               *
@rem *                                                                     *
@rem *                 David Allsopp, OCaml Labs, Cambridge.               *
@rem *                                                                     *
@rem *   Copyright 2018 MetaStack Solutions Ltd.                           *
@rem *                                                                     *
@rem ***********************************************************************

@rem BE CAREFUL ALTERING THIS FILE TO ENSURE THAT ERRORS PROPAGATE
@rem IF A COMMAND SHOULD FAIL IT PROBABLY NEEDS TO END WITH
@rem   || exit /b 1
@rem BASICALLY, DO THE TESTING IN BASH...

@rem Do not call setlocal!
@echo off

goto %1

goto :EOF

:CheckPackage
"%CYG_ROOT%\bin\bash.exe" -lc "cygcheck -dc %1" | findstr %1 > nul
if %ERRORLEVEL% equ 1 (
  echo Cygwin package %1 will be installed
  set CYGWIN_INSTALL_PACKAGES=%CYGWIN_INSTALL_PACKAGES%,%1
)
goto :EOF

:UpgradeCygwin
if "%CYGWIN_INSTALL_PACKAGES%" neq "" "%CYG_ROOT%\setup-%CYG_ARCH%.exe" --quiet-mode --no-shortcuts --no-startmenu --no-desktop --only-site --root "%CYG_ROOT%" --site "%CYG_MIRROR%" --local-package-dir "%CYG_CACHE%" --packages %CYGWIN_INSTALL_PACKAGES:~1% > nul
for %%P in (%CYGWIN_COMMANDS%) do "%CYG_ROOT%\bin\bash.exe" -lc "%%P --help" > nul || set CYGWIN_UPGRADE_REQUIRED=1
"%CYG_ROOT%\bin\bash.exe" -lc "cygcheck -dc %CYGWIN_PACKAGES%"
if %CYGWIN_UPGRADE_REQUIRED% equ 1 (
  echo Cygwin package upgrade required - please go and drink coffee
  "%CYG_ROOT%\setup-%CYG_ARCH%.exe" --quiet-mode --no-shortcuts --no-startmenu --no-desktop --only-site --root "%CYG_ROOT%" --site "%CYG_MIRROR%" --local-package-dir "%CYG_CACHE%" --upgrade-also > nul
  "%CYG_ROOT%\bin\bash.exe" -lc "cygcheck -dc %CYGWIN_PACKAGES%"
)
goto :EOF

:install
set CYG_ROOT=C:\%CYG_ROOT%
set Path=C:\OCaml\bin;%CYG_ROOT%\bin;%Path%

cd "%APPVEYOR_BUILD_FOLDER%"

rem CYGWIN_PACKAGES is the list of required Cygwin packages (cygwin is included
rem in the list just so that the Cygwin version is always displayed on the log).
rem CYGWIN_COMMANDS is a corresponding command to run with --version to test
rem whether the package works. This is used to verify whether the installation
rem needs upgrading.
set CYGWIN_PACKAGES=cygwin make mingw64-i686-gcc-core mingw64-x86_64-gcc-core
set CYGWIN_COMMANDS=cygcheck make i686-w64-mingw32-gcc x86_64-w64-mingw32-gcc x86_64-pc-cygwin-gcc i686-pc-cygwin-gcc

if "%CYG_ROOT%" equ "cygwin" (
  set CYGWIN_PACKAGES=%CYGWIN_PACKAGES% cygwin64-gcc-core gcc-core
) else (
  set CYGWIN_PACKAGES=%CYGWIN_PACKAGES% gcc-core cygwin32-gcc-core
)

rem OCaml cannot (yet) bootstrap flexlink, so it has to be installed separately
if "%OCAML_PORT:~0,-2%" equ "cygwin" (
  set CYGWIN_PACKAGES=%CYGWIN_PACKAGES% flexdll
  set CYGWIN_COMMANDS=%CYGWIN_COMMANDS% flexlink
)

set CYGWIN_INSTALL_PACKAGES=
set CYGWIN_UPGRADE_REQUIRED=0

for %%P in (%CYGWIN_PACKAGES%) do call :CheckPackage %%P
call :UpgradeCygwin

if "%OCAML_PORT%" equ "msvc" call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars32.bat"
if "%OCAML_PORT%" equ "msvc64" call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat"

"%CYG_ROOT%\bin\bash" -lc "$APPVEYOR_BUILD_FOLDER/appveyor_build.sh install" || exit /b 1

goto :EOF

:build
"%CYG_ROOT%\bin\bash" -lc "$APPVEYOR_BUILD_FOLDER/appveyor_build.sh build" || exit /b 1

goto :EOF

:test
"%CYG_ROOT%\bin\bash" -lc "$APPVEYOR_BUILD_FOLDER/appveyor_build.sh test" || exit /b 1

goto :EOF
