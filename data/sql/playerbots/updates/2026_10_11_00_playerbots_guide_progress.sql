-- Guildmaster guides (Plan 5c): each guided bot's place in its levelling guide, so it resumes there after a restart
-- or a relog and never walks back through travel steps. `line` is the step's line in the Zygor file: it finds the
-- step again when the route file changes.
CREATE TABLE IF NOT EXISTS `playerbots_guide_progress` (
  `bot` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `guide` VARCHAR(48) NOT NULL COMMENT 'guide id, e.g. a-human-1-13',
  `seq` SMALLINT UNSIGNED NOT NULL COMMENT 'the first step not yet passed',
  `line` INT UNSIGNED NOT NULL DEFAULT 0,
  `updated_at` INT UNSIGNED NOT NULL COMMENT 'unix seconds',
  PRIMARY KEY (`bot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
