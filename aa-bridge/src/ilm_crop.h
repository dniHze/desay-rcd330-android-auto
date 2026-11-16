// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The video surface's crop for the CarLife shim: see ilm_crop.cpp.
// `aa-bridge --ilm-crop <id> <source> <destination>`: one call, then exit.
int ilm_crop_main(int argc, char** argv);
// The running bridge's crop socket (AA_BRIDGE_CTL), on a thread of its own.
void ilm_crop_serve();
