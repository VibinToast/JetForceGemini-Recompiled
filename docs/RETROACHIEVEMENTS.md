# RetroAchievements (casual)

Jet Force Gemini has a [RetroAchievements set](https://retroachievements.org/game/30) (63 achievements). The game can
earn them in **casual (softcore)** mode, with every port feature available: modern controls, 60 FPS, cutscene skip and
the rest. Hardcore isn't offered: RetroAchievements only accepts hardcore unlocks from emulators it has approved, and
a recompilation isn't one.

Status: experimental. Tested on the Android build (Retroid Pocket 6, this branch on dev 783271f): the US ROM is
recognised as game 30, and Dwarf Planet Precision (30 pistol kills, a hit count) unlocked in play and shows on the
RetroAchievements profile as a casual unlock. Not yet tested on Windows.

RetroAchievements doesn't know this client, so on the first session its server also awards "Warning: Unknown
Emulator", a placeholder it awards to clients that aren't on its list.

## Logging in

In the launcher's **Play** page, the **RetroAchievements** block: enter your RetroAchievements username and password and choose **Log in**.
The password is used once and isn't stored: the game keeps only RetroAchievements' login token (in
`retroachievements.json` in the game's folder, readable only by your account on Linux and Android) and logs in with
it next time. **Log out** removes it. Nothing runs and
nothing is sent to RetroAchievements until you log in.

Once the game is running, the block shows how many of the set's achievements you have.

## In the game

- **Banners** at the top of the screen: your progress when the game loads, each unlock (title, description, points)
  and mastering the set. Several are shown one after another.
- **Challenge indicators** in the bottom right corner: while an achievement is primed (every condition met but the
  last, such as "no damage taken" during a boss fight), its badge is shown until it unlocks or fails, as in
  DuckStation and RetroArch. Badges are downloaded once into `ra_badges` in the game's folder.

## Mods and options

Achievements keep counting with every mod and option, as casual mode allows (it allows save states too). Some change
how the set's challenges play, and the RetroAchievements block on the Play page, plus a banner when the game loads, says so:

- **Gameplay mods**: any enabled mod with a tag other than HUD, Controls, Visuals or Audio, or with no tags. Custom
  levels come as mods tagged Levels. They can make a challenge easier or harder (more enemies, other weapons). The note
  names the first such mod and how many others are on.
- **Keep Area Progress** and **Permanent Tribal Saves**: they let an area's challenge be split over several visits or
  loads, which makes some "in one visit" Tribal achievements easier (Rover Rescue, They're All Boxed In, We Aren't
  Looking for Diamonds, Struck in the Muck, Strength of the Wielder). The **Modern (Achievements)** profile is Modern
  with both off.

rcheevos can't switch single achievements off, so the choice was between this note and stopping all achievements while
any of these is on; both options are on in the Modern profiles, and most gameplay mods are small changes.

`JFG_RA_TEST_BANNER=1` shows two sample banners and a sample indicator (using `icons/64.png`), to check the look.

## How it works

- `src/jfg/jfg_achievements.cpp` runs rcheevos' `rc_client` (lib/rcheevos, v12.5.0, MIT licence).
- The game is identified from the ROM the launcher stored (`jfg-us.z64`), with RetroAchievements' N64 hash.
- Achievements read the game's RDRAM directly. N64Recomp keeps it in the layout RetroAchievements' N64 sets are made
  against (offsets 0-0x7FFFFF, 32-bit words in host byte order, as mupen64plus stores it).
- The set is checked once per video frame, as in emulators: a game frame covers 2-3 video frames at the original frame
  rate and 1 at 60 FPS (the game's `delayDat`), so it is checked that many times per game frame. Timers in the set read
  the game's own counters, and its hit counts ("for 400 frames") assume 60 checks a second.
- Network requests run on their own threads; their replies are handled on the game thread.
- Requests use `jfg_http_request`: Android, the app's Java HTTPS helpers; Windows, WinHTTP; Linux, curl, started
  without a shell, with the request body on its standard input. The password is never on a command line or in a file.
- `lib/rcheevos` is a submodule pinned to the v12.5.0 tag. CMake stops if it is missing; configure with
  `-DJFG_RETROACHIEVEMENTS=OFF` to build without RetroAchievements.
- The port gives the game a larger heap than the N64 had (docs/MEMORY.md). Achievements that follow pointers are
  unaffected; any that read fixed heap addresses could behave differently.
