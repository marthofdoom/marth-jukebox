# marth-jukebox

SKSE plugin that replaces Skyrim's music with tracks from your own [Harmony](https://github.com/marthofdoom/harmony) server, chosen per music environment (genre / artist / playlist).

Status: design phase. See [DESIGN.md](DESIGN.md). Requires a running Harmony server; ships no audio.

Security note: the personal key is sent with every request. Use an `https://` server URL whenever Harmony is reached from outside your LAN or Tailnet; plain `http://` sends the key in clear text.

## 1.0 notes (core)

- "Remember track place": in 1.0 an interrupted track always resumes (that is the game's own behaviour);
  the per-environment checkbox is stored but switching it off has no effect yet.
- Qobuz is never used unless you add `AllowQobuz=1` under `[General]` in `MarthJukebox.ini` (without a
  subscription Qobuz only serves 30 s previews).
- When the Harmony server cannot be reached while music plays from it, a notification appears top-left
  ("Jukebox: Harmony server not reachable - playing saved/vanilla music"), again every 5 minutes, and once
  "Jukebox: Harmony is back". `[General] ToastEnabled=0` switches it off, `ToastIntervalMinutes` (1-60) sets the
  interval. To see it: enter a well-formed but unreachable server address in the menu (for example the right host
  with a wrong port), or stop the server. A malformed address gives no notification (the menu says what is wrong).

## Credits

- CommonLibSSE-NG (its MIT-licensed fork, from marthofdoom/vcpkg-registry), [nlohmann/json](https://github.com/nlohmann/json) (MIT),
  [spdlog](https://github.com/gabime/spdlog) (MIT).
- [dr_flac](https://github.com/mackron/dr_libs) by David Reid (public domain / MIT-0), vendored in `third_party/dr_libs/`.
