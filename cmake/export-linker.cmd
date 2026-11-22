@echo off
REM SPDX-License-Identifier: Apache-2.0
REM SPDX-FileCopyrightText: Copyright The LanceDB Authors
REM
REM Run the caller's MSVC linker and, after the lancedb DLL link, install the
REM paired import library generated from the public export file. Every other
REM link invocation is forwarded unchanged.

setlocal EnableExtensions

set "LANCEDB_LINKER=link.exe"
if defined LANCEDB_REAL_LINKER set "LANCEDB_LINKER=%LANCEDB_REAL_LINKER%"

"%LANCEDB_LINKER%" %*
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
