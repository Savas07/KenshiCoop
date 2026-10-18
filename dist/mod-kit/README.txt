KenshiCoop v0.57.2 - co-op mod
==============================

This zip contains ONE folder: "KenshiCoop". That folder IS the mod.

INSTALL (both players)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it.
  2. Copy the "KenshiCoop" folder into your Kenshi mods folder:
       <Kenshi>\mods\
     so you end up with:
       <Kenshi>\mods\KenshiCoop\KenshiCoop.dll   (and the other files)
     The default Steam path is:
       C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\
  3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

PREREQUISITES (both players)
----------------------------
  1. Kenshi 1.0.65 (Steam). If Steam has 1.0.68, the RE_Kenshi installer
     creates the 1.0.65 copy for you.
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. For the Steam transport (recommended): Steam RUNNING and ONLINE on both
     machines. No port forwarding, no IPs, no config editing, no Steam IDs to
     swap - you invite your friend in-game (see PLAY below).

PLAY (Steam - recommended)
--------------------------
  Kenshi's MAIN MENU has a MULTIPLAYER button: it opens the co-op lobby. Up to
  8 players (host + 7 friends); everyone needs this same release. Each player
  drives one squad tab (host = tab 1, first friend = tab 2, ...), named after
  that player's Steam name. The save remembers who had which squad, so next
  time everyone gets their own squad back whatever order you join in. If
  someone new comes instead of a regular, they take the absent player's squad.
  1. HOST: main menu > MULTIPLAYER > HOST GAME. The lobby lists everyone in it
     (Steam name + avatar) and your online Steam friends (friends already in
     Kenshi first; < and > page through). Click INVITE next to each friend -
     they get a Steam invite and show up under PLAYERS when they accept. Then:
       NEW GAME  - Kenshi's own New Game window opens with the
                   "Multiplayer (Wanderer xN)" start for your lobby size already
                   selected (any other start can still be picked), or
       LOAD GAME - your saves are listed; click LOAD next to the one to play.
     Friends wait in the lobby until your world is running (after your own
     character creation on a new game), then join it automatically.
  2. JOIN: accept the Steam invite, or main menu > MULTIPLAYER > JOIN next to a
     friend who is hosting, or "Join Game" on the host in your Steam friends
     list. The lobby opens and shows who is in it. If Kenshi was closed when you
     accepted, Steam starts it and you join once the main menu is up. The host
     sends its world to you and you load right into it. (You do NOT need the
     host's save beforehand. An identical copy already on disk is used as-is.)
  3. In-game, F2 opens the co-op panel: status, Invite more friends, and
     "Leave co-op session". A status banner in the TOP-LEFT corner shows the
     session state at the main menu as well as in-game.
  Good to know:
   * New game: each joining player designs their character in Kenshi's own
     character editor right after loading in (about 8 s after their world
     appears; the game stays paused for everyone until they confirm). Loading
     a save never opens it - established characters are never re-rolled.
   * When a friend joins a new game, the host's world is first saved as
     "KenshiCoop YYYY-MM-DD HHMM" - it never overwrites an existing save.
   * Squad tabs you renamed yourself keep your name.
   * Before playing, check that the lobby window's top-right corner shows the
     SAME version for everyone. Steam does not always fetch a Workshop update
     right away: restart Steam, or unsubscribe and subscribe again.

PLAY (Advanced - manual Steam ID or LAN / direct UDP)
-----------------------------------------------------
  Click "Advanced" in the F2 panel for the manual controls: Role / Transport /
  Connection toggles plus "Copy my Steam ID" / "Paste friend's Steam ID" (each
  player copies theirs, sends it, and pastes the other's; per-session, nothing
  is written to disk). For LAN / direct UDP: open
  <Kenshi>\mods\KenshiCoop\coop_config.json in Notepad, set "transport": "udp",
  and put the host's address in "ip" (and "port" if you changed it). In the
  Advanced view set Transport: UDP, pick Host/Join, and toggle Connection to
  ONLINE. ip/port are re-read whenever you go ONLINE, so no restart is needed
  after an edit.

UNINSTALL
---------
  Delete <Kenshi>\mods\KenshiCoop. Nothing else is touched.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (Steam): both Steams must be RUNNING and ONLINE. A friend is
    listed under FRIENDS HOSTING in the lobby (and "Join a friend" in F2) only
    while they are hosting (HOST GAME / "Host co-op") and Steam shows them in
    Kenshi; the lists refresh every few seconds. "different mod version" next
    to a host means you run different KenshiCoop releases. With the Advanced
    manual flow, each side must have Pasted the OTHER player's ID. Look for
    '[invite]' / '[lobby]' lines and '[steam] session ... active=1' in
    <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": one player has an older/newer build; both should use
    the same release.
  * Sending logs: every player's log is <Kenshi>\KenshiCoop_host.log (joining
    players too - their lines switch to [JOIN] once they join a friend).
