KenshiCoop - co-op mod
======================

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
  The Co-op panel opens with F2, at the MAIN MENU as well as in-game. Up to 8
  players (host + 7 friends); everyone needs this same release. Each player
  drives one squad tab in join order (host = tab 1, first friend = tab 2, ...):
  start a new game with "Multiplayer (Wanderer xN)" for your group size (x2-x8).
  1. HOST: load the save you want to play (or start a new game), press F2 and
     click "Host co-op". Your online Steam friends are listed (friends already
     in Kenshi first, six per page - "More friends" pages on). Click "Invite"
     next to your friend's name: they get a Steam invite.
  2. JOIN: straight from the MAIN MENU - no save needed. Either accept the
     Steam invite, or press F2 and click "Join" next to the host's name under
     "Join a friend", or use "Join Game" on the host in your Steam friends
     list. If Kenshi was closed when you accepted, Steam starts it and you join
     once the main menu is up. The host sends its world to you on connect and
     you load right into it. (You do NOT need the host's save beforehand. If
     you already have an identical copy on disk it is used as-is instead of
     transferring.)
  3. The status line in the panel shows live state, and a status banner in the
     TOP-LEFT corner shows it too - at the main menu as well as in-game, so a
     joining player can watch the transfer before the world loads. Click
     "Leave co-op session" (or "Stop hosting" before anyone joined) to leave.

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
    listed under "Join a friend" only while they are hosting ("Host co-op")
    and Steam shows them in Kenshi; the list refreshes every few seconds while
    the panel is open. "different mod version" next to a host means you run
    different KenshiCoop releases. With the Advanced manual flow, each side
    must have Pasted the OTHER player's ID. Look for '[invite]' lines and
    '[steam] session ... active=1' in <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": one player has an older/newer build; both should use
    the same release.
