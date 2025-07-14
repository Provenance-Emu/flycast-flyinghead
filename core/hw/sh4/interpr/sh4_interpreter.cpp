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

// Global cache instance
static OptimizedInstructionCache g_instruction_cache;

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);
	OpPtr[op](ctx, op);
	sh4cycles.executeCycles(op);
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

		OpPtr[op](ctx, op);
		addCyclesOptimized(estimated_cycles);

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
				addCyclesOptimized(5 * CPU_RATIO);
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
		addCyclesOptimized(5 * CPU_RATIO);
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

	INFO_LOG(INTERPRETER, "Optimized SH4 Interpreter - Advanced instruction caching and adaptive execution enabled");
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

Sh4Executor *Get_Sh4Interpreter()
{
	return new Sh4Interpreter();
}
