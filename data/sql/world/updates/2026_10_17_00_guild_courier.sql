-- guildmaster Plan 6b: the guild couriers (OUR OWN RULE). Two templates that borrow the Children's Week orphans'
-- looks (not the orphans themselves, which carry scripts and quests). The fork spawns them in memory only, and only in
-- a world with AiPlayerbot.GuildBank = 1; an unspawned template changes nothing.
DELETE FROM `creature_template` WHERE `entry` IN (3460700, 3460701);
INSERT INTO `creature_template` (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `faction`, `npcflag`,
  `speed_walk`, `speed_run`, `unit_class`, `unit_flags`, `unit_flags2`, `type`, `AIName`, `MovementType`,
  `RegenHealth`, `flags_extra`, `ScriptName`, `VerifiedBuild`) VALUES
(3460700, 'Guild Courier', 'Guild Bank', 4, 4, 29, 1, 1, 1, 1, 770, 2048, 7, 'SmartAI', 0, 1, 0, '', 0),
(3460701, 'Guild Courier', 'Guild Bank', 4, 4, 12, 1, 1, 1, 1, 770, 2048, 7, 'SmartAI', 0, 1, 0, '', 0);
DELETE FROM `creature_template_model` WHERE `CreatureID` IN (3460700, 3460701);
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`,
  `VerifiedBuild`) VALUES
(3460700, 0, 14589, 1, 1, 0),
(3460701, 0, 338, 1, 1, 0);
-- Now and then a little idle gesture (talk, laugh, cheer, bow, wave): every 30 to 90 seconds out of combat.
DELETE FROM `smart_scripts` WHERE `entryorguid` IN (3460700, 3460701) AND `source_type` = 0;
INSERT INTO `smart_scripts` (`entryorguid`, `source_type`, `id`, `link`, `event_type`, `event_phase_mask`,
  `event_chance`, `event_flags`, `event_param1`, `event_param2`, `event_param3`, `event_param4`, `event_param5`,
  `event_param6`, `action_type`, `action_param1`, `action_param2`, `action_param3`, `action_param4`, `action_param5`,
  `action_param6`, `target_type`, `target_param1`, `target_param2`, `target_param3`, `target_param4`, `target_x`,
  `target_y`, `target_z`, `target_o`, `comment`) VALUES
(3460700, 0, 0, 0, 1, 0, 100, 0, 30000, 90000, 30000, 90000, 0, 0, 10, 1, 11, 4, 2, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
 'Guild Courier - Out of Combat - Play Random Emote'),
(3460701, 0, 0, 0, 1, 0, 100, 0, 30000, 90000, 30000, 90000, 0, 0, 10, 1, 11, 4, 2, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
 'Guild Courier - Out of Combat - Play Random Emote');
