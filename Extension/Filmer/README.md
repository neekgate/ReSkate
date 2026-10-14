# Filmer

skate. filmed the way a skater with a VX1000 films a line.

## Modes

- **AI filmer** (`filmer mode follow`): a virtual filmer rides with a skater and films them while
  you play normally. It rides the trail the skater left, checking every step against the game's
  own collision: real ground under it, no walls, nothing a board would not roll over. It stays at
  the level it rides at: up banks and slopes with you, but alongside a ledge or rail rather than up
  on it. Where you came down from its level in one go (a set, a drop) or jumped a gap, it stops at
  the edge, films you land and ride away, and cuts back in behind you once you are 25 m off. This
  goes by your trail, so it works even where the game's collision gives no answer. It pushes to keep up, picks angles through a line (Auto), keeps your board in frame (Tilt),
  and points the camera like a person: a beat late, a loose frame, a hand that is never still, but
  it whips back before a trick leaves the frame. It keeps to whichever side you choose (Side: Auto,
  Left, Right, or a bindable swap). Film yourself, or anyone in the session
  (`filmer subject <name>`). Click the left stick to have it stop and film from where it is (again
  to ride on); click the right stick to slap the lens.
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

Slap the lens: a hand rushes at it and smacks it, the camera is knocked back, the iris shuts while
the palm is on the glass and overshoots as it comes off, and the clip ends. Clips are numbered and logged with their end time in
`%LOCALAPPDATA%\ReSkate\filmer\clips.csv`, so they can be found in a recording.

- Click the right stick (filming, or following with Stick clicks on).
- Any time: bind **Filmer: slap the lens** in Settings > Binds, or `filmer slap`.

**Filmer on / off** and **Filmer: stop and film / ride again** can be bound too.

The date and the viewfinder text are drawn by the shader in a 5x7 camcorder font, burnt in after the
lens (straight, over the hood) and before the tape (softened, interlaced, grainy).

The tape can be worn (Tape age): dropouts, head-switching noise along the bottom, a tracking band
rolling up the picture as each clip starts, and reds smearing to the right. The auto-iris lags the
light: into the sun the picture blows out for a moment, into shade it goes murky, then settles. With
the fisheye the AI filmer lunges in close and low under an air, then pulls back for the landing.
Each of these has its own switch.

**Over any camera** draws the look whatever has the camera, including the replay editor's. The
Filmer's own camera does not run in replays yet.

## Commands

| Command | |
| --- | --- |
| `filmer toggle [follow\|film]` | On (as last used) or off |
| `filmer mode off\|follow\|film` | |
| `filmer subject me\|<player>` | Whom the AI filmer films |
| `filmer target auto\|<player>` | Whom the assist frames while you film |
| `filmer set <option> <value>` | `style auto\|behind\|side\|ahead`, `lens fisheye\|long`, `distance`, `height`, `tilt`, `human`, `reaction`, `sensitivity`, `hold`, `bend`, `age`; switches `assist`, `invert`, `recenter`, `hidehud`, `stick`, `clicks`, `drops`, `lunge`, `look`, `anywhere`, `dropouts`, `headswitch`, `tracking`, `smear`, `iris`, `fisheye`, `bars`, `hood`, `grade`, `soft`, `scanlines`, `grain`, `date`, `viewfinder` |
| `filmer plant [on\|off]` | The AI filmer stops and films from where it is, or rides again |
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
