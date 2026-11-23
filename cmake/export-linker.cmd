@echo off
REM SPDX-License-Identifier: Apache-2.0
REM SPDX-FileCopyrightText: Copyright The LanceDB Authors
REM
REM Generate and use the paired MSVC export file for the explicitly configured
REM lancedb shared-library link, then install the matching import library.
REM Ordinary Cargo builds do not use this wrapper.

setlocal EnableExtensions

set "LANCEDB_LINKER=link.exe"
if defined LANCEDB_REAL_LINKER set "LANCEDB_LINKER=%LANCEDB_REAL_LINKER%"

if not defined LANCEDB_MSVC_DEF goto :link

lib.exe /nologo /DEF:"%LANCEDB_MSVC_DEF%" /OUT:"%LANCEDB_MSVC_IMPORT_LIBRARY%" /MACHINE:%LANCEDB_MSVC_MACHINE%
if errorlevel 1 exit /b 1
"%LANCEDB_LINKER%" %* "%LANCEDB_MSVC_EXPORT_FILE%"
goto :linked

:link
"%LANCEDB_LINKER%" %*

:linked
set "LANCEDB_STATUS=%ERRORLEVEL%"
if not "%LANCEDB_STATUS%"=="0" exit /b %LANCEDB_STATUS%

if not defined LANCEDB_MSVC_IMPORT_LIBRARY exit /b 0
if not exist "%LANCEDB_MSVC_IMPORT_LIBRARY%" (
  echo export-linker: missing %LANCEDB_MSVC_IMPORT_LIBRARY% 1>&2
  exit /b 1
)

copy /Y "%LANCEDB_MSVC_IMPORT_LIBRARY%" "%LANCEDB_MSVC_IMPORT_LIBRARY_DEPS%" >nul
if errorlevel 1 exit /b 1
copy /Y "%LANCEDB_MSVC_IMPORT_LIBRARY%" "%LANCEDB_MSVC_IMPORT_LIBRARY_UPLIFT%" >nul
if errorlevel 1 exit /b 1

exit /b 0
