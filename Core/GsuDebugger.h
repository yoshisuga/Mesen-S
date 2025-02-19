#pragma once
#include "stdafx.h"
#include "DebugTypes.h"

class Disassembler;
class Debugger;
class TraceLogger;
class Gsu;
class MemoryAccessCounter;
class MemoryManager;

class GsuDebugger
{
	Debugger* _debugger;
	Disassembler* _disassembler;
	TraceLogger* _traceLogger;
	MemoryAccessCounter* _memoryAccessCounter;
	MemoryManager* _memoryManager;
	Gsu* _gsu;

	unique_ptr<StepRequest> _step;

	uint8_t _prevOpCode = 0xFF;
	uint32_t _prevProgramCounter = 0;

public:
	GsuDebugger(Debugger* debugger);

	void Reset();

	void ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type);
	void ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type);
	void Run();
	void Step(int32_t stepCount, StepType type);
};