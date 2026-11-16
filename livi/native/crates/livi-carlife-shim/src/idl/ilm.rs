// Generated from aa-carlife-shim/src/ilm_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const ILM_SERVICE: &str = "SVP_iLMService";
pub const ILM_SERVICE_WAIT_SURFACE: i32 = 0;
pub const ILM_SERVICE_SHOW_SURFACE: i32 = 1;
pub const ILM_SERVICE_SET_SURFACE_POS: i32 = 2;
pub const ILM_SERVICE_SCREENSHOT: i32 = 3;
pub const ILM_SERVICE_METHODS: &[Method] = &[
    Method { id: ILM_SERVICE_WAIT_SURFACE, name: "WaitSurface", sig: "sig<int, unsigned int>" },
    Method { id: ILM_SERVICE_SHOW_SURFACE, name: "ShowSurface", sig: "sig<int, unsigned int, int>" },
    Method { id: ILM_SERVICE_SET_SURFACE_POS, name: "SetSurfacePos", sig: "sig<int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int>" },
    Method { id: ILM_SERVICE_SCREENSHOT, name: "Screenshot", sig: "sig<int, unsigned int, std::string>" },
];
