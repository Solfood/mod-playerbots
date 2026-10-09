-- Quest routes safety net (guildmaster spec 2026-10-07 §6): quests a bot gave up on after they stalled. The bot never
-- accepts them again; the list survives restarts. Written by RouteMgr::Drop; the guildmaster bridge publishes the
-- drop count per quest (quest_drops). Empty while AiPlayerbot.QuestRoutes = 0.
CREATE TABLE IF NOT EXISTS `playerbots_dropped_quests` (
  `bot` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `quest` INT UNSIGNED NOT NULL,
  `dropped_at` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`bot`, `quest`),
  KEY `quest` (`quest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
