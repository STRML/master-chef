# Known limitations

- Frame rate can fall in combat and busy rooms, including The Maw. A stable
  campaign-wide FPS target has not been established.
- Panorama seams, stale peripheral views, texture flashing, and some lighting
  differences remain under investigation.
- Audio interruptions have been reported. Synthetic mixer checks do not
  establish uninterrupted audible playback on a headset.
- The presenter uses a stereo forward view; the entire surrounding panorama
  is not full stereo.
- Complete campaign playthroughs, all checkpoints, extended sessions, and
  all controller/headset combinations have not been qualified.
- The supported executable is fixed by hash. Replacing it with another
  edition or patch can invalidate translation addresses and runtime hooks.
- Source checks and desktop GPU tests are separate from headset testing.
  An unsigned successful build is separate from provisioning and installation.

When reporting a problem, include the source version, app build identifier,
visionOS version, mission/checkpoint, reproduction steps, and whether it
occurs while stationary, turning the controller, or moving your head. Attach
only reviewed excerpts of diagnostics; omit game files and private identifiers.
