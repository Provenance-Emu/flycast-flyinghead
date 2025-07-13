#pragma once
#include "types.h"
#include "sh4_cycles.h"

class Sh4Interpreter : public Sh4Executor
{
public:
	void Run() override;
	void ResetCache() override;
	void Start() override;
	void Stop() override;
	void Step() override;
	void Reset(bool hard) override;
	void Init() override;
	void Term() override;
	bool IsCpuRunning() override;
	void ExecuteDelayslot();
	void ExecuteDelayslot_RTE();
	Sh4Context *getContext() { return ctx; }

	static Sh4Interpreter *Instance;

protected:
	Sh4Context *ctx = nullptr;

public:
	Sh4Cycles sh4cycles{CPU_RATIO};

private:
	void ExecuteOpcode(u16 op);
	u16 ReadNexOp();

	// Optimized execution methods
	u16 FetchInstructionOptimized(u8* cycles_out);
	void ExecutePerformanceMegaBatch();
	void ExecuteHotBatch();
	void ExecuteNormalBatch();
	void ExecuteAdaptiveBatch();
	// SH4 underclock factor when using the interpreter so that it's somewhat usable
#ifdef STRICT_MODE
	static constexpr int CPU_RATIO = 1;
#else
	static constexpr int CPU_RATIO = 8;
#endif
};
