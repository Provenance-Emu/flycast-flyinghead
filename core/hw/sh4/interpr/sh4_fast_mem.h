#pragma once
#include "types.h"

/// Fast Memory Access for SH4 Interpreter
/// These bypass expensive cycle calculations by using simplified estimates

// Forward declarations
u8 FastReadMem8_Interp(u32 addr);
u16 FastReadMem16_Interp(u32 addr);
u32 FastReadMem32_Interp(u32 addr);
u64 FastReadMem64_Interp(u32 addr);
void FastWriteMem8_Interp(u32 addr, u8 data);
void FastWriteMem16_Interp(u32 addr, u16 data);
void FastWriteMem32_Interp(u32 addr, u32 data);
void FastWriteMem64_Interp(u32 addr, u64 data);
s32 FastReadMemS8_Interp(u32 addr);
s32 FastReadMemS16_Interp(u32 addr);

/// Drop-in replacements for expensive memory macros used in sh4_opcodes.cpp
/// These use the current memory handlers but add simplified cycle estimates

// Simplified cycle estimates for memory operations
static constexpr u8 FAST_READ_CYCLES = 2;
static constexpr u8 FAST_WRITE_CYCLES = 2;

// Fast memory access macros with simplified cycle accounting
#define FAST_READ_MEM_U32(to, addr) \
    do { \
        to = FastReadMem32_Interp(addr); \
    } while(0)

#define FAST_READ_MEM_S32(to, addr) \
    do { \
        to = (s32)FastReadMem32_Interp(addr); \
    } while(0)

#define FAST_READ_MEM_S16(to, addr) \
    do { \
        to = FastReadMemS16_Interp(addr); \
    } while(0)

#define FAST_READ_MEM_S8(to, addr) \
    do { \
        to = FastReadMemS8_Interp(addr); \
    } while(0)

#define FAST_WRITE_MEM_U32(addr, data) \
    do { \
        FastWriteMem32_Interp(addr, (u32)data); \
    } while(0)

#define FAST_WRITE_MEM_U16(addr, data) \
    do { \
        FastWriteMem16_Interp(addr, (u16)data); \
    } while(0)

#define FAST_WRITE_MEM_U8(addr, data) \
    do { \
        FastWriteMem8_Interp(addr, (u8)data); \
    } while(0)
