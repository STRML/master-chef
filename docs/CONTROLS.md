# Controls

Pair a compatible game controller with the Vision Pro before launching.
The labels below use a PlayStation-style controller; equivalent Xbox buttons
follow the same physical positions.

| Input | Action |
| --- | --- |
| Left stick | Move |
| Right stick | Look and aim |
| R2 / right trigger | Fire |
| L2 / left trigger | Throw grenade |
| Square / left face button | Use or reload |
| Cross / bottom face button | Jump / confirm |
| Circle / right face button | Crouch / back |
| Options / menu | Pause |

Use the app's recenter control after choosing a comfortable seated or reclined
position. Aiming remains controller-driven. Menu gaze/pinch and head-pointer
behavior depend on the app settings and available tracking.

## Quest 3 (Touch controllers)

The Quest 3 build reads the OpenXR `oculus/touch_controls` profile
(`native/EngineHost/android/openxr_input.c`) and maps it to the same
actions, using the PlayStation label positions from the table above:

| Input | Action |
| --- | --- |
| Left stick | Move |
| Right stick | Look and aim |
| Right trigger | Fire |
| Left trigger | Throw grenade |
| X (left face) | Use or reload |
| A (bottom face) | Jump / confirm |
| B (right face) | Crouch / back |
| Y (top face) | Melee |
| Left grip | Shoulder (alt) |
| Right grip | Shoulder (alt) |
| Left stick click | Walk |
| Right stick click | Zoom |
| Menu (left controller) | Pause |
| Haptics | Impact on right controller (80 ms) |

The trigger threshold is 0.75 (Meta runtime default). The digital
shoulder view of the trigger and the analog trigger value both reach the
game through the same DirectInput bridge the desktop build uses.
