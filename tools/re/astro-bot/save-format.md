# ASTRO BOT Rescue Mission (CUSA12392): save format and level unlocks

Static analysis of `eboot.elf` (addresses at load base 0) and the game data. Nothing here was run.
Generator for the edited saves: `make_save.py` (this folder).

## 1. Container: `sce_sdmemory/memory.dat`

- This file is shadPS4's backing store for `sceSaveDataSetupSaveDataMemory2`, `GetSaveDataMemory2` and
  `SetSaveDataMemory2` (wrappers at 0xc5e6b0, 0xc5e9b0 and 0xc5eb90). It has no header and no checksum.
  `param.sfo` has no checksum or size that depends on the data, so it is copied unchanged.
- The game hands out blocks starting at offset 0x400 (0xc5e5c4: `mov [rsi+0x10], 0x400`). The first 0x400
  bytes are never written, which is why they are zero.
- The PlayRoom block is 0x8000 bytes (0x15ff0 calls 0xc5ee70 with `ecx=0x8000`), so the XML must stay under 32 KiB.
- Write path (0xc5ef90 calls 0xc5eb90): the game serializes the XML and writes **only strlen bytes, without a NUL**.
  Any bytes left over from an earlier, longer write stay in the file. The trailing `>` after `</PlayRoom>` in the
  original save is one such stale byte (the previous text was 1 character longer, for example
  `IntroWatched false` before it became `true`).
- Read path (0xc5eee0 calls 0xc5e9b0): it reads the whole block, then parses the XML. Junk after the root
  element is tolerated, because the original save loads.
- On every load the game increments `PlayCount` (0x16e90, "SaveData for %s is ready play count = %d").

## 2. Schema (built by reflection in 0x15ff0; save object S = `[[App+0x48]]+0x18`, getter 0xa80cd0)

Each int field object stores value/default/min/max/step at +0x90/+0x94/+0x98/+0x9c/+0xa0.
The setter 0xe45290 clamps the value to [min, max].

| tag | S+field | range (default) | meaning |
|---|---|---|---|
| PlayCount | 0xd0 | 0..INT_MAX | +1 on each load |
| NewGameSelectCount | 0x180 | 0..INT_MAX | +1 when NEW GAME is chosen (0x9058b0, 0x90ab20). Statistics only, not a gate |
| Coin | 0x230 | 0..INT_MAX | coins (crane game currency) |
| IntroWatched | 0x2e0 (bool) | false | intro played |
| L1..L78 | 0x380 + i*0x630 | record, see below | per-level progress; `GetLevel(S,i)` = 0x168a0 (asserts i<78) |
| CraneGame_* | 0x1e620.. | | crane game state (0x176b0) |
| HeroDeathFlag | 0x1e9d0 | 0..INT_MAX | |

### Level record (constructor 0x17390). Only A..E are registered for serialization; F/G/H are runtime only.

| tag | value at | range (default) | meaning and evidence |
|---|---|---|---|
| **A** | +0x140 | 0..1 (0) | **unlocked / selectable.** `IsLocked` 0x94b890 is `!(A==1)`. A planet is locked iff all its levels have A!=1 (0x97bcd0). Lock-level routine 0x9941e0 sets A=0. **A=2 is clamped to 1.** World-select init always forces L1.A=1 (0xa85f8b), which is why the fresh save has L1 A=1. |
| **B** | +0x1f0 | 0..9999 (0) | **rescued-bot bitmask** (bit i = bot i). Level end ORs result bits into it (0x8d8290). Total bots = sum of popcount(B) (0x168c0). Debug JSON name "bots" (0xb0b350). A full normal level is 0xFF (8 bots). Challenge levels use 2 bits (3). Bosses use 0. |
| **C** | +0x2a0 | 0..9999 (0) | **chameleon found** bitmask (bit0). Level end ORs it in (0x8d8290). `HasChameleon` 0x94d9d0. Chameleons unlock challenge stages. |
| **D** | +0x350 | 0..9999 (0) | **clear count** (debug JSON name "numClear"). Level end does `D += 1` (0x8d843c). `IsCleared` = D>0 (0x94c300). |
| **E** | +0x400 | 0..5999 (5999) | **best time**: level end does `E = min(E, (int)result.time)` (0x8d8399). 5999 means "no record" (= 99:59 if the unit is seconds; the unit was not confirmed). |
| F/G/H | +0x4b0/+0x560/+0x610 | not saved | runtime: cleared this session / score accumulators |

