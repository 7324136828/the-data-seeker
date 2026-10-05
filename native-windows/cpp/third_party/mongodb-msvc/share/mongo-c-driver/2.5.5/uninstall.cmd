@echo off

rem MongoDB C Driver uninstall program, generated with CMake
rem
rem Copyright 2009-present MongoDB, Inc.
rem
rem Licensed under the Apache License, Version 2.0 (the "License")
rem
rem you may not use this file except in compliance with the License.
rem You may obtain a copy of the License at
rem
rem   http://www.apache.org/licenses/LICENSE-2.0
rem
rem Unless required by applicable law or agreed to in writing, software
rem distributed under the License is distributed on an "AS IS" BASIS,
rem WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
rem See the License for the specific language governing permissions and
rem limitations under the License.

if "%DESTDIR%"=="" (set __prefix=C:\Users\zachn\Documents\GitHub\the-data-seeker\native-windows\cpp\third_party\mongodb-msvc) else (set __prefix=!DESTDIR!\Users\zachn\Documents\GitHub\the-data-seeker\native-windows\cpp\third_party\mongodb-msvc)

(GOTO) 2>nul & (
  <nul set /p "=Remove file: %__prefix%\lib\bson2.dll.lib "
  if EXIST "%__prefix%\lib\bson2.dll.lib" (
    del /Q /F "%__prefix%\lib\bson2.dll.lib" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\bin\bson2.dll "
  if EXIST "%__prefix%\bin\bson2.dll" (
    del /Q /F "%__prefix%\bin\bson2.dll" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\bson-2.5.5\bson_shared-targets.cmake "
  if EXIST "%__prefix%\lib\cmake\bson-2.5.5\bson_shared-targets.cmake" (
    del /Q /F "%__prefix%\lib\cmake\bson-2.5.5\bson_shared-targets.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\bson-2.5.5\bson_shared-targets-release.cmake "
  if EXIST "%__prefix%\lib\cmake\bson-2.5.5\bson_shared-targets-release.cmake" (
    del /Q /F "%__prefix%\lib\cmake\bson-2.5.5\bson_shared-targets-release.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\pkgconfig\bson2.pc "
  if EXIST "%__prefix%\lib\pkgconfig\bson2.pc" (
    del /Q /F "%__prefix%\lib\pkgconfig\bson2.pc" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-bcon.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-bcon.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-bcon.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-clock.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-clock.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-clock.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-context.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-context.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-context.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-decimal128.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-decimal128.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-decimal128.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-endian.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-endian.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-endian.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-iter.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-iter.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-iter.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-json.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-json.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-json.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-keys.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-keys.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-keys.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-oid.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-oid.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-oid.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-prelude.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-prelude.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-prelude.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-reader.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-reader.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-reader.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-string.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-string.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-string.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-types.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-types.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-types.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-utf8.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-utf8.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-utf8.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-value.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-value.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-value.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-vector.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-vector.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-vector.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-version-functions.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-version-functions.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-version-functions.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson-writer.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson-writer.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson-writer.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\bson_t.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\bson_t.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\bson_t.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\compat.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\compat.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\compat.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\error.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\error.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\error.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\macros.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\macros.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\macros.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\memory.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\memory.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\memory.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\config.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\config.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\config.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\bson-2.5.5\bson\version.h "
  if EXIST "%__prefix%\include\bson-2.5.5\bson\version.h" (
    del /Q /F "%__prefix%\include\bson-2.5.5\bson\version.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\bson-2.5.5\00-mongo-platform-targets.cmake "
  if EXIST "%__prefix%\lib\cmake\bson-2.5.5\00-mongo-platform-targets.cmake" (
    del /Q /F "%__prefix%\lib\cmake\bson-2.5.5\00-mongo-platform-targets.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\bson-2.5.5\bsonConfig.cmake "
  if EXIST "%__prefix%\lib\cmake\bson-2.5.5\bsonConfig.cmake" (
    del /Q /F "%__prefix%\lib\cmake\bson-2.5.5\bsonConfig.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\bson-2.5.5\bsonConfigVersion.cmake "
  if EXIST "%__prefix%\lib\cmake\bson-2.5.5\bsonConfigVersion.cmake" (
    del /Q /F "%__prefix%\lib\cmake\bson-2.5.5\bsonConfigVersion.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\pkgconfig\mongoc2.pc "
  if EXIST "%__prefix%\lib\pkgconfig\mongoc2.pc" (
    del /Q /F "%__prefix%\lib\pkgconfig\mongoc2.pc" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\mongoc2.dll.lib "
  if EXIST "%__prefix%\lib\mongoc2.dll.lib" (
    del /Q /F "%__prefix%\lib\mongoc2.dll.lib" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\bin\mongoc2.dll "
  if EXIST "%__prefix%\bin\mongoc2.dll" (
    del /Q /F "%__prefix%\bin\mongoc2.dll" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-config.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-config.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-config.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-version.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-version.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-version.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-apm.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-apm.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-apm.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulk-operation.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulk-operation.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulk-operation.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulkwrite.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulkwrite.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulkwrite.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-change-stream.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-change-stream.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-change-stream.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-pool.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-pool.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-pool.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-side-encryption.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-side-encryption.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-side-encryption.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-collection.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-collection.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-collection.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-cursor.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-cursor.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-cursor.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-database.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-database.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-database.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-error.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-error.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-error.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-flags.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-flags.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-flags.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-find-and-modify.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-find-and-modify.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-find-and-modify.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-bucket.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-bucket.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-bucket.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file-page.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file-page.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file-page.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file-list.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file-list.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-gridfs-file-list.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-handshake.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-handshake.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-handshake.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-host-list.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-host-list.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-host-list.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-init.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-init.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-init.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-iovec.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-iovec.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-iovec.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-log.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-log.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-log.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-macros.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-macros.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-macros.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-oidc-callback.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-oidc-callback.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-oidc-callback.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-opcode.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-opcode.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-opcode.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-optional.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-optional.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-optional.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-prelude.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-prelude.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-prelude.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-read-concern.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-read-concern.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-read-concern.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-read-prefs.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-read-prefs.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-read-prefs.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-server-api.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-server-api.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-server-api.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-server-description.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-server-description.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-server-description.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-session.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-session.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-client-session.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-sleep.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-sleep.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-sleep.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-socket.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-socket.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-socket.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-tls-openssl.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-tls-openssl.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-tls-openssl.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-buffered.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-buffered.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-buffered.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-file.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-file.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-file.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-gridfs.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-gridfs.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-gridfs.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-socket.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-socket.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-socket.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-structured-log.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-structured-log.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-structured-log.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-topology-description.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-topology-description.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-topology-description.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-uri.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-uri.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-uri.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-version-functions.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-version-functions.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-version-functions.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-write-concern.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-write-concern.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-write-concern.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-rand.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-rand.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-rand.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-tls.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-tls.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-stream-tls.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-ssl.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-ssl.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-ssl.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulkwrite.h "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulkwrite.h" (
    del /Q /F "%__prefix%\include\mongoc-2.5.5\mongoc\mongoc-bulkwrite.h" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\mongoc-2.5.5\mongoc-targets.cmake "
  if EXIST "%__prefix%\lib\cmake\mongoc-2.5.5\mongoc-targets.cmake" (
    del /Q /F "%__prefix%\lib\cmake\mongoc-2.5.5\mongoc-targets.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\mongoc-2.5.5\mongoc-targets-release.cmake "
  if EXIST "%__prefix%\lib\cmake\mongoc-2.5.5\mongoc-targets-release.cmake" (
    del /Q /F "%__prefix%\lib\cmake\mongoc-2.5.5\mongoc-targets-release.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\mongoc-2.5.5\mongocConfig.cmake "
  if EXIST "%__prefix%\lib\cmake\mongoc-2.5.5\mongocConfig.cmake" (
    del /Q /F "%__prefix%\lib\cmake\mongoc-2.5.5\mongocConfig.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\lib\cmake\mongoc-2.5.5\mongocConfigVersion.cmake "
  if EXIST "%__prefix%\lib\cmake\mongoc-2.5.5\mongocConfigVersion.cmake" (
    del /Q /F "%__prefix%\lib\cmake\mongoc-2.5.5\mongocConfigVersion.cmake" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\share\mongo-c-driver\2.5.5\COPYING "
  if EXIST "%__prefix%\share\mongo-c-driver\2.5.5\COPYING" (
    del /Q /F "%__prefix%\share\mongo-c-driver\2.5.5\COPYING" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\share\mongo-c-driver\2.5.5\NEWS "
  if EXIST "%__prefix%\share\mongo-c-driver\2.5.5\NEWS" (
    del /Q /F "%__prefix%\share\mongo-c-driver\2.5.5\NEWS" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\share\mongo-c-driver\2.5.5\README.rst "
  if EXIST "%__prefix%\share\mongo-c-driver\2.5.5\README.rst" (
    del /Q /F "%__prefix%\share\mongo-c-driver\2.5.5\README.rst" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\share\mongo-c-driver\2.5.5\THIRD_PARTY_NOTICES "
  if EXIST "%__prefix%\share\mongo-c-driver\2.5.5\THIRD_PARTY_NOTICES" (
    del /Q /F "%__prefix%\share\mongo-c-driver\2.5.5\THIRD_PARTY_NOTICES" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove file: %__prefix%\share\mongo-c-driver\2.5.5\uninstall.cmd "
  if EXIST "%__prefix%\share\mongo-c-driver\2.5.5\uninstall.cmd" (
    del /Q /F "%__prefix%\share\mongo-c-driver\2.5.5\uninstall.cmd" && echo - ok
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\share\mongo-c-driver\2.5.5 "
  if EXIST "%__prefix%\share\mongo-c-driver\2.5.5" (
    rmdir /Q "%__prefix%\share\mongo-c-driver\2.5.5" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\share\mongo-c-driver "
  if EXIST "%__prefix%\share\mongo-c-driver" (
    rmdir /Q "%__prefix%\share\mongo-c-driver" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\lib\pkgconfig "
  if EXIST "%__prefix%\lib\pkgconfig" (
    rmdir /Q "%__prefix%\lib\pkgconfig" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\lib\cmake\mongoc-2.5.5 "
  if EXIST "%__prefix%\lib\cmake\mongoc-2.5.5" (
    rmdir /Q "%__prefix%\lib\cmake\mongoc-2.5.5" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\lib\cmake\bson-2.5.5 "
  if EXIST "%__prefix%\lib\cmake\bson-2.5.5" (
    rmdir /Q "%__prefix%\lib\cmake\bson-2.5.5" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\lib\cmake "
  if EXIST "%__prefix%\lib\cmake" (
    rmdir /Q "%__prefix%\lib\cmake" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\include\mongoc-2.5.5\mongoc "
  if EXIST "%__prefix%\include\mongoc-2.5.5\mongoc" (
    rmdir /Q "%__prefix%\include\mongoc-2.5.5\mongoc" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\include\mongoc-2.5.5 "
  if EXIST "%__prefix%\include\mongoc-2.5.5" (
    rmdir /Q "%__prefix%\include\mongoc-2.5.5" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\include\bson-2.5.5\bson "
  if EXIST "%__prefix%\include\bson-2.5.5\bson" (
    rmdir /Q "%__prefix%\include\bson-2.5.5\bson" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  <nul set /p "=Remove directory: %__prefix%\include\bson-2.5.5 "
  if EXIST "%__prefix%\include\bson-2.5.5" (
    rmdir /Q "%__prefix%\include\bson-2.5.5" 2>nul && echo - ok || echo - skipped ^(non-empty?^)
  ) ELSE echo - skipped: not present
) && (
  ver>nul
)
