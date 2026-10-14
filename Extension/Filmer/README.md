# Filmer

skate. filmed the way a skater with a VX1000 films a line.

## Modes

- **AI filmer** (`filmer mode follow`): a virtual filmer rides with a skater and films them while
  you play normally. It rides the trail the skater left (ground they just skated, so it rarely
  clips into walls), rolls down a set after them, pushes to keep up, picks angles through a line
  (Auto), and points the camera like a person: a beat late, a loose frame, a hand that is never
  still, but it whips back before a trick leaves the frame. Film yourself, or anyone in the
  session (`filmer subject <name>`).
- **You film** (`filmer mode film`): your skater is the filmer. The left stick rides; the right
  stick points the camera (the game stops seeing it, so no flicks while you film). The aim assist
  keeps a friend in frame (the closest one ahead, or `filmer target <name>`) and the stick shifts
  the framing. Click the right stick to slap the lens.

## The look

Drawn over the game's frame by the overlay while the Filmer has the camera, one switch each:
the Century MK1's fisheye bend and its dark hood edge, 4:3, the VX's colour, standard-definition
softness with the camcorder's sharpening halo and colour bleed, interlacing, tape grain, a date
stamp and the viewfinder's REC. HDR displays get the geometry (bend, 4:3, hood, interlacing); the
colour and grain are SDR only. The game's HUD is hidden while filming (switchable).

## Marking clips

Slap the lens: a hand covers it, the iris closes and overshoots as it comes off, the camera jolts,
and the clip ends. Clips are numbered and logged with their end time in
`%LOCALAPPDATA%\ReSkate\filmer\clips.csv`, so they can be found in a recording.

- While you film: click the right stick.
- Any time: bind **Filmer: slap the lens** in Settings > Binds, or `filmer slap`.

**Filmer on / off** can be bound too.

## Commands

| Command | |
| --- | --- |
| `filmer toggle [follow\|film]` | On (as last used) or off |
| `filmer mode off\|follow\|film` | |
| `filmer subject me\|<player>` | Whom the AI filmer films |
| `filmer target auto\|<player>` | Whom the assist frames while you film |
| `filmer set <option> <value>` | `style auto\|behind\|side\|ahead`, `lens fisheye\|long`, `distance`, `height`, `human`, `reaction`, `sensitivity`, `hold`, `bend`; switches `assist`, `invert`, `recenter`, `hidehud`, `stick`, `look`, `fisheye`, `bars`, `hood`, `grade`, `soft`, `scanlines`, `grain`, `date`, `viewfinder` |
| `filmer slap` | End the clip |
| `filmer clips` | This session's clips |
| `filmer status`, `filmer reset` | |

Settings are kept in `%LOCALAPPDATA%\ReSkate\filmer\filmer.json`.

## Limits

- The camera takes over the native free camera, so the Filmer, Freecam, Noclip, First person and
  Spectate take turns.
- The right stick is kept from the game through XInput. A PlayStation pad read by the game
  directly (no Steam Input or DS4Windows) still flicks while you film; with Steam Input it is fine.
- Nametags are placed for the unbent picture, so they sit a little off with the fisheye bend on.

## Files

| File | |
| --- | --- |
| `filmer_rig.*` | The operator: where they ride and where they point. Pure maths. |
| `filmer.cpp` | The game side: skaters, the camera lease, the right stick, settings, clips. |
| `filmer_commands.cpp` | The `filmer` console commands. |
| `filmer_view.cpp` | The snapshot the overlay reads (part of the overlay library). |
| `filmer_look.*` | The VX look: a copy of the frame drawn back through `filmer_look.hlsl`. |
| `filmer_page.cpp` | The FILMER menu page. |
