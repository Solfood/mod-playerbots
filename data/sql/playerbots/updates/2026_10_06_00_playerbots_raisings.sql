-- Death Knight raisings (honest world): one row per raising. Also the hall-of-legends record.
CREATE TABLE IF NOT EXISTS `playerbots_raisings` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `old_guid` INT UNSIGNED NOT NULL,
  `new_guid` INT UNSIGNED NOT NULL,
  `account` INT UNSIGNED NOT NULL,
  `name` VARCHAR(12) NOT NULL,
  `race` TINYINT UNSIGNED NOT NULL,
  `gender` TINYINT UNSIGNED NOT NULL,
  `look` VARCHAR(32) NOT NULL COMMENT 'skin:face:hairStyle:hairColor:facialHair',
  `team` TINYINT UNSIGNED NOT NULL COMMENT '0 Alliance, 1 Horde',
  `old_class` TINYINT UNSIGNED NOT NULL,
  `old_level` TINYINT UNSIGNED NOT NULL,
  `guild_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `carry` TEXT NOT NULL COMMENT 'skills=id:step:value:max,...;spells=id,...',
  `state` VARCHAR(16) NOT NULL COMMENT 'logout, unlink, created, done; rollback, failed, failed_login, failed_restore',
  `raised_at` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  KEY `team_time` (`team`, `raised_at`),
  KEY `new_guid` (`new_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
