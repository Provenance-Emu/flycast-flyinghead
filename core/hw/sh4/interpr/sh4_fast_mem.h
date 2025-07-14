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

/// Simplified cycle mode - bypass expensive instruction cycle calculations
/// Enable/disable with global flag for testing
extern bool g_simplified_cycles_enabled;

/// Branch prediction - cache recent branch targets for FMV performance
extern bool g_branch_prediction_enabled;

/// Branch Target Cache Entry
struct BranchCacheEntry {
    u32 pc;           // Branch instruction PC
    u32 target;       // Target address
    bool taken;       // Last taken state
    u8 confidence;    // Prediction confidence (0-255)
    u8 pattern;       // Recent taken pattern (last 8 branches)
};

/// Fast branch target prediction cache
extern BranchCacheEntry g_branch_cache[64]; // 64 entries, cache-line aligned
extern u32 g_branch_cache_hits;
extern u32 g_branch_cache_misses;

/// Fast branch prediction functions
u32 PredictBranchTarget(u32 pc, u16 op, bool condition_flag);
void UpdateBranchPrediction(u32 pc, u32 actual_target, bool taken);
bool IsBranchInstruction(u16 op);

/// Instruction Fusion - execute common 2-instruction patterns as single operations
extern bool g_instruction_fusion_enabled;

/// Fused instruction types for common FMV patterns
enum FusedInstructionType {
    FUSED_NONE = 0,
    FUSED_MOV_ADD_IMM,      // mov Rm,Rn + add #imm,Rn
    FUSED_LOAD_ADD,         // mov.l @Rm,Rn + add Rx,Rn
    FUSED_LOAD_SUB,         // mov.l @Rm,Rn + sub Rx,Rn
    FUSED_ADD_STORE,        // add Rm,Rn + mov.l Rn,@Rx
    FUSED_SUB_STORE,        // sub Rm,Rn + mov.l Rn,@Rx
    FUSED_CMP_BRANCH,       // cmp/eq Rm,Rn + bt/bf target
    FUSED_SHIFT_ADD,        // shll2/shll8 Rn + add Rm,Rn
    FUSED_SHIFT_SUB,        // shll2/shll8 Rn + sub Rm,Rn
    FUSED_COUNT
};

/// Instruction fusion cache for pattern detection
struct FusionCacheEntry {
    u32 pc1;                // First instruction PC
    u16 op1, op2;          // Instruction pair
    FusedInstructionType type; // Fusion type
    u8 confidence;         // Fusion confidence (0-255)
    u32 hit_count;         // Number of times this pattern executed
};

extern FusionCacheEntry g_fusion_cache[32]; // Small cache for hot patterns
extern u32 g_fusion_hits, g_fusion_misses;

/// Instruction fusion functions
FusedInstructionType DetectFusionPattern(u16 op1, u16 op2);
bool ExecuteFusedInstruction(FusedInstructionType type, u16 op1, u16 op2, Sh4Context* ctx);
void UpdateFusionCache(u32 pc, u16 op1, u16 op2, FusedInstructionType type);

/// Hot Path Specialization - optimize common instruction sequences
extern bool g_hot_path_specialization_enabled;

/// Hot path pattern types for common sequences
enum HotPathPatternType {
    HOT_PATH_NONE = 0,
    HOT_PATH_MOV_IMM_ADD_IMM,     // mov #imm,Rn + add #imm2,Rn
    HOT_PATH_DUAL_MOV,            // mov Rm,Rn + mov Rx,Ry
    HOT_PATH_LOOP_COUNTER,        // add #1,Rn + cmp/eq #val,Rn + bt/bf
    HOT_PATH_COUNT
};

/// Hot path specialization functions
bool ExecuteHotPathPattern(u32 pc, u16* ops, u8 length, Sh4Context* ctx);
void UpdateHotPathCache(u32 pc, u16* ops, u8 length);
bool CheckHotPathCache(u32 pc, u16* ops, u8 length, Sh4Context* ctx);

/// Fast cycle calculation with simple pattern-based estimates
inline u8 FastCalculateInstructionCycles(u16 op) {
    // Simple pattern-based cycle estimates for common operations
    switch (op & 0xF000) {
        case 0x6000: // MOV operations
            if ((op & 0x000F) <= 0x0003) return 2; // Memory moves
            return 1; // Register moves
        case 0x2000: // Store operations
            return 2;
        case 0x8000: // Conditional branches
        case 0xA000: // Unconditional branches
        case 0xB000: // BSR/JSR
            return 2;
        case 0xF000: // Floating point
            return 3;
        case 0x4000: // Complex operations
            if ((op & 0x00FF) >= 0x20 && (op & 0x00FF) <= 0x2B) return 3; // MUL/MAC
            return 2;
        default:
            return 1; // ALU, immediate ops
    }
}

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
