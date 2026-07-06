# mod-xpcatchup

A dynamic XP redistribution module for **AzerothCore** (WotLK 3.3.5a).

When a group of characters kill creatures together, this module redistributes the accumulated kill XP so that lower-level characters catch up to the current highest-level eligible member of the group for that kill — keeping everyone on a similar progression track.

> **Note:** Works with both the mod-playerbots fork and vanilla AzerothCore. `RequireRealMaster` is now a deprecated compatibility option and is ignored; catchup works for players and bots alike.

---

## Features

- **XP Equalization** — Collects kill XP from all eligible party or raid members into a shared pool and redistributes it based on each member's progress relative to the current highest-level eligible character for that kill
- **Two Distribution Modes:**
  - **Lowest** (default) — gives the entire XP pool to the single character furthest behind
  - **Dynamic** — splits the pool proportionally, weighted by how far behind each character is
- **Configurable Level Window** — only characters within N levels of the current highest-level eligible character participate (default: 7 levels)
- **Players and Bots** — catchup works for players and bots alike; the old `RequireRealMaster` option is retained only for backward-compatible config files
- **Kill XP Only** — quest XP, reputation XP, and other sources are left untouched
- **Configurable & Debuggable** — toggle features, set a catchup activation threshold, and enable debug logging to watch the redistribution in real time
- **Catchup Threshold** — skip redistribution when all members are within a configurable margin of each other, letting natural XP flow
- **Reference-Aware Catchup** — if the current highest-level eligible character later falls behind another equally high-level character’s progress, the new highest-progress character naturally becomes the reference on subsequent kills

---

## How It Works

### Basic Flow

1. Party members kill a creature. All XP from that kill is collected into a redistribution pool — no XP is lost, it all flows through the pool.
2. The pool accumulates XP from every group member for that victim (tracked by victim GUID).
3. When all members have contributed, the pool is redistributed — either all to the lowest-XP member (lowest mode), or split proportionally by XP deficit (dynamic mode).
4. The highest-level eligible character for that kill becomes the reference point. If no eligible member is at least 10 percent behind that reference, catchup is skipped entirely and all XP flows naturally.
5. Members outside the configured level window from that reference are excluded from redistribution.

### Example

```
Highest eligible player (Audin, level 8): 10,000 / 12,000 XP  (83.3% progress — reference)
Ally (level 8):                    5,000 / 12,000 XP   (41.7% progress)
Bob (level 8):                     3,000 / 12,000 XP   (25.0% progress)
Bard (level 8):                    7,000 / 12,000 XP   (58.3% progress)
Dave (level 8):                    9,000 / 12,000 XP   (75.0% progress)
```

In **lowest mode**, Bob gets the entire redistributed pool (lowest progress ratio).

In **dynamic mode**, weights combine level deficit (100% per level) plus XP progress ratio deficit relative to the current highest-level eligible reference player:
- Ally (0 levels, ratio deficit 41.7%) → weight 4167 → ~31% of pool
- Bob (0 levels, ratio deficit 58.3%) → weight 5833 → ~44% of pool
- Bard (0 levels, ratio deficit 25.0%) → weight 2500 → ~19% of pool
- Dave (0 levels, ratio deficit 8.3%) → weight 833 → ~6% of pool

If a character is 2 levels below the reference player, they get a 200% base deficit on top of their ratio deficit:
- Newbie (level 6, 30% progress) vs reference (83.3%) → weight = 20000 + 5330 = 25330

---

## Requirements

