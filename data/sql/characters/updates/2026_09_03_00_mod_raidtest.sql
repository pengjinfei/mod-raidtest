-- mod-raidtest: 自动化团测结果表
-- 设计文档: docs/04-mod-raidtest-设计.md §6
CREATE TABLE IF NOT EXISTS `raidtest_accounts` (
    `scenario_key` VARCHAR(64) NOT NULL,
    `slot` TINYINT NOT NULL,
    `account_id` INT UNSIGNED NOT NULL,
    `character_guid` INT UNSIGNED NOT NULL,
    `class` TINYINT NOT NULL,
    `role` VARCHAR(16) NOT NULL,
    `created_at` DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`scenario_key`, `slot`)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS `raidtest_runs` (
    `id` INT UNSIGNED AUTO_INCREMENT,
    `scenario_key` VARCHAR(64) NOT NULL,
    `map_id` INT UNSIGNED NOT NULL,
    `boss_entry` INT UNSIGNED NOT NULL,
    `attempts_total` INT NOT NULL,
    `kills` INT DEFAULT 0,
    `wipes` INT DEFAULT 0,
    `timeouts` INT DEFAULT 0,
    `started_at` DATETIME DEFAULT CURRENT_TIMESTAMP,
    `finished_at` DATETIME NULL,
    PRIMARY KEY (`id`),
    KEY `idx_scenario` (`scenario_key`)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS `raidtest_attempts` (
    `id` INT UNSIGNED AUTO_INCREMENT,
    `run_id` INT UNSIGNED NOT NULL,
    `seq` INT NOT NULL,
    `result` ENUM('kill','wipe','timeout','aborted') NOT NULL,
    `duration_ms` INT NOT NULL,
    `boss_hp_min` TINYINT NULL,
    `deaths` INT NOT NULL,
    `death_names` TEXT NULL,
    `notes` VARCHAR(255) NULL,
    PRIMARY KEY (`id`),
    KEY `idx_run` (`run_id`, `seq`)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS `raidtest_events` (
    `id` BIGINT UNSIGNED AUTO_INCREMENT,
    `attempt_id` INT UNSIGNED NOT NULL,
    `rel_ms` INT NOT NULL,
    `event_type` ENUM('spell','damage','death','boss_hp','combat_start','combat_end','strategy','state') NOT NULL,
    `source_guid` BIGINT UNSIGNED NOT NULL,
    `target_guid` BIGINT UNSIGNED NULL,
    `spell_id` INT UNSIGNED NULL,
    `actor_entry` INT UNSIGNED NULL,
    `value` INT NULL,
    `detail` VARCHAR(255) NULL,
    PRIMARY KEY (`id`),
    KEY `idx_attempt` (`attempt_id`, `rel_ms`),
    KEY `idx_attempt_type` (`attempt_id`, `event_type`)
) ENGINE=InnoDB;