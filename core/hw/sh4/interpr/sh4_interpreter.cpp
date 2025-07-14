/*
	Highly optimized SH4 interpreter with instruction caching and adaptive execution
	Optimized specifically for performance on platforms without JIT capabilities
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "../sh4_cache.h"
#include "debug/gdb_server.h"
#include "../sh4_cycles.h"
#include "build.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/addrspace.h"
#include "sh4_fast_mem.h"

#include <array>
#include <algorithm>

Sh4ICache icache;
Sh4OCache ocache;
Sh4Interpreter *Sh4Interpreter::Instance;

/// Fast instruction fetch using the fast MMU path when available
static inline u16 FastIReadMem16(u32 vaddr)
{
	if (vaddr & 1)
		// Alignment check
		mmu_raise_exception(MmuError::BADADDR, vaddr, MMU_TT_IREAD);

	if (!mmu_enabled()) {
		// No MMU, direct read
		return addrspace::read16(vaddr);
	}

#ifdef FAST_MMU
	// Fast MMU path - inline optimization
	u32 paddr;
	MmuError rv = mmu_instruction_translation(vaddr, paddr);
	if (rv != MmuError::NONE)
		mmu_raise_exception(rv, vaddr, MMU_TT_IREAD);
	return addrspace::read16(paddr);
#else
	// Fall back to the slow path for non-fast MMU builds
	return mmu_IReadMem16(vaddr);
#endif
}

// === OPTIMIZED INSTRUCTION CACHE ===
#define ICACHE_SIZE 512
#define ICACHE_MASK (ICACHE_SIZE - 1)

/// Cache-line aligned structure for optimal performance
struct alignas(64) OptimizedInstructionCache {
	// Separate arrays for better cache locality
	alignas(64) std::array<u32, ICACHE_SIZE> pc;
	alignas(64) std::array<u16, ICACHE_SIZE> opcode;
	alignas(64) std::array<u32, ICACHE_SIZE> access_count;
	alignas(64) std::array<u8, ICACHE_SIZE> estimated_cycles;

	void reset() {
		pc.fill(0xFFFFFFFF);
		std::memset(access_count.data(), 0, access_count.size() * sizeof(u32));
		estimated_cycles.fill(1);
		opcode.fill(0);
	}

	u16 fetch(u32 addr, u8* cycles_out) {
		u32 index = (addr >> 1) & ICACHE_MASK;

		// Prefetch next cache lines for A10's aggressive prefetcher
		__builtin_prefetch(&pc[(index + 8) & ICACHE_MASK], 0, 3);
		__builtin_prefetch(&opcode[(index + 8) & ICACHE_MASK], 0, 3);

		if (__builtin_expect(pc[index] == addr, 1)) {
			access_count[index]++;
			*cycles_out = estimated_cycles[index];
			return opcode[index];
		}

		u16 op = FastIReadMem16(addr);
		pc[index] = addr;
		opcode[index] = op;
		access_count[index] = 1;

		u8 est_cycles = estimateInstructionCycles(op);
		estimated_cycles[index] = est_cycles;
		*cycles_out = est_cycles;

		return op;
	}

	bool isHotPath(u32 addr) {
		u32 index = (addr >> 1) & ICACHE_MASK;
		return pc[index] == addr && access_count[index] > 20;
	}

private:
	u8 estimateInstructionCycles(u16 op) {
		// Use branch prediction for branch instructions if enabled
		if (g_branch_prediction_enabled && IsBranchInstruction(op)) {
			// Branch prediction can reduce effective cycles for predicted branches
			return 1; // Optimistic estimate for predicted branches
		}

		switch (op & 0xF000) {
			case 0x6000:
				if ((op & 0x000F) <= 0x0003) return 2;
				return 1;
			case 0x2000:
				return 2;
			case 0x8000:
			case 0xA000:
			case 0xB000:
				return 2;
			case 0xF000:
				return 3;
			default:
				return 1;
		}
	}
};

// === ADAPTIVE CYCLE MANAGEMENT ===
static u32 g_cycle_debt = 0;
static u32 g_instruction_count = 0;
static u32 g_cycles_since_aica_check = 0;

static const u32 AICA_TICK_INTERVAL = 4535;
static const u32 AICA_SAFETY_MARGIN = 200;
static const u32 FMV_CYCLE_BATCH_SIZE = 512;
static const u32 NORMAL_CYCLE_BATCH_SIZE = 128;

static inline void addCyclesOptimized(u8 cycles) {
	g_cycle_debt += cycles;
	g_cycles_since_aica_check += cycles;
	g_instruction_count++;
}

static inline void flushCyclesIfNeeded(Sh4Interpreter* interpreter) {
	u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));

	u32 batch_size;
	if (cycles_until_aica <= AICA_SAFETY_MARGIN || g_cycles_since_aica_check >= (AICA_TICK_INTERVAL - AICA_SAFETY_MARGIN)) {
		if (g_cycle_debt > 0) {
			interpreter->sh4cycles.addCycles(g_cycle_debt);
			g_cycles_since_aica_check = 0;
			g_cycle_debt = 0;
			return;
		}
		batch_size = 1;
	} else if (g_instruction_count > 100) {
		batch_size = FMV_CYCLE_BATCH_SIZE;
	} else {
		batch_size = NORMAL_CYCLE_BATCH_SIZE;
	}

	if (__builtin_expect(g_cycle_debt >= batch_size, 0)) {
		interpreter->sh4cycles.addCycles(g_cycle_debt);
		g_cycle_debt = 0;
	}
}

static inline void forceFlushCycles(Sh4Interpreter* interpreter) {
	if (g_cycle_debt > 0) {
		interpreter->sh4cycles.addCycles(g_cycle_debt);
		g_cycle_debt = 0;
		g_cycles_since_aica_check = 0;
	}
}

// === PERFORMANCE MODE DETECTION ===
static u32 g_consecutive_instructions = 0;
static u32 g_last_pc = 0;
static u32 g_performance_mode_timer = 0;
static bool g_in_performance_mode = false;

static inline bool isInPerformanceMode(u32 current_pc) {
	if (current_pc == g_last_pc + 2) {
		g_consecutive_instructions++;
		g_last_pc = current_pc;

		bool potential_performance = g_consecutive_instructions > 300 &&
		                           (current_pc & 0xFF000000) == 0x8C000000;

		if (potential_performance && !g_in_performance_mode) {
			g_in_performance_mode = true;
			g_performance_mode_timer = 0;
			return true;
		} else if (g_in_performance_mode) {
			g_performance_mode_timer++;
			if (g_performance_mode_timer > 5000) {
				g_in_performance_mode = false;
				g_performance_mode_timer = 0;
				return false;
			}
			return true;
		}
		return false;
	} else {
		g_consecutive_instructions = 0;
		g_last_pc = current_pc;
		g_in_performance_mode = false;
		g_performance_mode_timer = 0;
		return false;
	}
}

// === FAST MEMORY ACCESS FOR INTERPRETER ===
/// Bypass expensive memory access cycle calculations
/// These use simplified cycle estimates instead of complex area-based calculations

/// Fast memory access functions with simplified cycle accounting
u8 FastReadMem8_Interp(u32 addr) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2; // Simple estimate
	return ReadMem8(addr);
}

u16 FastReadMem16_Interp(u32 addr) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2;
	return ReadMem16(addr);
}

u32 FastReadMem32_Interp(u32 addr) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2;
	return ReadMem32(addr);
}

u64 FastReadMem64_Interp(u32 addr) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 3; // Slightly higher for 64-bit
	return ReadMem64(addr);
}

void FastWriteMem8_Interp(u32 addr, u8 data) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2;
	WriteMem8(addr, data);
}

void FastWriteMem16_Interp(u32 addr, u16 data) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2;
	WriteMem16(addr, data);
}

void FastWriteMem32_Interp(u32 addr, u32 data) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2;
	WriteMem32(addr, data);
}

void FastWriteMem64_Interp(u32 addr, u64 data) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 3;
	WriteMem64(addr, data);
}

s32 FastReadMemS8_Interp(u32 addr) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2;
	return (s32)(s8)ReadMem8(addr);
}

s32 FastReadMemS16_Interp(u32 addr) {
	Sh4Interpreter::Instance->getContext()->cycle_counter -= 2;
	return (s32)(s16)ReadMem16(addr);
}

// Global cache instance
static OptimizedInstructionCache g_instruction_cache;

// === SIMPLIFIED CYCLE MODE ===
/// Global flag to enable/disable simplified cycle calculations
bool g_simplified_cycles_enabled = false;

// === BRANCH PREDICTION CACHE ===
/// Global flag to enable/disable branch prediction
bool g_branch_prediction_enabled = false;

/// Branch prediction cache for FMV performance
alignas(64) BranchCacheEntry g_branch_cache[64];
u32 g_branch_cache_hits = 0;
u32 g_branch_cache_misses = 0;

// === INSTRUCTION FUSION ===
/// Global flag to enable/disable instruction fusion
bool g_instruction_fusion_enabled = false;

/// Instruction fusion cache for common patterns
alignas(64) FusionCacheEntry g_fusion_cache[32];
u32 g_fusion_hits = 0;
u32 g_fusion_misses = 0;

/// Check if instruction is a branch
bool IsBranchInstruction(u16 op) {
    switch (op & 0xF000) {
        case 0x8000: // bf, bt, bf.s, bt.s
            return (op & 0x0F00) == 0x0B00 || (op & 0x0F00) == 0x0900 ||
                   (op & 0x0F00) == 0x0F00 || (op & 0x0F00) == 0x0D00;
        case 0xA000: // bra
        case 0xB000: // bsr
            return true;
        case 0x0000: // braf, bsrf, jmp, jsr, rts, rte
            return (op & 0x00FF) == 0x23 || (op & 0x00FF) == 0x03 ||
                   (op & 0x00FF) == 0x2B || (op & 0x00FF) == 0x0B;
        case 0x4000: // jmp, jsr
            return (op & 0x00FF) == 0x2B || (op & 0x00FF) == 0x0B;
    }
    return false;
}

/// Calculate branch target from opcode
static u32 CalculateBranchTarget(u32 pc, u16 op) {
    switch (op & 0xF000) {
        case 0x8000: // Conditional branches with 8-bit displacement
            return pc + 2 + ((s32)(s8)(op & 0xFF)) * 2;
        case 0xA000: // bra - 12-bit displacement
        case 0xB000: // bsr - 12-bit displacement
            return pc + 2 + ((s32)(((s16)(op << 4)) >> 4)) * 2;
    }
    return 0; // Indirect branches calculated at runtime
}

/// Predict branch target using cache and heuristics
u32 PredictBranchTarget(u32 pc, u16 op, bool condition_flag) {
    if (!g_branch_prediction_enabled) return 0;

    u32 cache_index = (pc >> 1) & 63; // Use PC bits for cache index
    BranchCacheEntry* entry = &g_branch_cache[cache_index];

    // Check cache hit
    if (entry->pc == pc) {
        // Cache hit - use stored prediction
        g_branch_cache_hits++;

        // For conditional branches, check pattern
        switch (op & 0xF000) {
            case 0x8000: // bf, bt variants
                if ((op & 0x0F00) == 0x0B00 || (op & 0x0F00) == 0x0F00) { // bf, bf.s
                    return condition_flag ? 0 : entry->target;
                } else { // bt, bt.s
                    return condition_flag ? entry->target : 0;
                }
            default:
                return entry->target;
        }
    }

    // Cache miss - calculate and store
    g_branch_cache_misses++;
    u32 target = CalculateBranchTarget(pc, op);

    entry->pc = pc;
    entry->target = target;
    entry->taken = true;
    entry->confidence = 128; // Medium confidence for new predictions
    entry->pattern = 0xFF; // Assume taken initially

    return target;
}

/// Update branch prediction based on actual outcome
void UpdateBranchPrediction(u32 pc, u32 actual_target, bool taken) {
    if (!g_branch_prediction_enabled) return;

    u32 cache_index = (pc >> 1) & 63;
    BranchCacheEntry* entry = &g_branch_cache[cache_index];

    if (entry->pc == pc) {
        // Update existing entry
        entry->pattern = (entry->pattern << 1) | (taken ? 1 : 0);

        if (taken == entry->taken) {
            // Correct prediction - increase confidence
            if (entry->confidence < 240) entry->confidence += 16;
        } else {
            // Wrong prediction - decrease confidence
            if (entry->confidence > 16) entry->confidence -= 16;
            entry->taken = taken;
        }

                 if (taken) entry->target = actual_target;
     }
}

/// Detect if two consecutive instructions can be fused
FusedInstructionType DetectFusionPattern(u16 op1, u16 op2) {
    if (!g_instruction_fusion_enabled) return FUSED_NONE;

    // Helper macros for instruction pattern matching
    #define GetN(op) ((op >> 8) & 0xF)
    #define GetM(op) ((op >> 4) & 0xF)
    #define GetImm8(op) (op & 0xFF)

    // Pattern 1: mov Rm,Rn + add #imm,Rn
    if ((op1 & 0xF00F) == 0x6003 && (op2 & 0xF000) == 0x7000) {
        if (GetN(op1) == GetN(op2)) {
            return FUSED_MOV_ADD_IMM;
        }
    }

    // Pattern 2: mov.l @Rm,Rn + add Rx,Rn
    if ((op1 & 0xF00F) == 0x6002 && (op2 & 0xF00F) == 0x300C) {
        if (GetN(op1) == GetN(op2)) {
            return FUSED_LOAD_ADD;
        }
    }

    // Pattern 3: mov.l @Rm,Rn + sub Rx,Rn
    if ((op1 & 0xF00F) == 0x6002 && (op2 & 0xF00F) == 0x3008) {
        if (GetN(op1) == GetN(op2)) {
            return FUSED_LOAD_SUB;
        }
    }

    // Pattern 4: add Rm,Rn + mov.l Rn,@Rx
    if ((op1 & 0xF00F) == 0x300C && (op2 & 0xF00F) == 0x2002) {
        if (GetN(op1) == GetM(op2)) {
            return FUSED_ADD_STORE;
        }
    }

    // Pattern 5: sub Rm,Rn + mov.l Rn,@Rx
    if ((op1 & 0xF00F) == 0x3008 && (op2 & 0xF00F) == 0x2002) {
        if (GetN(op1) == GetM(op2)) {
            return FUSED_SUB_STORE;
        }
    }

    // Pattern 6: cmp/eq Rm,Rn + bt/bf target
    if ((op1 & 0xF00F) == 0x3000 &&
        ((op2 & 0xFF00) == 0x8900 || (op2 & 0xFF00) == 0x8B00)) {
        return FUSED_CMP_BRANCH;
    }

    // Pattern 7: shll2 Rn + add Rm,Rn
    if ((op1 & 0xF0FF) == 0x4008 && (op2 & 0xF00F) == 0x300C) {
        if (GetN(op1) == GetN(op2)) {
            return FUSED_SHIFT_ADD;
        }
    }

    // Pattern 8: shll8 Rn + add Rm,Rn
    if ((op1 & 0xF0FF) == 0x4018 && (op2 & 0xF00F) == 0x300C) {
        if (GetN(op1) == GetN(op2)) {
            return FUSED_SHIFT_ADD;
        }
    }

    #undef GetN
    #undef GetM
    #undef GetImm8

    return FUSED_NONE;
}

/// Execute fused instruction pattern
bool ExecuteFusedInstruction(FusedInstructionType type, u16 op1, u16 op2, Sh4Context* ctx) {
    #define GetN(op) ((op >> 8) & 0xF)
    #define GetM(op) ((op >> 4) & 0xF)
    #define GetImm8(op) (op & 0xFF)
    #define GetSImm8(op) ((s32)(s8)(op & 0xFF))

    switch (type) {
        case FUSED_MOV_ADD_IMM: {
            // mov Rm,Rn + add #imm,Rn -> Rn = Rm + imm
            u32 m = GetM(op1);
            u32 n = GetN(op1);
            s32 imm = GetSImm8(op2);
            ctx->r[n] = ctx->r[m] + imm;
            return true;
        }

        case FUSED_LOAD_ADD: {
            // mov.l @Rm,Rn + add Rx,Rn -> Rn = *(u32*)Rm + Rx
            u32 m1 = GetM(op1), n = GetN(op1);
            u32 x = GetM(op2);
            ctx->r[n] = FastReadMem32_Interp(ctx->r[m1]) + ctx->r[x];
            return true;
        }

        case FUSED_LOAD_SUB: {
            // mov.l @Rm,Rn + sub Rx,Rn -> Rn = *(u32*)Rm - Rx
            u32 m1 = GetM(op1), n = GetN(op1);
            u32 x = GetM(op2);
            ctx->r[n] = FastReadMem32_Interp(ctx->r[m1]) - ctx->r[x];
            return true;
        }

        case FUSED_ADD_STORE: {
            // add Rm,Rn + mov.l Rn,@Rx -> Rn += Rm; *(u32*)Rx = Rn
            u32 m = GetM(op1), n = GetN(op1);
            u32 x = GetN(op2);
            ctx->r[n] += ctx->r[m];
            FastWriteMem32_Interp(ctx->r[x], ctx->r[n]);
            return true;
        }

        case FUSED_SUB_STORE: {
            // sub Rm,Rn + mov.l Rn,@Rx -> Rn -= Rm; *(u32*)Rx = Rn
            u32 m = GetM(op1), n = GetN(op1);
            u32 x = GetN(op2);
            ctx->r[n] -= ctx->r[m];
            FastWriteMem32_Interp(ctx->r[x], ctx->r[n]);
            return true;
        }

        case FUSED_CMP_BRANCH: {
            // cmp/eq Rm,Rn + bt/bf target -> compare and branch in one operation
            u32 m = GetM(op1), n = GetN(op1);
            bool equal = (ctx->r[m] == ctx->r[n]);
            ctx->sr.T = equal ? 1 : 0;

            // Handle branch
            if ((op2 & 0xFF00) == 0x8900) { // bt
                if (equal) {
                    s32 disp = GetSImm8(op2);
                    ctx->pc = ctx->pc + disp * 2;
                }
            } else { // bf
                if (!equal) {
                    s32 disp = GetSImm8(op2);
                    ctx->pc = ctx->pc + disp * 2;
                }
            }
            return true;
        }

        case FUSED_SHIFT_ADD: {
            // shll2/shll8 Rn + add Rm,Rn -> Rn = (Rn << shift) + Rm
            u32 n = GetN(op1);
            u32 m = GetM(op2);
            if ((op1 & 0xF0FF) == 0x4008) { // shll2
                ctx->r[n] = (ctx->r[n] << 2) + ctx->r[m];
            } else { // shll8
                ctx->r[n] = (ctx->r[n] << 8) + ctx->r[m];
            }
            return true;
        }

        default:
            return false;
    }

    #undef GetN
    #undef GetM
    #undef GetImm8
    #undef GetSImm8
}

/// Update instruction fusion cache
void UpdateFusionCache(u32 pc, u16 op1, u16 op2, FusedInstructionType type) {
    if (!g_instruction_fusion_enabled || type == FUSED_NONE) return;

    u32 cache_index = (pc >> 2) & 31; // Use PC bits for cache index
    FusionCacheEntry* entry = &g_fusion_cache[cache_index];

    if (entry->pc1 == pc && entry->op1 == op1 && entry->op2 == op2) {
        // Cache hit - update stats
        entry->hit_count++;
        if (entry->confidence < 240) entry->confidence += 16;
    } else {
        // Cache miss - new entry
        entry->pc1 = pc;
        entry->op1 = op1;
        entry->op2 = op2;
        entry->type = type;
        entry->confidence = 128;
        entry->hit_count = 1;
    }
}

/// Check if instruction fusion is available for PC
static FusedInstructionType CheckFusionCache(u32 pc, u16 op1, u16 op2) {
    if (!g_instruction_fusion_enabled) return FUSED_NONE;

    u32 cache_index = (pc >> 2) & 31;
    FusionCacheEntry* entry = &g_fusion_cache[cache_index];

    if (entry->pc1 == pc && entry->op1 == op1 && entry->op2 == op2 && entry->confidence > 64) {
        g_fusion_hits++;
        return entry->type;
    }

    g_fusion_misses++;
    return FUSED_NONE;
}

/// Reset instruction fusion cache
static void ResetInstructionFusionCache() {
    std::memset(g_fusion_cache, 0, sizeof(g_fusion_cache));
    g_fusion_hits = 0;
    g_fusion_misses = 0;

    // Initialize all entries as invalid
    for (int i = 0; i < 32; i++) {
        g_fusion_cache[i].pc1 = 0xFFFFFFFF;
        g_fusion_cache[i].confidence = 128;
    }
}

/// Reset branch prediction cache
static void ResetBranchPredictionCache() {
    std::memset(g_branch_cache, 0, sizeof(g_branch_cache));
    g_branch_cache_hits = 0;
    g_branch_cache_misses = 0;

    // Initialize all entries as invalid
    for (int i = 0; i < 64; i++) {
        g_branch_cache[i].pc = 0xFFFFFFFF;
        g_branch_cache[i].confidence = 128;
    }
}

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);
	OpPtr[op](ctx, op);

	// Use simplified cycle mode if enabled, otherwise use complex calculations
	if (__builtin_expect(g_simplified_cycles_enabled, 1)) {
		u8 cycles = FastCalculateInstructionCycles(op);
		ctx->cycle_counter -= cycles;
	} else {
		sh4cycles.executeCycles(op);
	}
}

u16 Sh4Interpreter::ReadNexOp()
{
	u32 addr = ctx->pc;
	if (!mmu_enabled() && (addr & 1))
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);

	ctx->pc = addr + 2;
	return FastIReadMem16(addr);
}

/// Optimized instruction fetching with caching
u16 Sh4Interpreter::FetchInstructionOptimized(u8* cycles_out)
{
	u32 addr = ctx->pc;
	if (!mmu_enabled() && (addr & 1))
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);

	// WINCE syscall optimizations temporarily disabled for jitless build
	// TODO: Implement alternative approach for WINCE syscalls in jitless mode

	ctx->pc += 2;
	return g_instruction_cache.fetch(addr, cycles_out);
}

/// Execute performance-optimized mega batch for FMV sequences
void Sh4Interpreter::ExecutePerformanceMegaBatch()
{
	const u32 MEGA_BATCH_SIZE = 512;

	for (u32 i = 0; i < MEGA_BATCH_SIZE; i++) {
		if (__builtin_expect((i & 15) == 0 && ctx->cycle_counter <= 0, 0)) {
			break;
		}

		if ((i & 63) == 63) {
			u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));
			if (cycles_until_aica <= AICA_SAFETY_MARGIN) {
				forceFlushCycles(this);
				break;
			}
		}

		u8 estimated_cycles;
		u16 op = FetchInstructionOptimized(&estimated_cycles);

		if (__builtin_expect(ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

		// Try instruction fusion if enabled
		bool fused = false;
		if (g_instruction_fusion_enabled && i < (MEGA_BATCH_SIZE - 1)) {
			u32 next_pc = ctx->pc;
			u16 next_op = FastIReadMem16(next_pc);

			FusedInstructionType fusion_type = CheckFusionCache(ctx->pc - 2, op, next_op);
			if (fusion_type == FUSED_NONE) {
				fusion_type = DetectFusionPattern(op, next_op);
			}

			if (fusion_type != FUSED_NONE) {
				if (ExecuteFusedInstruction(fusion_type, op, next_op, ctx)) {
					UpdateFusionCache(ctx->pc - 2, op, next_op, fusion_type);
					ctx->pc += 2; // Skip next instruction since we fused it
					addCyclesOptimized(estimated_cycles + 1); // Estimate for both instructions
					fused = true;
					i++; // Count the fused instruction
				}
			}
		}

		if (!fused) {
			OpPtr[op](ctx, op);
			addCyclesOptimized(estimated_cycles);
		}

		if ((i & 31) == 31) {
			flushCyclesIfNeeded(this);
		}
	}
}

/// Execute optimized batch for hot code paths
void Sh4Interpreter::ExecuteHotBatch()
{
	const u32 HOT_BATCH_SIZE = 64;

	for (u32 i = 0; i < HOT_BATCH_SIZE && ctx->cycle_counter > 0; i++) {
		if ((i & 31) == 31) {
			u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));
			if (cycles_until_aica <= AICA_SAFETY_MARGIN) {
				forceFlushCycles(this);
				break;
			}
		}

		u8 estimated_cycles;
		u16 op = FetchInstructionOptimized(&estimated_cycles);

		if (__builtin_expect(ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

		OpPtr[op](ctx, op);
		addCyclesOptimized(estimated_cycles);

		if ((i & 15) == 15) {
			flushCyclesIfNeeded(this);
		}
	}
}

/// Execute normal batch for regular code
void Sh4Interpreter::ExecuteNormalBatch()
{
	const u32 NORMAL_BATCH_SIZE = 32;

	for (u32 i = 0; i < NORMAL_BATCH_SIZE && ctx->cycle_counter > 0; i++) {
		if ((i & 15) == 15) {
			u32 cycles_until_aica = (AICA_TICK_INTERVAL - (g_cycles_since_aica_check % AICA_TICK_INTERVAL));
			if (cycles_until_aica <= AICA_SAFETY_MARGIN) {
				forceFlushCycles(this);
				break;
			}
		}

		u8 estimated_cycles;
		u16 op = FetchInstructionOptimized(&estimated_cycles);

		if (__builtin_expect(ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

		OpPtr[op](ctx, op);
		addCyclesOptimized(estimated_cycles);

		if ((i & 7) == 7) {
			flushCyclesIfNeeded(this);
		}
	}
}

/// Adaptive execution engine that chooses optimal batch size
void Sh4Interpreter::ExecuteAdaptiveBatch()
{
	if (isInPerformanceMode(ctx->pc)) {
		ExecutePerformanceMegaBatch();
	} else if (g_instruction_cache.isHotPath(ctx->pc)) {
		ExecuteHotBatch();
	} else {
		ExecuteNormalBatch();
	}
}

void Sh4Interpreter::Run()
{
	Instance = this;
	ctx->restoreHostRoundingMode();

	g_instruction_cache.reset();
	g_cycle_debt = 0;
	g_instruction_count = 0;

	try {
		do {
			try {
				do {
					ExecuteAdaptiveBatch();
				} while (__builtin_expect(ctx->cycle_counter > 0, 1));

								forceFlushCycles(this);
				ctx->cycle_counter += SH4_TIMESLICE;
				UpdateSystem_INTC();

			} catch (const SH4ThrownException& ex) {
				forceFlushCycles(this);
				Do_Exception(ex.epc, ex.expEvn);
				addCyclesOptimized(5 * sh4cycles.getCpuRatio());
				forceFlushCycles(this);
			}
		} while (__builtin_expect(ctx->CpuRunning, 1));
	} catch (const debugger::Stop&) {
		forceFlushCycles(this);
	}

	ctx->CpuRunning = false;
	Instance = nullptr;
}

void Sh4Interpreter::Start()
{
	ctx->CpuRunning = true;
}

void Sh4Interpreter::Stop()
{
	ctx->CpuRunning = false;
	forceFlushCycles(this);
}

void Sh4Interpreter::Step()
{
	verify(!ctx->CpuRunning);
	Instance = this;

	ctx->restoreHostRoundingMode();
	try {
		u8 estimated_cycles;
		u16 op = FetchInstructionOptimized(&estimated_cycles);

		if (__builtin_expect(ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

		OpPtr[op](ctx, op);
		addCyclesOptimized(estimated_cycles);
		forceFlushCycles(this);
	} catch (const SH4ThrownException& ex) {
		forceFlushCycles(this);
		Do_Exception(ex.epc, ex.expEvn);
		addCyclesOptimized(5 * sh4cycles.getCpuRatio());
		forceFlushCycles(this);
	} catch (const debugger::Stop&) {
		forceFlushCycles(this);
	}
	Instance = nullptr;
}

void Sh4Interpreter::Reset(bool hard)
{
	verify(!ctx->CpuRunning);

	if (hard) {
		int schedNext = ctx->sh4_sched_next;
		memset(ctx, 0, sizeof(*ctx));
		ctx->sh4_sched_next = schedNext;
	}

	ctx->pc = 0xA0000000;
	memset(ctx->r, 0, sizeof(ctx->r));
	memset(ctx->r_bank, 0, sizeof(ctx->r_bank));
	ctx->gbr = ctx->ssr = ctx->spc = ctx->sgr = ctx->dbr = ctx->vbr = 0;
	ctx->mac.full = ctx->pr = ctx->fpul = 0;
	ctx->sr.setFull(0x700000F0);
	ctx->old_sr.status = ctx->sr.status;
	UpdateSR();
	ctx->fpscr.full = 0x00040001;
	ctx->old_fpscr = ctx->fpscr;

	icache.Reset(hard);
	ocache.Reset(hard);
	sh4cycles.reset();
	ctx->cycle_counter = SH4_TIMESLICE;

	g_instruction_cache.reset();
	g_cycle_debt = 0;
	g_instruction_count = 0;
	g_cycles_since_aica_check = 0;
	g_consecutive_instructions = 0;
	g_last_pc = 0;
	g_performance_mode_timer = 0;
	g_in_performance_mode = false;

		// Enable simplified cycle mode by default for better FMV performance
	g_simplified_cycles_enabled = true;

	// Enable branch prediction for FMV performance
	g_branch_prediction_enabled = true;
	ResetBranchPredictionCache();

	// Enable instruction fusion for FMV performance
	g_instruction_fusion_enabled = true;
	ResetInstructionFusionCache();

	INFO_LOG(INTERPRETER, "Optimized SH4 Interpreter - Advanced instruction caching, adaptive execution, simplified cycle mode, branch prediction, and instruction fusion enabled");
}

bool Sh4Interpreter::IsCpuRunning()
{
	return ctx->CpuRunning;
}

void Sh4Interpreter::ExecuteDelayslot()
{
	try {
		u8 estimated_cycles;
		u16 op = FetchInstructionOptimized(&estimated_cycles);

		if (__builtin_expect(ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

		OpPtr[op](ctx, op);
		addCyclesOptimized(estimated_cycles);
		forceFlushCycles(this);
	} catch (SH4ThrownException& ex) {
		forceFlushCycles(this);
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		forceFlushCycles(this);
		ctx->pc -= 2;
		throw e;
	}
}

void Sh4Interpreter::ExecuteDelayslot_RTE()
{
	try {
		u8 estimated_cycles;
		u16 op = FetchInstructionOptimized(&estimated_cycles);
		ctx->sr.setFull(ctx->ssr);

		if (__builtin_expect(ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint(), 0))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);

		OpPtr[op](ctx, op);
		addCyclesOptimized(estimated_cycles);
		forceFlushCycles(this);
	} catch (const SH4ThrownException&) {
		forceFlushCycles(this);
		throw FlycastException("Fatal: SH4 exception in RTE delay slot");
	} catch (const debugger::Stop& e) {
		forceFlushCycles(this);
		ctx->pc -= 2;
		throw e;
	}
}

// every SH4_TIMESLICE cycles
int UpdateSystem_INTC()
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next < 0)
		sh4_sched_tick(SH4_TIMESLICE);
	if (Sh4cntx.interrupt_pend)
		return UpdateINTC();
	else
		return 0;
}

void Sh4Interpreter::ResetCache()
{
	g_instruction_cache.reset();
	g_cycle_debt = 0;
	g_instruction_count = 0;
	g_cycles_since_aica_check = 0;
	g_consecutive_instructions = 0;
	g_last_pc = 0;
	g_performance_mode_timer = 0;
	g_in_performance_mode = false;
	g_simplified_cycles_enabled = true; // Enable simplified cycles by default
	g_branch_prediction_enabled = true; // Enable branch prediction by default
	ResetBranchPredictionCache();
	g_instruction_fusion_enabled = true; // Enable instruction fusion by default
	ResetInstructionFusionCache();
}

void Sh4Interpreter::Init()
{
	ctx = &p_sh4rcb->cntx;
	memset(ctx, 0, sizeof(*ctx));
	sh4cycles.init(ctx);
	icache.init(ctx);
	ocache.init(ctx);
	ResetCache();
}

void Sh4Interpreter::Term()
{
	Stop();
	INFO_LOG(INTERPRETER, "Optimized SH4 Interpreter Term");
}

/// Toggle simplified cycle mode for performance testing
void Sh4Interpreter::SetSimplifiedCycleMode(bool enabled)
{
	g_simplified_cycles_enabled = enabled;
	INFO_LOG(INTERPRETER, "Simplified cycle mode %s", enabled ? "enabled" : "disabled");
}

bool Sh4Interpreter::GetSimplifiedCycleMode()
{
	return g_simplified_cycles_enabled;
}

/// Toggle branch prediction for performance testing
void Sh4Interpreter::SetBranchPredictionMode(bool enabled)
{
	g_branch_prediction_enabled = enabled;
	if (enabled) {
		ResetBranchPredictionCache();
	}
	INFO_LOG(INTERPRETER, "Branch prediction %s", enabled ? "enabled" : "disabled");
}

bool Sh4Interpreter::GetBranchPredictionMode()
{
	return g_branch_prediction_enabled;
}

/// Get branch prediction statistics
void Sh4Interpreter::GetBranchPredictionStats(u32& hits, u32& misses)
{
	hits = g_branch_cache_hits;
	misses = g_branch_cache_misses;
}

/// Toggle instruction fusion for performance testing
void Sh4Interpreter::SetInstructionFusionMode(bool enabled)
{
	g_instruction_fusion_enabled = enabled;
	if (enabled) {
		ResetInstructionFusionCache();
	}
	INFO_LOG(INTERPRETER, "Instruction fusion %s", enabled ? "enabled" : "disabled");
}

bool Sh4Interpreter::GetInstructionFusionMode()
{
	return g_instruction_fusion_enabled;
}

/// Get instruction fusion statistics
void Sh4Interpreter::GetInstructionFusionStats(u32& hits, u32& misses)
{
	hits = g_fusion_hits;
	misses = g_fusion_misses;
}

Sh4Executor *Get_Sh4Interpreter()
{
	return new Sh4Interpreter();
}
