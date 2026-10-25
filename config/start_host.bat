@echo off
rem Starts the co-op test copy as host over Steam. Steam must be running.
rem Player 1 (you) = Game controller 1 = your real controller (XInput pad 0).
rem Player 2 (friend) = Game controller 2 = virtual pad 1, fed by the friend's controller over Steam.
rem Virtual pad 1 is reported as connected from the start, so Options > Button settings can assign it.
set SteamAppId=254460
set SteamGameId=254460
set OBSCOOP_ROLE=host
set OBSCOOP_NET=steam
set OBSCOOP_REMOTE_PAD=1
set OBSCOOP_VPADS=1
rem Supersampling: 1 = off, 2 = the game renders at 2x width and height and shrinks it (sharper,
rem smoother edges; menu text shows smaller), 4 = 4x. Fullscreen becomes a borderless window.
set OBSCOOP_SSAA=2
rem Texture filtering: 0 = the game's own, 2..16 = anisotropic (sharper surfaces at an angle).
set OBSCOOP_ANISO=16
cd /d "%~dp0"
start "" Obscure.exe
