// PickupWeaverDSP_AVX.cpp -- optional runtime-dispatched AVX engine for x86 builds whose baseline is SSE2.
//
// Compile THIS FILE ONLY with AVX enabled (GCC/Clang: -mavx ; MSVC: /arch:AVX), and define
// PW_ENABLE_AVX_DISPATCH for the translation units that include PickupWeaverDSP.h. The front end then
// uses the AVX engine when the CPU and OS support it (checked once in prepare()), SSE2 otherwise.
// All inline code here lives in namespace pw::arch_avx, so no AVX-encoded copy of a shared inline
// function can replace the baseline one at link time.
#if !defined(__AVX__)
#error "PickupWeaverDSP_AVX.cpp must be compiled with AVX enabled (-mavx or /arch:AVX)"
#endif
#define PW_BUILDING_AVX_TU 1
#include "PickupWeaverDSP.h"

namespace pw {
detail::EngineBase* makeEngineAVX() { return new arch_avx::Engine<arch_avx::simd::VAVX>(); }
}
