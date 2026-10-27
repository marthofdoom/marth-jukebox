# Nexus Mods compliance analysis (2026-10-27)

Source: Nexus *File Submission Guidelines* (help.nexusmods.com/article/28) and *Moderation Policy*
(article 27), read 2026-10-27. This is an engineering reading, **not** a staff ruling; the Terms of
Service were not read. Nexus staff decide, and "staff may place files in moderation for any reason".

## Verdict

No rule is clearly violated. Four items need action before upload; one is a judgement call that could
get the page moderated.

| # | Rule | Our situation | Risk | Action |
|---|------|---------------|------|--------|
| 1 | "Executables or files that connect to the internet are prohibited unless crucial to the mod's function. Auto-update does not qualify. Contact staff with a justification and the source code." | The plugin's whole function is fetching audio over HTTP. | **Needs staff contact** | Write to staff *before* upload: justification, public MIT source link. Keep the network surface minimal (below). |
| 2 | "Content enabling illegal activity, including piracy and copyright infringement, is prohibited." / "Links to external sources for impermissible files are prohibited." | We ship no audio. But Harmony's default `sources=all` includes YouTube Music via yt-dlp, which violates YouTube's ToS and can be read as ripping. | **Highest** | Plugin defaults to `sources: "library"` (user's own files, plus Qobuz if they have it); YouTube is an explicit opt-in setting. The page describes a client for *your own music server*; it does not advertise YouTube sourcing, link to music sources, or ship search queries pointing at copyrighted works. |
| 3 | Generative-AI tagging: AI-generated **code** needs a tag; "AI Assisted" needs evidence (VCS history, understanding of the code); undisclosed use is moderated. | Most code is written by Sonnet agents under Opus review. | **Must disclose** | Tag **AI-Generated Content** (honest default); tag **AI Media** for any generated banner/logo. Commit history stays public. Do not claim "AI Assisted" unless the evidence test is met. |
| 4 | Requirements and caveats must be disclosed; collected user info must serve the function and be clear. | Needs a Harmony server, SKSE, Address Library, the settings framework; stores a server URL and a personal key in plain text. | Low if stated | Requirements section names all of it. State that the key is stored in plain text in the INI and is sent only to the user-configured server. |
| 5 | "User content may not advertise other websites or services." | Harmony is the user's own required software. | Low | Mention it as a requirement with one link; no promotion language. |
| 6 | Claims must be truthful and supported (performance/FPS). | Background downloads, up to ~1.5 GB cache. | Low | Do not claim "no performance impact"; state disk use, the cache cap setting and that IO is off the game thread. |
| 7 | Malicious files / virus scan; failing scans means moderation. | Unsigned DLL doing HTTP and file writes can trip heuristics. | Low-med | No packing or obfuscation, plain WinHTTP, public source; keep file writes inside `Data/SKSE/Plugins/` and `Data/Music/Harmony/`. |
| 8 | No donation/endorsement soliciting. | n/a | None | Keep the page free of "please endorse" and donation asks. |
| 9 | Copyrighted game files; others' content needs permission. | We ship an ESP of our own records and a DLL; no Bethesda assets. | None | Do not bundle vanilla music or extracted game files. List third-party libs with licences (CommonLibSSE-NG MIT fork, ImGui MIT, nlohmann-json MIT). |
| 10 | Files must be functional, no placeholders; public. | Page goes up only when milestones work. | None | No early "reserve the name" page. |

## Network surface (the budget the staff request is built on)

- One destination: the user-configured Harmony base URL. **Empty by default**; the plugin does nothing on
  the network until the player enters it.
- No telemetry, no update check, no analytics, no third-party hosts, no DNS beyond that host.
- Requests: `GET /api/game/status`, `PUT /api/game/profiles/*`, `POST /api/game/next`,
  `GET /api/game/audio/*`, `POST /api/game/playing`, and `GET /api/search` for the in-game picker.
- Credentials: only the user's own personal key, sent as `X-Harmony-Key` to that URL.
- All of the above documented on the mod page and in `README.md`.

## Open judgement calls (for marth)

- Default `sources`: `library` (safest for Nexus) vs `all` (works with zero accounts, which is the
  Harmony design goal). Recommendation: **library by default, one clear opt-in for streaming**.
- Whether to tag AI-Generated Content. Recommended for honesty; the rule makes undisclosed use a
  moderation matter, and the tag cannot be avoided if the code is mostly agent-written.
