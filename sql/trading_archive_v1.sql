-- Run against an explicitly selected MySQL 8.4 database before tradingd starts.
-- Migration DDL is intentionally separate from the archive worker.
SET SESSION sql_mode = 'STRICT_ALL_TABLES,NO_ZERO_DATE,NO_ZERO_IN_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION';
SET time_zone = '+00:00';

CREATE TABLE archive_schema (
  version BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  checksum VARBINARY(64) NOT NULL,
  installed_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB;

CREATE TABLE ledgers (
  ledger_id BINARY(16) NOT NULL PRIMARY KEY,
  account_key VARBINARY(128) NOT NULL,
  accounting_version BIGINT UNSIGNED NOT NULL,
  ingested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB;

CREATE TABLE archive_runs (
  ledger_id BINARY(16) NOT NULL,
  run_id BIGINT UNSIGNED NOT NULL,
  previous_run_id BIGINT UNSIGNED NOT NULL,
  previous_end_lsn BIGINT UNSIGNED NOT NULL,
  baseline_version BIGINT UNSIGNED NOT NULL,
  baseline LONGBLOB NOT NULL,
  history_gap BOOLEAN NOT NULL,
  final_lsn BIGINT UNSIGNED NULL,
  ingested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (ledger_id, run_id)
) ENGINE=InnoDB;

CREATE TABLE strategy_sessions (
  ledger_id BINARY(16) NOT NULL,
  strategy_session_id BIGINT UNSIGNED NOT NULL,
  client_id BIGINT UNSIGNED NOT NULL,
  metadata BLOB NOT NULL,
  ingested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (ledger_id, strategy_session_id),
  UNIQUE KEY session_client (ledger_id, client_id)
) ENGINE=InnoDB;

CREATE TABLE trade_events (
  ledger_id BINARY(16) NOT NULL,
  run_id BIGINT UNSIGNED NOT NULL,
  lsn BIGINT UNSIGNED NOT NULL,
  record_type BIGINT UNSIGNED NOT NULL,
  encoding_version BIGINT UNSIGNED NOT NULL,
  payload BLOB NOT NULL,
  monotonic_time_ns BIGINT UNSIGNED NOT NULL,
  ingested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (ledger_id, run_id, lsn)
) ENGINE=InnoDB;

CREATE TABLE archive_gaps (
  ledger_id BINARY(16) NOT NULL,
  run_id BIGINT UNSIGNED NOT NULL,
  gap_no BIGINT UNSIGNED NOT NULL,
  reason BIGINT UNSIGNED NOT NULL,
  previous_run_id BIGINT UNSIGNED NULL,
  previous_end_lsn BIGINT UNSIGNED NULL,
  has_baseline BOOLEAN NOT NULL,
  ingested_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (ledger_id, run_id, gap_no)
) ENGINE=InnoDB;

CREATE TABLE archive_progress (
  ledger_id BINARY(16) NOT NULL PRIMARY KEY,
  run_id BIGINT UNSIGNED NOT NULL,
  baseline_installed BOOLEAN NOT NULL,
  lsn BIGINT UNSIGNED NOT NULL,
  segment_no BIGINT UNSIGNED NOT NULL,
  segment_offset BIGINT UNSIGNED NOT NULL,
  projection_version BIGINT UNSIGNED NOT NULL,
  updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
    ON UPDATE CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB;

CREATE TABLE orders (
  ledger_id BINARY(16) NOT NULL,
  strategy_session_id BIGINT UNSIGNED NOT NULL,
  local_order_id BIGINT UNSIGNED NOT NULL,
  core_order_id BIGINT UNSIGNED NULL,
  instrument_id BIGINT UNSIGNED NOT NULL,
  side BIGINT UNSIGNED NOT NULL,
  position_effect BIGINT UNSIGNED NOT NULL,
  limit_price BIGINT NOT NULL,
  original_qty BIGINT NOT NULL,
  cumulative_filled_qty BIGINT NOT NULL,
  leaves_qty BIGINT NOT NULL,
  status BIGINT UNSIGNED NOT NULL,
  cancel_pending BOOLEAN NOT NULL,
  local_cancel_id BIGINT UNSIGNED NOT NULL,
  report_sequence BIGINT UNSIGNED NOT NULL,
  broker_namespace VARBINARY(16) NOT NULL,
  broker_order_id VARBINARY(64) NOT NULL,
  receive_time_ns BIGINT UNSIGNED NOT NULL,
  source_run_id BIGINT UNSIGNED NOT NULL,
  source_lsn BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (ledger_id, strategy_session_id, local_order_id),
  UNIQUE KEY order_core (ledger_id, core_order_id),
  KEY order_session_status (ledger_id, strategy_session_id, status),
  KEY order_instrument (ledger_id, instrument_id, core_order_id)
) ENGINE=InnoDB;

CREATE TABLE fills (
  ledger_id BINARY(16) NOT NULL,
  execution_namespace VARBINARY(16) NOT NULL,
  execution_id VARBINARY(64) NOT NULL,
  strategy_session_id BIGINT UNSIGNED NOT NULL,
  local_order_id BIGINT UNSIGNED NOT NULL,
  core_order_id BIGINT UNSIGNED NOT NULL,
  instrument_id BIGINT UNSIGNED NOT NULL,
  side BIGINT UNSIGNED NOT NULL,
  position_effect BIGINT UNSIGNED NOT NULL,
  price BIGINT NOT NULL,
  quantity BIGINT NOT NULL,
  broker_order_id VARBINARY(64) NOT NULL,
  business_time BIGINT UNSIGNED NULL,
  receive_time_ns BIGINT UNSIGNED NOT NULL,
  source_run_id BIGINT UNSIGNED NOT NULL,
  source_lsn BIGINT UNSIGNED NOT NULL,
  baseline_discovered BOOLEAN NOT NULL,
  PRIMARY KEY (ledger_id, execution_namespace, execution_id),
  KEY fill_order (ledger_id, strategy_session_id, local_order_id),
  KEY fill_instrument_time (ledger_id, instrument_id, business_time)
) ENGINE=InnoDB;

CREATE TABLE order_updates (
  ledger_id BINARY(16) NOT NULL,
  run_id BIGINT UNSIGNED NOT NULL,
  lsn BIGINT UNSIGNED NOT NULL,
  strategy_session_id BIGINT UNSIGNED NOT NULL,
  local_order_id BIGINT UNSIGNED NOT NULL,
  update_kind BIGINT UNSIGNED NOT NULL,
  status BIGINT UNSIGNED NOT NULL,
  reason VARBINARY(144) NOT NULL,
  cumulative_filled_qty BIGINT NOT NULL,
  report_sequence BIGINT UNSIGNED NOT NULL,
  local_cancel_id BIGINT UNSIGNED NOT NULL,
  notify_order BOOLEAN NOT NULL,
  PRIMARY KEY (ledger_id, run_id, lsn),
  KEY update_order (ledger_id, strategy_session_id, local_order_id, run_id, lsn)
) ENGINE=InnoDB;

CREATE TABLE account_cash (
  ledger_id BINARY(16) NOT NULL PRIMARY KEY,
  balance BIGINT NOT NULL,
  available BIGINT NOT NULL,
  reserved BIGINT NOT NULL,
  source_run_id BIGINT UNSIGNED NOT NULL,
  source_lsn BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB;

CREATE TABLE account_positions (
  ledger_id BINARY(16) NOT NULL,
  instrument_id BIGINT UNSIGNED NOT NULL,
  total_qty BIGINT NOT NULL,
  available_qty BIGINT NOT NULL,
  reserved_qty BIGINT NOT NULL,
  unavailable_qty BIGINT NOT NULL,
  source_run_id BIGINT UNSIGNED NOT NULL,
  source_lsn BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (ledger_id, instrument_id)
) ENGINE=InnoDB;

INSERT INTO archive_schema (version, checksum)
VALUES (1, 'yquant-trading-archive-v1-20261021');
