-- 观察会话（真人带队场次）：attempt 行需要一个既不是击杀/团灭/超时、也不是中止的
-- 结果值。沿用既有四值会把「只观察、不编排」的场次混进通过率统计里（此前 aborted
-- 占位行已经造成过一次误判），因此单独加 'observed'。
ALTER TABLE `raidtest_attempts`
    MODIFY COLUMN `result` ENUM('kill','wipe','timeout','aborted','observed') NOT NULL;
