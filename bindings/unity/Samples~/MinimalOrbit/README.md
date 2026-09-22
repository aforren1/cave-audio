# Minimal orbit

The smallest realistic BwAudio client: a click orbits the listener's head at ear height for six
seconds, then stops. Every binding of the engine has a copy of this demo and they all play the
same stimulus, so you can compare them by ear.

## Set it up

1. Make a GameObject named `BwAudio` and add the `Engine` component. Set **profile** to
   `Binaural` and **sink** to `Auto`.
2. Make a GameObject for the head, put it where the listener stands, and drag it into the
   Engine's **listener** field. The XR camera works too.
3. Make a third GameObject, add `PushEmitter` and `MinimalOrbit` to it, and press Play.

You need no audio file. `MinimalOrbit` synthesizes the stimulus and feeds it to the push voice.

## What it shows

`Engine` pushes every source transform and commits once per frame, so a client moves a transform
and feeds a voice. There is no commit call in the sample and there should not be one.

The orbit goes through `Room.FromRoom` instead of writing Unity coordinates directly. Unity is
left-handed, so the engine's +X (the listener's left) is Unity's -X. A trajectory written in Unity
coordinates mirrors, and a mirrored orbit still sounds like an orbit, which is why this is worth
one line of code.

## The stimulus

One 250 ms period: a 2 ms Hann-windowed noise burst at -12 dBFS peak, then silence. Looped, that
is four clicks a second. The click is broadband because the cues that place a source in elevation
and front to back live in the spectrum. A pure tone carries almost none of them, and an orbit made
with one reads as a level pan.
