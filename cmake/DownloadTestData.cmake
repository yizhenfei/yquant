cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/TestDataManifest.cmake")

if(NOT DEFINED YQUANT_TEST_DATA_DIR)
  get_filename_component(YQUANT_TEST_DATA_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../test_data" ABSOLUTE)
endif()
if(NOT IS_ABSOLUTE "${YQUANT_TEST_DATA_DIR}")
  message(FATAL_ERROR "YQUANT_TEST_DATA_DIR must be an absolute path")
endif()

function(yquant_file_matches path expected_hash output)
  set(${output} FALSE PARENT_SCOPE)
  if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
    file(SHA256 "${path}" actual_hash)
    if(actual_hash STREQUAL expected_hash)
      set(${output} TRUE PARENT_SCOPE)
    endif()
  endif()
endfunction()

file(MAKE_DIRECTORY "${YQUANT_TEST_DATA_DIR}")
file(LOCK "${YQUANT_TEST_DATA_DIR}/.download.lock"
  GUARD PROCESS TIMEOUT 660 RESULT_VARIABLE lock_result)
if(NOT lock_result STREQUAL "0")
  message(FATAL_ERROR "Cannot lock test-data cache: ${lock_result}")
endif()

set(snapshot
  "${YQUANT_TEST_DATA_DIR}/${YQUANT_TEST_DATA_SNAPSHOT_RELATIVE_PATH}")
yquant_file_matches("${snapshot}"
  "${YQUANT_TEST_DATA_SNAPSHOT_SHA256}" snapshot_valid)
if(snapshot_valid)
  message(STATUS "Test data ready: ${snapshot}")
  return()
endif()

set(archive
  "${YQUANT_TEST_DATA_DIR}/${YQUANT_TEST_DATA_ARCHIVE_RELATIVE_PATH}")
yquant_file_matches("${archive}"
  "${YQUANT_TEST_DATA_ARCHIVE_SHA256}" archive_valid)
if(NOT archive_valid)
  get_filename_component(archive_directory "${archive}" DIRECTORY)
  file(MAKE_DIRECTORY "${archive_directory}")
  set(partial_archive "${archive}.part")
  file(REMOVE "${partial_archive}")
  message(STATUS "Downloading test data: ${YQUANT_TEST_DATA_ARCHIVE_URL}")
  file(DOWNLOAD "${YQUANT_TEST_DATA_ARCHIVE_URL}" "${partial_archive}"
    TLS_VERIFY ON TIMEOUT 600 INACTIVITY_TIMEOUT 60
    STATUS download_status)
  list(GET download_status 0 download_code)
  list(GET download_status 1 download_message)
  if(NOT download_code EQUAL 0)
    file(REMOVE "${partial_archive}")
    message(FATAL_ERROR
      "Test-data download failed (${download_code}): ${download_message}")
  endif()
  yquant_file_matches("${partial_archive}"
    "${YQUANT_TEST_DATA_ARCHIVE_SHA256}" download_valid)
  if(NOT download_valid)
    file(REMOVE "${partial_archive}")
    message(FATAL_ERROR "Downloaded test-data archive has an invalid SHA256")
  endif()
  file(RENAME "${partial_archive}" "${archive}" RESULT archive_result)
  if(NOT archive_result STREQUAL "0")
    file(REMOVE "${partial_archive}")
    message(FATAL_ERROR
      "Cannot publish test-data archive: ${archive_result}")
  endif()
endif()

# Keep extraction on the same filesystem as the final CSV so rename is atomic.
set(staging "${YQUANT_TEST_DATA_DIR}/.dolphindb-snapshot-staging")
file(REMOVE_RECURSE "${staging}")
file(MAKE_DIRECTORY "${staging}")
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${staging}"
  PATTERNS "${YQUANT_TEST_DATA_SNAPSHOT_ARCHIVE_MEMBER}")
set(snapshot_archive
  "${staging}/${YQUANT_TEST_DATA_SNAPSHOT_ARCHIVE_MEMBER}")
if(NOT EXISTS "${snapshot_archive}")
  file(REMOVE_RECURSE "${staging}")
  message(FATAL_ERROR "Test-data archive does not contain the snapshot ZIP")
endif()
file(ARCHIVE_EXTRACT INPUT "${snapshot_archive}"
  DESTINATION "${staging}/snapshot" PATTERNS "snapshot.csv")
set(staged_snapshot "${staging}/snapshot/snapshot.csv")
yquant_file_matches("${staged_snapshot}"
  "${YQUANT_TEST_DATA_SNAPSHOT_SHA256}" extracted_valid)
if(NOT extracted_valid)
  file(REMOVE_RECURSE "${staging}")
  message(FATAL_ERROR "Extracted snapshot.csv has an invalid SHA256")
endif()

get_filename_component(snapshot_directory "${snapshot}" DIRECTORY)
file(MAKE_DIRECTORY "${snapshot_directory}")
file(RENAME "${staged_snapshot}" "${snapshot}" RESULT snapshot_result)
file(REMOVE_RECURSE "${staging}")
if(NOT snapshot_result STREQUAL "0")
  message(FATAL_ERROR "Cannot publish snapshot.csv: ${snapshot_result}")
endif()
message(STATUS "Test data ready: ${snapshot}")