"100% complete" (crown, 0x94da50) means: normal level popcount(B)>=8 and C!=0; boss D>0; challenge popcount(B)>=2.

## 3. L number to level

Formula in 0x94ec60 (also inlined in 0x94d880 and 0x94ed10). Inputs: level type (+0x1a0: 0 Normal, 1 Challenge,
2 Collection, 3 Boss, 4 Multi, 5 Credits; parser 0x942480), planet index p (+0x190, planet order in
`mup_product_levels.xml`), and index l within the planet (+0x194):

- Normal/Boss: `p*5 + l`. If the `<FinalBoss>` flag (+0x19c) is set: 25.
- Collection: 26. Challenge: `27 + l`. Multi: `57 + l` (not used in product). Credits: 77.
- The L tag number is index+1.

Data: `data/multi_platformer/mup_product_levels.xml` (`<Planets>`). Note: the order of the flat `<Levels>` list
there does **not** matter.

| L | World | name | dir (`levels/<dir>`) |
|---|---|---|---|
| 1-5 | W1 | ROOFTOPS, CONSTRUCTION, CAVE & CANNONS, ROCK CLIMBING, GORILLA (boss) | world0_0_new_intro_1, world0_0_new_intro_2, cave_trap_1, cave_canyons_half1, rope_boss_test_01 |
| 6-10 | W2 | BEANSTALK, UNDERWATER, GIANT 1, MINE CART, FIRE OCTOPUS (boss) | beanstalk, sea_whale_split1, giant_1, mine_coaster_04, water_boss_final |
| 11-15 | W3 | CANYONS, INDY CAVE 1, INTO THE WHALE, FOREST CASTLE, BIRD BOSS | cave_canyons_half2, world3_1_Indy_cave_split1, sea_whale_split2, forest_castle, bird_boss_test_01 |
| 16-20 | W4 | GIANT 2, FUNFAIR, FIRE TRAPS, INDY CAVE 2, SPIDER | giant_2, city_funfair_remake, cave_trap_2, world3_1_Indy_cave_split2, spider_boss_test_01 |
| 21-25 | W5 | HEAVEN & STORM, GRAVEYARD, NINJA, VOLCANO, SHARK BOSS | heaven, cave_graveyard, ninja_stage, volcano, shark_boss_01 |
| 26 | final | ALIEN BOSS | final_boss |
| 27 | | COLLECTION (mothership) | collection_room |
| 28-53 | Challenge planet, in order | BUMPERS!, DODGEBALL!, CANNON MANIA, ROPE MIX, GORILLA CH., WATER SHOOTING, UNDERWATER MAZE, CATERPILLAR MANIA, EXTREME PLATFORMS, FIRE OCTOPUS CH., WATERFALL, LASERS, WAVES, VERTICAL TREE, BIRD BOSS CH., BUMPERS! (HARD), MACHINE GUN, FIRE ROAD, TATAMI, SPIDER BOSS CH., STORM CITY, LIGHT BLOCKS, SHURIKEN RIVER, LAVA RUN, SHARK BOSS CH., FINAL BOSS CH. | challenge_* |
| 54-77 | unused (58+ = Multi) | | |
| 78 | | CREDITS ROLL | challenge_credits |

## 4. Unlock rules

- Unlocking is driven by `RequiredForUnlock` in `mup_product_levels.xml`; each world is a chain.
  W2's BEANSTALK requires GORILLA, and CANYONS requires FIRE OCTOPUS. The game applies unlocks (sets A=1 and plays
  UnlockSequence, SequenceGotoNewPlanet, ChallengeUnlockSequence, ...) only **when returning from a level**.
  That path uses the "last played level" name (0xa86380), the list of dependents (0xa867a0) and the return
  sequence 0xa914e0, which hides new levels via the `+0xf6` flag until they are animated.
  A cold CONTINUE has no last level, so the world select starts in its idle state (0xa87780, state 2),
  and whatever A says is what is shown. **The previous attempt failed because L6 (BEANSTALK) stayed A=0,
  so World 2 had no unlocked level.**