- AzerothCore (WotLK 3.3.5a) — works with both the [mod-playerbots fork](https://github.com/mod-playerbots/azerothcore-wotlk/) and vanilla AzerothCore
- MySQL 8.0+
- Compiler with C++20 support

---

## Installation

### 1. Clone the Module

Navigate to your AzerothCore modules directory:

```bash
cd <ACoreDir>/modules
git clone https://github.com/Jellypowered/mod-xpcatchup.git
```

### 2. Compile

Re-compile AzerothCore:

If using the acore.sh script: 
```bash
cd <ACoreDir>
./acore.sh compiler build
```

Manually: 
```bash
cd <ACoreDir>/build
cmake ../ -DCMAKE_INSTALL_PREFIX=/path/to/server
make -j $(nproc)
make install
```

The module is compiled statically into the `modules` library and loaded automatically — no separate module loading needed.

### 3. Configure

Edit `<ACoreDir>/env/dist/etc/modules/xpcatchup.conf` in your server's config directory:

```conf
# XP Catch-Up Module Configuration

# Main toggle - enables or disables the XP catch-up system
XPCatchup.Enable = true

# Level difference window - members more than this far from the current
# highest-level eligible character for the kill are excluded from XP redistribution
# Recommended: 7 (matches the effective party XP sharing range in WotLK)
# Higher values allow catch-up for wider level spreads but may include
# members who receive negligible XP from kills.
XPCatchup.LevelWindow = 7

# Deprecated compatibility option.
# Catchup now works for players and bots alike and uses the highest-level
# eligible character for each kill as the reference point, not the party leader.
# This setting is currently ignored and kept only for backward compatibility.
XPCatchup.RequireRealMaster = true

# Distribution algorithm:
#   0 = lowest  - gives entire pool to the single lowest-XP member (default)
#   1 = dynamic - splits pool proportionally, weighted by XP deficit
XPCatchup.Distribution = 0

# Catchup activation threshold (percent)
# Catchup only activates when at least one group member has a deficit >=
# this percentage relative to the current highest-level eligible character.
XPCatchup.Threshold = 10

# Chat debug mode - sends what each bot received to party chat after each kill
# Messages appear like: "[XP Catch-Up] received 840 XP (weight 4200; deficit: 42.00%)"
XPCatchup.ChatDebug = false

# Debug logging - enables LOG_INFO messages to console and Server.log
# Requires logger level 4 or 5 in worldserver.conf (e.g. Logger.XPCatchup=4,Console Server)
XPCatchup.Logging = false
```

If you want console logs for the mods output to monitor what the mod is doing add this line to your worldserver.conf: 
```
Logger.XPCatchup=4,Console Server
```

### 4. Restart Server

Restart your worldserver to load the module:

```bash
./worldserver
```

---

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `XPCatchup.Enable` | Boolean | `true` | Enable or disable the XP catch-up system globally |
| `XPCatchup.LevelWindow` | Integer | `7` | Maximum level difference from the current highest-level eligible character for eligible members. Recommended: 7 (matches WotLK party XP sharing range) |
| `XPCatchup.RequireRealMaster` | Boolean | `true` | Deprecated compatibility option. Ignored by current logic; catchup works for players and bots alike. |
| `XPCatchup.Distribution` | Integer | `0` | `0` = lowest, `1` = dynamic weighted distribution |
| `XPCatchup.Threshold` | Integer | `10` | Catchup activates only when at least one eligible member has a deficit >= this percentage relative to the current highest-level eligible character. Set to 0 to always activate. |
| `XPCatchup.ChatDebug` | Boolean | `false` | Send redistribution details to party chat after each kill |
| `XPCatchup.Logging` | Boolean | `false` | Enable detailed LOG_INFO output to console and Server.log |

---

## How It Works (Technical)

### Hook: `OnPlayerGiveXP`

The module hooks into AzerothCore's `PlayerScript::OnPlayerGiveXP` callback, which fires once per group member after a creature kill. The `amount` parameter is passed by reference, allowing the module to intercept the XP before it's applied.

### Accumulation & Redistribution

1. **Threshold check** — when `OnPlayerGiveXP` fires, the module first finds the current highest-level eligible character for that kill and checks whether any eligible group member has a deficit >= `Threshold` percent relative to that reference. If not, the hook returns immediately and all XP flows naturally.
2. **For each player who kills** — the core calculates their XP share, then `OnPlayerGiveXP` fires. The module intercepts it and adds it to a pending pool (tracked by victim GUID). The player's XP is zeroed at this point.
3. **Pool accumulation** — the module waits for every AzerothCore-eligible XP recipient for that kill to contribute. The pool contains the sum of all those players' XP for that specific kill.
4. **Redistribution** — once all eligible contributors have contributed, the pool is redistributed according to the selected algorithm. No XP is lost; it flows through the pool and out to the recipients.
5. In **lowest mode**, the entire pool goes to the single lowest-XP member. In **dynamic mode**, it is split proportionally by XP progress ratio deficit weighted by level difference.

### Eligibility Rules

- Only `XPSOURCE_KILL` XP is intercepted (quest XP, reputation, etc. pass through)
- Only players AzerothCore would reward for that kill are pooled and considered for redistribution
- Members more than `LevelWindow` levels from the current highest-level eligible character are excluded
- If no eligible member has a deficit >= `Threshold` percent relative to the current highest-level eligible character, catchup is skipped entirely
- Players and bots are both supported; `RequireRealMaster` is deprecated and ignored
- Dead members are naturally excluded (core doesn't call `OnPlayerGiveXP` for them)

### Cleanup

A world script runs every 5 seconds to clean up expired pending XP entries (older than 10 seconds since the last contribution), preventing stale pools from lingering forever while still allowing large groups enough time to finish a kill’s XP callback fan-out.

---

## Edge Cases & Limitations

- **Deprecated Master Setting** — `RequireRealMaster` is retained only for backward-compatible config files and is ignored by the current logic.
- **Level 80 Players** — Level 80 players receive zero XP from the core. The module sees already-zero XP and has no effect.
- **Rest XP** — Rest XP bonuses apply to redistributed XP automatically (interception happens before `GiveXP()` is called).
- **Pets** — Pet XP (`GivePetXP`) is not intercepted; bots control their own pets.
- **Reference Changes** — The highest-level eligible character for each kill becomes the reference for that kill. No persistent leader tracking is required.

---

## Debugging

### Chat Debug

> **WARNING:** This can be VERY spammy in busy dungeons. Only enable for verification/debugging.

Enable chat debug mode by setting `XPCatchup.ChatDebug = true`. After each kill, a party chat message appears for each beneficiary bot, showing what they received:

```
[XP Catch-Up] received 840 XP (weight 4200; deficit: 42.00%)
[XP Catch-Up] received 465 XP (weight 2000; deficit: 20.00%)
[XP Catch-Up] received 93 XP (weight 100; deficit: 1.00%)
```

These appear in **party chat** as if the bot sent them. All group members will see each message.

- **weight** — proportional weight for XP distribution (higher = further behind). Combines level deficit (100% per level) plus XP progress ratio deficit.
- **deficit** — XP progress ratio deficit relative to the current highest-level eligible reference character (percentage), before this kill. A higher percentage means further behind that reference player's progress.

### Debug Logging

> **WARNING:** This can be VERY spammy in busy dungeons. Only enable for verification/debugging.

Enable debug logging by setting `XPCatchup.Logging = true`. This outputs all `LOG_INFO` messages (first contributions, pool redistribution, target selection, etc.) to both the **Server.log** file and the **console** (if the logger level allows).

Note: `LOG_INFO` messages also require the logger level in `worldserver.conf` to be set to at least 4 (INFO) or 5 (DEBUG):
```
Logger.XPCatchup=4,Console Server
```

Or enable it at runtime:
```
.server.log XPCatchup 4
```

Both `XPCatchup.Logging` and the logger level must be enabled for messages to appear.

---

## Test Setup

This module was tested against the following commit hashes:

AzerothCore Playerbots Branch: `4d380b987`
- `mod-ah-bot-plus`: `9fbcbe6`
- `mod-aoe-loot`: `2ddf6ff`
- `mod-autobalance`: `73d4ad3`
- `mod-dungeon-master`: `a488836`
- `mod-guildfunds`: `f69bff1`
- `mod-individual-progression`: `fd0673d`
- `mod-junk-to-gold`: `2134690`
- `mod-multibot-bridge`: `fba3d24`
- `mod-xpcatchup`: `d356cd4`
- `mod-player-bot-level-brackets`: `b03737f`
- `mod-playerbots`: `1041b0b0`
- `mod-quest-loot-party`: `6f073c1`

---

## File Structure

```
mod-xpcatchup/
├── src/
│   ├── XPCatchup.h              # Main class declarations
│   ├── XPCatchup.cpp            # Hook implementations
│   └── XPCatchup_loader.cpp     # Module entry point
├── conf/
│   └── xpcatchup.conf.dist      # Configuration template
└── data/
    └── sql/
        └── db-world/
            └── skeleton_module_acore_string.sql
```

---

## License

This module is released under the [GNU AGPL v3 License](LICENSE).

---

*Built for [AzerothCore](https://www.azerothcore.org/) — open-source WoW 3.3.5a server emulator.*
