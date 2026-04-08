-- mod-pvpbots: initial database setup
-- Run this once against your acore_world database.
-- AzerothCore will apply files in data/sql/db-world/ automatically on startup.

CREATE TABLE IF NOT EXISTS `pvpbots_registry` (
    `id`            INT UNSIGNED    NOT NULL AUTO_INCREMENT,
    `account_id`    INT UNSIGNED    NOT NULL COMMENT 'Account ID from auth.account',
    `char_guid`     INT UNSIGNED    NOT NULL COMMENT 'Character GUID from characters.characters',
    `char_name`     VARCHAR(12)     NOT NULL COMMENT 'Character name',
    `faction`       TINYINT         NOT NULL COMMENT '0 = Horde, 1 = Alliance',
    `role`          TINYINT         NOT NULL COMMENT '0 = Ganker, 1 = Duelist, 2 = Objective',
    `class`         TINYINT         NOT NULL COMMENT 'WoW class ID (1=Warrior, 2=Paladin, etc.)',
    `state`         TINYINT         NOT NULL DEFAULT 0 COMMENT '0=Idle, 1=Active, 2=Dead, 3=InBG',
    `last_updated`  TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uq_char_guid` (`char_guid`),
    KEY `idx_role` (`role`),
    KEY `idx_faction` (`faction`),
    KEY `idx_state` (`state`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-pvpbots: registry of all pvp bot characters';
