-- World of Craft part 2, the smart market (guildmaster Plan 7a, spec B): what each auction house remembers, one row
-- per day, house and item. Bots price from past sales and returns (a player could know them); `wanted` is saved for
-- the app only and no bot rule reads it. Written by MarketBook (Note*: one delta per note, merged by the primary key);
-- rows older than AiPlayerbot.Crafting.MarketKeepDays are deleted at startup. Empty while AiPlayerbot.Crafting = 0.
CREATE TABLE IF NOT EXISTS `playerbots_market_days` (
  `day` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix seconds / 86400',
  `house` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'AuctionHouse.dbc id (AuctionHouseEntry::houseId)',
  `item` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'item_template.entry',
  `sold_units` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'units sold that day',
  `sold_copper` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'copper those units fetched',
  `returned_units` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'units that expired unsold',
  `wanted` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'visits that wanted it and found none they could buy (app only)',
  `listed_units` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'units listed at the last snapshot of the day',
  `min_per_unit` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'cheapest buyout per unit at the last snapshot (0 = none listed)',
  PRIMARY KEY (`day`, `house`, `item`),
  KEY `item` (`item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- The bots' gold by level band, one row per snapshot (AiPlayerbot.Crafting.MarketSnapshotMinutes): for the app.
CREATE TABLE IF NOT EXISTS `playerbots_market_gold` (
  `taken_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix seconds',
  `band` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'level / 10',
  `bots` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'bots counted in the band',
  `gold_sum` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'copper',
  `gold_median` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'copper',
  PRIMARY KEY (`taken_at`, `band`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
