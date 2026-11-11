# Changelog

## 1.5.0
- English and Spanish. *Shortcuts > Language* switches between them; until you pick one, it follows Windows.
- New icon, with a simpler version for the tray and other small sizes.

## 1.4.3
- *View > Shape* separates one row / two rows from curved / flat, so two flat rows are possible too.

## 1.4.2
- A *?* button next to *Screens > GPU* explains when to change it.
- Only redraws when something moves or changes on the screens: with everything still, the GPU almost rests.

## 1.4.1
- After switching GPUs, the screens land in the right place the first time (Windows takes a moment to have them
  ready).
- With the screens on one GPU and the glasses on another, the view no longer drops to half the refresh rate.

## 1.4.0
- *Screens > GPU*: choose which GPU draws the virtual screens. On laptops with two GPUs, putting them on the
  integrated one keeps Windows animations from going back and forth between them.
- Home shows which GPU the screens are on.

## 1.3.1
- Every 30 s the log writes a `perf:` line (frames, frame times, late frames, cost of drawing and copying,
  sensor), and every visible hitch as `slow frame`.
- The old log is kept as `log-old.txt` instead of being deleted.

## 1.3.0
- Switching between one and two rows during a session rearranges the screens in Windows too.
- Pushing the mouse down on any screen of the bottom row takes it to the laptop.
- New shortcut Ctrl+Alt+P: mouse to the laptop screen.

## 1.2.2
- The "No data from the glasses' sensor" warning no longer shows up by mistake right after calibrating.

## 1.2.1
- On start, a message in the glasses reminds you they have to be in Follow mode.
- The log records where Windows puts each screen and, in mouse mode, which one the mouse is on.

## 1.2.0
- *View > Up and down*: with one row the screens move up and down with you; with two rows they stay fixed
  (needs calibration). A warning in the glasses if the calibration is missing.

## 1.1.1
- Calibration notices if you turned left instead of right.
- Clearer texts about what calibrating changes.

## 1.1.0
- Draws on the same GPU as the virtual screens (laptops with two GPUs).
- Less latency: no queued frames, smoothing only while the head is still, and *Predict turns*.
- *View > Sharpness*.
- Captures from another GPU no longer hold up drawing.

## 1.0.0
- First version.
