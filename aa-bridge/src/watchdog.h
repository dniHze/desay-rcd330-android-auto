// SPDX-License-Identifier: GPL-3.0-or-later
//
// Watchdog for the main loop.
//
// The main loop does everything that can block: every GStreamer state change (the decoder,
// the sink, the media ALSA sink), the session-end teardown of the PulseAudio streams, and the
// feed. procguard restarts the bridge only when it exits, so a bridge stuck in one of those
// calls stays up and useless: no picture or sound until the next reboot (step 3 run 9 showed
// one such block, a pulsesink retry). The loop ticks at least every 250 ms while it serves a
// session; if no tick comes for AA_WATCHDOG_S seconds (default 10; 0 turns it off), a thread
// logs where the loop was and ends the process with exit status 0, which procguard restarts.
// While the loop waits for a feed connection it says so (watchdog_idle), as that wait has no
// limit.
#ifndef AA_BRIDGE_WATCHDOG_H
#define AA_BRIDGE_WATCHDOG_H

namespace aa {

// Starts the watchdog thread (once), with AA_WATCHDOG_S from the environment.
void watchdog_start();

// The main loop is alive and about to do `stage` (a string literal, shown if it never returns).
void watchdog_tick(const char *stage);

// The main loop waits for something that may take any time (a feed connection).
void watchdog_idle();

} // namespace aa

#endif // AA_BRIDGE_WATCHDOG_H
