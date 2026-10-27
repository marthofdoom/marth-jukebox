# marth-jukebox

SKSE plugin that replaces Skyrim's music with tracks from your own [Harmony](https://github.com/marthofdoom/harmony) server, chosen per music environment (genre / artist / playlist).

Status: design phase. See [DESIGN.md](DESIGN.md). Requires a running Harmony server; ships no audio.

Security note: the personal key is sent with every request. Use an `https://` server URL whenever Harmony is reached from outside your LAN or Tailnet; plain `http://` sends the key in clear text.
