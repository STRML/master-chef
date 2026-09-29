# Architecture

The generation tools decode the supported user-owned x86 executable and emit
C translation units. `EngineReuse` implements the guest register, floating
point, memory, and dispatch model. `EngineHost` implements the APIs used by
the translated program, including graphics, sound, input, files, and threading.

The graphics bridge translates the game's shader programs for Metal and
renders several views around the player. The visionOS layer publishes those
views to an immersive presenter, with a separate interface layer and a stereo
forward view. Optional views are scheduled to bound rendering cost.

The public repository includes the translator and numeric function-entry
lists. Whole-executable generated C, original executable data, and compiled
engine objects are produced locally and excluded from source distribution.

The source-only tests use synthetic fixtures or small numerical regression
traces. Tests that require locally translated functions or original map data
explicitly skip without their inputs. Those tests must not be treated as a
full original-game or headset acceptance run.
