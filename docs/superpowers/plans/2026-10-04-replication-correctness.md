# Replication Correctness Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 修复架构审查发现的数据分歧、资源放大、副本读取和复制生命周期问题。

**Architecture:** 保留 slot、immutable snapshot 和轮询复制。写入与确定的日志在同一 slot 锁内提交；副本串行应用，读与应用由复制模块互斥。轮询通过进度报告确认，日志缺口触发快照。

**Tech Stack:** C++17, CMake, existing C++/Tcl/shell tests.

**Spec:** 本次用户确认的 KISS/YAGNI 修复方案，以及 `docs/superpowers/specs/2026-05-30-cache-server-replication-design.md` 的单主多副本、slot 原子性和断线恢复要求。

## Constraints and decisions

- 当前 bugfix 分支直接执行；不操作用户已有的未跟踪文件，不自动提交。
- 不添加 worker 池、通用事件框架或外部依赖。
- 复制协议升级到 v2，拒绝不兼容版本；节点需一起升级。
- 过期时间使用 Unix 微秒，日志记录主库执行时间，副本不物理清理重放仍需要的过期对象。
- 全局 binlog 预算是周期维护的软上限；压力回收允许落后副本重新快照。
- 保留每个命令的现有实现；仅收敛没有独立语义的执行包装和旧复制 API。

## Review focus

延迟重放跨过 TTL；快照并发写入；空批次和主库重启；预算淘汰后的缺口；多 key/SCAN 与重同步并发读取。

## Tasks

- [x] 1. Add failing regression tests for SPOP effects, HDEL single commit, delayed TTL, empty-slot readiness, budget eviction, read gating, and bounded codec allocations. Build and record red results.
- [x] 2. Update `src/cache`, command mutations and time semantics: log overrides from mutations, recorded execution time/final deadline, replica writes without re-logging; HDEL and SPOP regressions green.
- [x] 3. Update `src/protocol/resp_codec.*`, `src/repl/repl_frame.*` and snapshot decoding: allocate scratch by actual input, decode parsed args directly, v2 round trips and malformed-frame tests.
- [x] 4. Update master/slave/link and runtime: serialized session handling, ordered replica apply, explicit session/completion frames, fair bounded batches, actual budget reclamation and periodic maintenance. Test reconnect, empty slots, replacement master and gap recovery.
- [x] 5. Move replica read policy into serialized execution; command metadata covers multi-key reads and SCAN checks visited slots. Test disabled reads, writes, mixed ready slots and malformed commands.
- [x] 6. Remove obsolete wrappers/worker scaffolding, update tests/docs, run C++ suite and full CTest, request independent review and address findings.

## Verification

Each task runs its behavioral regressions after observing the previous failure. Full verification: `cmake --build build -j 4 && ctest --test-dir build --output-on-failure`. Network tests require loopback socket permission. No success claim substitutes for actual test output.

## Progress

- Initial tracked worktree clean; existing untracked `build/` and `dump.rdb` preserved. Baseline CTest 10/10 passed with local TCP permission.
- Observed failing regressions for SPOP effects, HDEL commit count, delayed TTL, budget reclamation, v2 metadata, empty-slot readiness and codec capacity; implemented fixes and C++ suite passed.
- Added full wire round-trip recovery coverage for primary replacement, budget eviction, expiry extension, fair batches and an interrupted response before DONE. Network integration also checks multi-key reads, SCAN and disabled replica reads.
- Full suite after interface cleanup: `CACHE_SERVER_REDIS_UNIT_PORT=17499 ctest --test-dir build --output-on-failure` → 10/10 passed. Default port 6399 was occupied; its existing process was left alone.
- Expiry deletion regression was RED (replica resurrected the value), then GREEN after replay DEL physically removed it.
- Independent read-only review found one Important issue: snapshots omitted expired objects still needed by a queued EXPIRE. Regression `SnapshotPreservesObjectsNeededByAnInFlightExpiryExtension` was RED, then GREEN after snapshots retained stored objects and deadlines. Removed the now-unused snapshot/Poll time parameter.
- Final: regraded startup thread ownership as Important after an occupied-port startup reproduced an AddressSanitizer crash in the maintenance thread. Added clean-failure coverage to network integration; main now stops and joins its background thread before destroying the engine. AddressSanitizer regression: RED, then 10/10 clean exits.
- Final: minor (deferred): tests cover replica read gating and recovery sequentially, but do not include concurrent MGET/SCAN versus resynchronization stress. The independent review found the whole-command mutex scope sound.
- Final: Ruling: authentication/adversarial HELLO is outside this fix; replication remains a trusted interface. Cost if wrong: untrusted peers can disrupt replication.
- Final: Ruling: keep existing whole-slot snapshot size/element limits. Cost if wrong: oversized slots cannot synchronize without future chunking support.
- Final: Ruling: Unix deadlines require synchronized host clocks, and protocol v2 requires coordinated upgrades. Both constraints are documented; clock skew changes expiry visibility and mixed versions cannot replicate.
- Final: Ruling: historical design document is retained with the implemented v2 contract at its top; current runtime behavior supersedes its worker/ACK proposal.
- Final verification: build passed; `CACHE_SERVER_REDIS_UNIT_PORT=17499 ctest --test-dir build --output-on-failure` → 10/10 passed, including 113 C++ cases and local TCP replication tests. AddressSanitizer occupied-port startup → 10/10 clean exits. `git diff --check` passed. Changes remain uncommitted on the existing bugfix branch.
- 2026-10-05 follow-up: review found unbounded snapshot/log batch payloads and a blocking replica `connect` during startup failure. Snapshot and log byte-budget regressions were RED then GREEN; a loopback full-backlog startup probe waited beyond 7.5 seconds before the fix and exited within the 5-second connect deadline afterward. Batches now stop near 16 MiB of data payload and send frames individually; one larger frame is allowed for progress. Full CTest 10/10 passed after a fresh build. This follows the same KISS scope without new configuration.
