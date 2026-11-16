// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP iLM service (svp/bin/ilmservice, registry svp.svc.ilm): the platform's window
// manager front end over the GENIVI ILM. Recovered from the stock client svp/lib/libilm.so
// (SVPiLMService_Client, RcfClient<SVP_iLMService>): interface name "SVP_iLMService", TCP
// 30005, and the method IDs and types below, which build.sh checks against tools/stock_ids.txt.
//
// Stock carlifeservice calls ShowSurface(getpid() + 20000000, 1) on Show(1) and (…, 0) on
// Show(0): the service sets visibility AND opacity and places video surfaces itself
// (SVPiLMMgr::ShowSurface_private). WaitSurface blocks until the surface exists.
#ifndef AA_CARLIFE_SHIM_ILM_IDL_HPP
#define AA_CARLIFE_SHIM_ILM_IDL_HPP

#include <string>

#include <RCF/Idl.hpp>
#include <SF/string.hpp>

RCF_BEGIN(SVP_iLMService, "SVP_iLMService")
    RCF_METHOD_R1(int, WaitSurface, unsigned int)                                  // 0
    RCF_METHOD_R2(int, ShowSurface, unsigned int, int)                             // 1
    RCF_METHOD_R5(int, SetSurfacePos, unsigned int, unsigned int, unsigned int,
                  unsigned int, unsigned int)                                      // 2
    RCF_METHOD_R2(int, Screenshot, unsigned int, std::string)                      // 3
RCF_END(SVP_iLMService)

const int kILMServicePort = 30005;

// The ILM surface id a process's video gets: the unit's waylandsink creates it with
// ilm_surfaceCreate(getpid() + 20000000), the same convention stock carlifeservice uses.
const unsigned int kILMVideoSurfaceBase = 20000000u;

#endif // AA_CARLIFE_SHIM_ILM_IDL_HPP