- Bosses (0x94c580): the unlock needs A==0, the prerequisite's D>0, and total bots (popcount of all B) >= `LostbotsRequired`.
  From `multi_platformer.xml` `Menus/WorldSelect/.../LevelParameters`: gorilla 20, octopus 40, bird 60, spider 80,
  shark 100, final_boss 140. Setting A=1 directly bypasses this.
- Challenges: `RequiredForUnlock` (a main level) plus that level's chameleon; boss challenges need the boss cleared.
- Collection: requires GORILLA.
- `-sequence demo` (mode 2, see below): the world select resets the save (0x169a0) and opens World 1 only:
  4 levels with A=1,D=1 and the boss with A=1,D=0 (0xa7f742..0xa7fcb6). This confirms A=unlocked and D=cleared.

## 5. Command line (`/app0/args.txt` plus extra args; parser 0xbe3960, startup 0x540)

- Sequences registered through 0xbd2270: `main` (0x1001), `drs` = DebugLaunchSequence (0x1002),
  `product` (0x1003), `demo` (0x1004). `-sequence demo` sets mode 2; any other `-sequence` sets mode 1
  (`[cfg+0x734]`).
- `-package null|default|master|future` sets 1..4. Other flags: `-usertest`, `-debugInput true|false`.
- `-room <Room>` (with `-viewer`/`gv`) starts DebugLaunchSequence directly in that room (0x540 calls 0x1660 with 0x1002).
- `-level <file>` / `-camera <name>` (0xb02a30) only override the **first room request of type 0x401**.
  Rooms: TitleRoom=0x402; Calibration/Orb/Pause/Credit=0x403; **SelectRoom=0x401** (0xb1c031); level rooms=0x401.
  - In the product flow, the first 0x401 request is the world select itself, so `-level` cannot pick a
    gameplay level there. This matches the observation that it was ignored.
  - The only direct-load path is `-room WorldRoom -level <dir>`. It runs DebugLaunchSequence, whose debug
    server (socket wrappers 0xc17ce0..0xc180e0, JSON commands such as `SaveData{levels[index].numClear,bots}`
    at 0xb0b350) is the path that crashed.
- There is no other save-independent level select in the product sequence.

## 6. Edited saves (generated by `make_save.py`)

Both `memory.dat` copies are edited identically. The text is CRLF + tabs and is otherwise unchanged; the stale
`>` trailer is kept. The file stays 1 MiB, the 0x400 prefix stays zero, and `param.sfo` is unchanged.

- `build/pc-bench/save-w2/` is the state after playing W1 and W2 to 100%:
  - L1-4 and L6-9: A=1 B=255 C=1 D=1
  - L5 GORILLA and L10 FIRE OCTOPUS: A=1 D=1
  - L11 CANYONS: A=1, so World 3 appears with one level
  - L27 COLLECTION: A=1
  - L28-37 (the 10 challenges tied to W1/W2): A=1
  - Total bots: 64
- `build/pc-bench/save-all/` (less conservative):
  - W1-W5 all cleared the same way (160 bots)
  - L26 ALIEN BOSS: A=1, not cleared
  - COLLECTION and challenges L28-52 unlocked

## 7. World select layout (`levels/world_select/lvx/design.lvx`)

Positions relative to the player, who faces -Z:

| planet | horizontal | vertical |
|---|---|---|
| World1 | 86° left | 23° up |
| World2 | 53° left | 3° up |
| World3 | 18° left | 22° up |
| World4 | 16° right | 11° up |
| World5 | 54° right | 14° up |
| Challenge | 85° right | 13° up |
| Collection (mothership) | 46° left | 44° up |
| FinalBoss/Credits | 31° right | 45° up |

- In far view a planet highlights when you look within `LookAtAngleIn` = 18° of it.
- Each planet has five pieces, `planet_piece01..05`: levels 1-4, then the boss (piece05 carries the boss icon).
- To enter BEANSTALK with save-w2: CONTINUE, then in the world select select World2 (about 33° right of and
  about 20° below World1), then in near view pick the first piece.
- It was not verified statically which planet the camera starts on.
