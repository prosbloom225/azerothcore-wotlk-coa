CREATE TABLE IF NOT EXISTS `coa_profession_token_award` (
  `boss_entry` INT UNSIGNED NOT NULL,
  `player_guid` INT UNSIGNED NOT NULL,
  `rune_entry` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`boss_entry`, `player_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
