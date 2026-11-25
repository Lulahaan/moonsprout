// Write-gather pipe (GXWGFifo) for host builds. GameCube code writes GX commands straight to the
// FIFO address 0xCC008000; on the host those writes go to Aurora's command FIFO instead.
#include "../../extern/aurora/lib/gx/fifo.hpp"

#include <cstdint>

extern "C" {
void p2_wgpipe_u8(uint8_t v) { aurora::gx::fifo::write_u8(v); }
void p2_wgpipe_u16(uint16_t v) { aurora::gx::fifo::write_u16(v); }
void p2_wgpipe_u32(uint32_t v) { aurora::gx::fifo::write_u32(v); }
void p2_wgpipe_f32(float v) { aurora::gx::fifo::write_f32(v); }
}
