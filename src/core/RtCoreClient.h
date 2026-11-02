#pragma once
// UNOFFICIAL 2026-10-28 (NVR DXVK, RT reflections; docs/next/FORK-RT-REFLECTIONS.md): the two things the RT reflections need from the
// shared RT core (core/RtCore.h, docs/next/FORK-RT-CORE.md) beyond its user API, in one place:
// - Ready(): the helper is up (RT on this GPU for 64-bit programs; never on a GTX 10xx: the core says so and stays off);
// - Exchange(): a second exchange of the frame for one user, at the point that user calls it. Reflections makes its rays in the post
//   chain (RayGen, after the material buffer and the screen march) and reads the hits right after (Resolve); the core's own exchange
//   (RtCore::Frame) is earlier, before the sun shadows. Requested from rt-sun-core 2026-10-28 (RtCore::Exchange); until RtCore.h
//   defines NVR_RTCORE_EXCHANGE it answers false, and Reflections draws its normal trace.
#include "RtCore.h"

namespace RtCoreClient {
	inline bool Ready() { return RtCore::Running(); }
	inline bool Exchange(IDirect3DDevice9* Device, const RtCore::User& user) {
#ifdef NVR_RTCORE_EXCHANGE
		return RtCore::Exchange(Device, user);
#else
		(void)Device; (void)user; return false;
#endif
	}
}
