// SPDX-License-Identifier: GPL-3.0-or-later
//
// Datagrams to the CarLife shim, on its notify socket (AA_SHIM_NOTIFY, default
// /tmp/run/aa/notify.sock, "-" for none). Best effort: nothing listening is fine.
//
//   "open <stream name>"  a PulseAudio stream opened (pa_out.h): the shim registers it with the
//                         unit's audio manager, which gives it the active source's volume.
//   "video"               the video pipeline was rebuilt after an error (video.h) and its first
//                         frame is up: the sink made its ILM surface (20000000 + our pid) anew,
//                         hidden, so the shim shows it again if the AA screen is up.
#ifndef AA_BRIDGE_NOTIFY_H
#define AA_BRIDGE_NOTIFY_H

#include <string>

namespace aa {

void notify_shim(const std::string &msg);

} // namespace aa

#endif // AA_BRIDGE_NOTIFY_H
