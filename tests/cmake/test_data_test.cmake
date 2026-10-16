cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and TEST_BINARY_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
file(LOCK "${TEST_BINARY_DIR}/.download-test.lock"
  GUARD PROCESS TIMEOUT 30)
set(test_root "${TEST_BINARY_DIR}/download_test")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}/scripts" "${test_root}/fixture"
  "${test_root}/fixture/stock_market_replay" "${test_root}/origin")
configure_file("${SOURCE_DIR}/cmake/DownloadTestData.cmake"
  "${test_root}/scripts/DownloadTestData.cmake" COPYONLY)

set(snapshot_relative_path "dolphindb/2020-12-31/snapshot.csv")
set(archive_relative_path "archives/stock_market_replay.zip")
set(snapshot_archive_member "stock_market_replay/snapshot.zip")
set(origin "${test_root}/origin/stock_market_replay.zip")
set(downloader "${test_root}/scripts/DownloadTestData.cmake")
file(WRITE "${test_root}/fixture/snapshot.csv"
  "Date,Time,SecurityID,BidPX1,BidSize1,OfferPX1,OfferSize1\n"
  "2020.12.31,09:30:00.000,000519,9.61,8500,9.62,6600\n")
file(SHA256 "${test_root}/fixture/snapshot.csv" snapshot_hash)

function(create_archive)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${snapshot_archive_member}"
      --format=zip snapshot.csv
    WORKING_DIRECTORY "${test_root}/fixture"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Cannot create snapshot fixture: ${error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${origin}" --format=zip
      "${snapshot_archive_member}"
    WORKING_DIRECTORY "${test_root}/fixture"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Cannot create replay fixture: ${error}")
  endif()
endfunction()

function(write_manifest archive_hash)
  file(WRITE "${test_root}/scripts/TestDataManifest.cmake"
    "set(YQUANT_TEST_DATA_ARCHIVE_URL [==[file://${origin}]==])\n"
    "set(YQUANT_TEST_DATA_ARCHIVE_SHA256 ${archive_hash})\n"
    "set(YQUANT_TEST_DATA_ARCHIVE_RELATIVE_PATH ${archive_relative_path})\n"
    "set(YQUANT_TEST_DATA_SNAPSHOT_ARCHIVE_MEMBER ${snapshot_archive_member})\n"
    "set(YQUANT_TEST_DATA_SNAPSHOT_SHA256 ${snapshot_hash})\n"
    "set(YQUANT_TEST_DATA_SNAPSHOT_RELATIVE_PATH ${snapshot_relative_path})\n")
endfunction()

function(run_download cache expected_success)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DYQUANT_TEST_DATA_DIR=${cache}"
      -P "${downloader}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 15)
  if(NOT result MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Download process did not finish: ${result}")
  elseif(expected_success AND NOT result STREQUAL "0")
    message(FATAL_ERROR "Download unexpectedly failed: ${output}${error}")
  elseif(NOT expected_success AND result STREQUAL "0")
    message(FATAL_ERROR "Download unexpectedly succeeded: ${output}")
  endif()
endfunction()

function(assert_snapshot cache)
  set(snapshot "${cache}/${snapshot_relative_path}")
  if(NOT EXISTS "${snapshot}")
    message(FATAL_ERROR "Expected snapshot is missing: ${snapshot}")
  endif()
  file(SHA256 "${snapshot}" actual_hash)
  if(NOT actual_hash STREQUAL snapshot_hash)
    message(FATAL_ERROR "Snapshot contents do not match the fixture")
  endif()
endfunction()

function(assert_not_published cache)
  if(EXISTS "${cache}/${snapshot_relative_path}")
    message(FATAL_ERROR "A failed download published a snapshot")
  endif()
endfunction()

create_archive()
file(SHA256 "${origin}" archive_hash)
write_manifest("${archive_hash}")
set(cache "${test_root}/cache")

# A fresh cache prepares the CSV from the two nested archives.
run_download("${cache}" TRUE)
assert_snapshot("${cache}")

# Once prepared, neither a valid CSV nor recovery from a valid archive needs
# access to the original download location.
file(RENAME "${origin}" "${origin}.offline")
file(RENAME "${cache}/${archive_relative_path}"
  "${cache}/${archive_relative_path}.offline")
run_download("${cache}" TRUE)
assert_snapshot("${cache}")
file(RENAME "${cache}/${archive_relative_path}.offline"
  "${cache}/${archive_relative_path}")
file(REMOVE "${cache}/${snapshot_relative_path}")
run_download("${cache}" TRUE)
assert_snapshot("${cache}")
file(WRITE "${cache}/${snapshot_relative_path}" "damaged CSV\n")
run_download("${cache}" TRUE)
assert_snapshot("${cache}")

# A damaged cached archive is replaced when the source is available again.
file(RENAME "${origin}.offline" "${origin}")
file(REMOVE "${cache}/${snapshot_relative_path}")
file(WRITE "${cache}/${archive_relative_path}" "damaged archive\n")
run_download("${cache}" TRUE)
assert_snapshot("${cache}")
file(SHA256 "${cache}/${archive_relative_path}" cached_archive_hash)
if(NOT cached_archive_hash STREQUAL archive_hash)
  message(FATAL_ERROR "The damaged cached archive was not replaced")
endif()

# Failed transfers and corrupt downloads leave no published CSV or archive.
file(RENAME "${origin}" "${origin}.offline")
set(missing_source_cache "${test_root}/missing_source_cache")
run_download("${missing_source_cache}" FALSE)
assert_not_published("${missing_source_cache}")
if(EXISTS "${missing_source_cache}/${archive_relative_path}")
  message(FATAL_ERROR "A failed transfer published an archive")
endif()
file(WRITE "${origin}" "corrupt download\n")
set(corrupt_download_cache "${test_root}/corrupt_download_cache")
run_download("${corrupt_download_cache}" FALSE)
assert_not_published("${corrupt_download_cache}")
if(EXISTS "${corrupt_download_cache}/${archive_relative_path}")
  message(FATAL_ERROR "An invalid download published an archive")
endif()

# An intact archive containing unexpected CSV contents must also fail.
file(WRITE "${test_root}/fixture/snapshot.csv" "unexpected snapshot\n")
create_archive()
file(SHA256 "${origin}" unexpected_archive_hash)
write_manifest("${unexpected_archive_hash}")
set(corrupt_snapshot_cache "${test_root}/corrupt_snapshot_cache")
run_download("${corrupt_snapshot_cache}" FALSE)
assert_not_published("${corrupt_snapshot_cache}")

# execute_process starts both commands concurrently. Both users must succeed
# when preparing the same empty cache.
file(RENAME "${origin}.offline" "${origin}")
write_manifest("${archive_hash}")
set(concurrent_cache "${test_root}/concurrent_cache")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --log-level=WARNING
    "-DYQUANT_TEST_DATA_DIR=${concurrent_cache}" -P "${downloader}"
  COMMAND "${CMAKE_COMMAND}" --log-level=WARNING
    "-DYQUANT_TEST_DATA_DIR=${concurrent_cache}" -P "${downloader}"
  RESULTS_VARIABLE results OUTPUT_VARIABLE output ERROR_VARIABLE error
  TIMEOUT 15)
foreach(result IN LISTS results)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR
      "Concurrent download failed (${results}): ${output}${error}")
  endif()
endforeach()
assert_snapshot("${concurrent_cache}")
message(STATUS "Test-data download checks passed")
