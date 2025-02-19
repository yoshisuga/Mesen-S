#include "stdafx.h"
#include "Debugger.h"
#include "DebugTypes.h"
#include "Console.h"
#include "Cpu.h"
#include "Ppu.h"
#include "Spc.h"
#include "Sa1.h"
#include "Gsu.h"
#include "Cx4.h"
#include "NecDsp.h"
#include "CpuDebugger.h"
#include "SpcDebugger.h"
#include "GsuDebugger.h"
#include "BaseCartridge.h"
#include "MemoryManager.h"
#include "EmuSettings.h"
#include "SoundMixer.h"
#include "MemoryMappings.h"
#include "NotificationManager.h"
#include "CpuTypes.h"
#include "DisassemblyInfo.h"
#include "TraceLogger.h"
#include "MemoryDumper.h"
#include "MemoryAccessCounter.h"
#include "CodeDataLogger.h"
#include "Disassembler.h"
#include "BreakpointManager.h"
#include "PpuTools.h"
#include "EventManager.h"
#include "EventType.h"
#include "DebugBreakHelper.h"
#include "LabelManager.h"
#include "ScriptManager.h"
#include "CallstackManager.h"
#include "ExpressionEvaluator.h"
#include "../Utilities/HexUtilities.h"
#include "../Utilities/FolderUtilities.h"

Debugger::Debugger(shared_ptr<Console> console)
{
	_console = console;
	_cpu = console->GetCpu();
	_ppu = console->GetPpu();
	_spc = console->GetSpc();
	_cart = console->GetCartridge();
	_settings = console->GetSettings();
	_memoryManager = console->GetMemoryManager();

	_labelManager.reset(new LabelManager(this));
	_watchExpEval[(int)CpuType::Cpu].reset(new ExpressionEvaluator(this, CpuType::Cpu));
	_watchExpEval[(int)CpuType::Spc].reset(new ExpressionEvaluator(this, CpuType::Spc));
	_watchExpEval[(int)CpuType::Sa1].reset(new ExpressionEvaluator(this, CpuType::Sa1));
	_watchExpEval[(int)CpuType::Gsu].reset(new ExpressionEvaluator(this, CpuType::Gsu));

	_codeDataLogger.reset(new CodeDataLogger(_cart->DebugGetPrgRomSize(), _memoryManager.get()));
	_memoryDumper.reset(new MemoryDumper(_ppu, console->GetSpc(), _memoryManager, _cart));
	_disassembler.reset(new Disassembler(console, _codeDataLogger, this));
	_traceLogger.reset(new TraceLogger(this, _console));
	_memoryAccessCounter.reset(new MemoryAccessCounter(this, console.get()));
	_breakpointManager.reset(new BreakpointManager(this));
	_ppuTools.reset(new PpuTools(_console.get(), _ppu.get()));
	_eventManager.reset(new EventManager(this, _cpu.get(), _ppu.get(), _console->GetDmaController().get()));
	_scriptManager.reset(new ScriptManager(this));

	_cpuDebugger.reset(new CpuDebugger(this, CpuType::Cpu));
	_spcDebugger.reset(new SpcDebugger(this));
	if(_cart->GetSa1()) {
		_sa1Debugger.reset(new CpuDebugger(this, CpuType::Sa1));
	} else if(_cart->GetGsu()) {
		_gsuDebugger.reset(new GsuDebugger(this));
	}

	_step.reset(new StepRequest());

	_executionStopped = false;
	_breakRequestCount = 0;
	_suspendRequestCount = 0;

	string cdlFile = FolderUtilities::CombinePath(FolderUtilities::GetDebuggerFolder(), FolderUtilities::GetFilename(_cart->GetRomInfo().RomFile.GetFileName(), false) + ".cdl");
	_codeDataLogger->LoadCdlFile(cdlFile);

	RefreshCodeCache();
}

Debugger::~Debugger()
{
	Release();
}

void Debugger::Release()
{
	string cdlFile = FolderUtilities::CombinePath(FolderUtilities::GetDebuggerFolder(), FolderUtilities::GetFilename(_cart->GetRomInfo().RomFile.GetFileName(), false) + ".cdl");
	_codeDataLogger->SaveCdlFile(cdlFile);

	while(_executionStopped) {
		Run();
	}
}

void Debugger::Reset()
{
	_cpuDebugger->Reset();
	_spcDebugger->Reset();
	if(_sa1Debugger) {
		_sa1Debugger->Reset();
	}
}

template<CpuType type>
void Debugger::ProcessMemoryRead(uint32_t addr, uint8_t value, MemoryOperationType opType)
{
	switch(type) {
		case CpuType::Cpu: _cpuDebugger->ProcessRead(addr, value, opType); break;
		case CpuType::Spc: _spcDebugger->ProcessRead(addr, value, opType); break;
		case CpuType::Sa1: _sa1Debugger->ProcessRead(addr, value, opType); break;
		case CpuType::Gsu: _gsuDebugger->ProcessRead(addr, value, opType); break;
	}
}

template<CpuType type>
void Debugger::ProcessMemoryWrite(uint32_t addr, uint8_t value, MemoryOperationType opType)
{
	switch(type) {
		case CpuType::Cpu: _cpuDebugger->ProcessWrite(addr, value, opType); break;
		case CpuType::Spc: _spcDebugger->ProcessWrite(addr, value, opType); break;
		case CpuType::Sa1: _sa1Debugger->ProcessWrite(addr, value, opType); break;
		case CpuType::Gsu: _gsuDebugger->ProcessWrite(addr, value, opType); break;
	}
}

void Debugger::ProcessWorkRamRead(uint32_t addr, uint8_t value)
{
	AddressInfo addressInfo { (int32_t)addr, SnesMemoryType::WorkRam };
	//TODO Make this more flexible/accurate
	MemoryOperationInfo operation { 0x7E0000 | addr, value, MemoryOperationType::Read };
	ProcessBreakConditions(false, CpuType::Cpu, operation, addressInfo);
}

void Debugger::ProcessWorkRamWrite(uint32_t addr, uint8_t value)
{
	AddressInfo addressInfo { (int32_t)addr, SnesMemoryType::WorkRam };
	//TODO Make this more flexible/accurate
	MemoryOperationInfo operation { 0x7E0000 | addr, value, MemoryOperationType::Write };
	ProcessBreakConditions(false, CpuType::Cpu, operation, addressInfo);
}

void Debugger::ProcessPpuRead(uint16_t addr, uint8_t value, SnesMemoryType memoryType)
{
	AddressInfo addressInfo { addr, memoryType };
	MemoryOperationInfo operation { addr, value, MemoryOperationType::Read };
	ProcessBreakConditions(false, CpuType::Cpu, operation, addressInfo);

	_memoryAccessCounter->ProcessMemoryAccess(addressInfo, MemoryOperationType::Read, _memoryManager->GetMasterClock());
}

void Debugger::ProcessPpuWrite(uint16_t addr, uint8_t value, SnesMemoryType memoryType)
{
	AddressInfo addressInfo { addr, memoryType };
	MemoryOperationInfo operation { addr, value, MemoryOperationType::Write };
	ProcessBreakConditions(false, CpuType::Cpu, operation, addressInfo);

	_memoryAccessCounter->ProcessMemoryAccess(addressInfo, MemoryOperationType::Write, _memoryManager->GetMasterClock());
}

void Debugger::ProcessPpuCycle()
{
	uint16_t scanline = _ppu->GetScanline();
	uint16_t cycle = _ppu->GetCycle();
	_ppuTools->UpdateViewers(scanline, cycle);

	if(_step->PpuStepCount > 0) {
		_step->PpuStepCount--;
		if(_step->PpuStepCount == 0) {
			SleepUntilResume(BreakSource::PpuStep);
		}
	}

	if(cycle == 0 && scanline == _step->BreakScanline) {
		_step->BreakScanline = -1;
		SleepUntilResume(BreakSource::PpuStep);
	}
}

void Debugger::ProcessNecDspExec(uint32_t addr, uint32_t value)
{
	if(_traceLogger->IsCpuLogged(CpuType::NecDsp)) {
		AddressInfo addressInfo { (int32_t)addr * 4, SnesMemoryType::DspProgramRom };

		_disassembler->BuildCache(addressInfo, 0, CpuType::NecDsp);

		DebugState debugState;
		GetState(debugState, true);

		DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo);
		_traceLogger->Log(CpuType::NecDsp, debugState, disInfo);
	}
}

void Debugger::ProcessCx4Exec()
{
	if(_traceLogger->IsCpuLogged(CpuType::Cx4)) {
		DebugState debugState;
		GetState(debugState, true);

		uint32_t addr = (debugState.Cx4.Cache.Address[debugState.Cx4.Cache.Page] + (debugState.Cx4.PC * 2)) & 0xFFFFFF;
		AddressInfo addressInfo = _memoryManager->GetMemoryMappings()->GetAbsoluteAddress(addr);

		if(addressInfo.Address >= 0) {
			_disassembler->BuildCache(addressInfo, 0, CpuType::Cx4);

			DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo);
			_traceLogger->Log(CpuType::Cx4, debugState, disInfo);
		}
	}
}

void Debugger::SleepUntilResume(BreakSource source, MemoryOperationInfo *operation, int breakpointId)
{
	if(_suspendRequestCount) {
		return;
	}

	_console->GetSoundMixer()->StopAudio();
	_disassembler->Disassemble(CpuType::Cpu);
	_disassembler->Disassemble(CpuType::Spc);
	if(_cart->GetSa1()) {
		_disassembler->Disassemble(CpuType::Sa1);
	} else if(_cart->GetGsu()) {
		_disassembler->Disassemble(CpuType::Gsu);
	}

	_executionStopped = true;
	
	if(_breakRequestCount == 0) {
		//Only trigger code break event if the pause was caused by user action
		BreakEvent evt = {};
		evt.BreakpointId = breakpointId;
		evt.Source = source;
		if(operation) {
			evt.Operation = *operation;
		}
		_waitForBreakResume = true;
		_console->GetNotificationManager()->SendNotification(ConsoleNotificationType::CodeBreak, &evt);
	}

	while((_waitForBreakResume && !_suspendRequestCount) || _breakRequestCount) {
		std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(10));
	}

	_executionStopped = false;
}

void Debugger::ProcessBreakConditions(bool needBreak, CpuType cpuType, MemoryOperationInfo &operation, AddressInfo &addressInfo, BreakSource source)
{
	if(needBreak || _breakRequestCount) {
		SleepUntilResume(source);
	} else {
		int breakpointId = _breakpointManager->CheckBreakpoint(cpuType, operation, addressInfo);
		if(breakpointId >= 0) {
			SleepUntilResume(BreakSource::Breakpoint, &operation, breakpointId);
		}
	}
}

template<CpuType type>
void Debugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	switch(type) {
		case CpuType::Cpu: _cpuDebugger->ProcessInterrupt(originalPc, currentPc, forNmi); break;
		case CpuType::Sa1: _sa1Debugger->ProcessInterrupt(originalPc, currentPc, forNmi); break;
	}
}

void Debugger::ProcessEvent(EventType type)
{
	_scriptManager->ProcessEvent(type);

	switch(type) {
		default: break;

		case EventType::StartFrame:
			_console->GetNotificationManager()->SendNotification(ConsoleNotificationType::EventViewerRefresh);
			_eventManager->ClearFrameEvents();
			break;

		case EventType::Reset:
			Reset();
			break;
	}
}

int32_t Debugger::EvaluateExpression(string expression, CpuType cpuType, EvalResultType &resultType, bool useCache)
{
	MemoryOperationInfo operationInfo { 0, 0, MemoryOperationType::Read };
	DebugState state;
	GetState(state, false);
	if(useCache) {
		return _watchExpEval[(int)cpuType]->Evaluate(expression, state, resultType, operationInfo);
	} else {
		ExpressionEvaluator expEval(this, cpuType);
		return expEval.Evaluate(expression, state, resultType, operationInfo);
	}
}

void Debugger::Run()
{
	_step.reset(new StepRequest());
	_cpuDebugger->Run();
	_spcDebugger->Run();
	if(_sa1Debugger) {
		_sa1Debugger->Run();
	}
	if(_gsuDebugger) {
		_gsuDebugger->Run();
	}
	_waitForBreakResume = false;
}

void Debugger::Step(CpuType cpuType, int32_t stepCount, StepType type)
{
	StepRequest step;

	_cpuDebugger->Run();
	_spcDebugger->Run();
	if(_sa1Debugger) {
		_sa1Debugger->Run();
	}
	if(_gsuDebugger) {
		_gsuDebugger->Run();
	}

	switch(type) {
		case StepType::PpuStep: step.PpuStepCount = stepCount; break;
		case StepType::SpecificScanline: step.BreakScanline = stepCount; break;

		default:
			switch(cpuType) {
				case CpuType::Cpu: _cpuDebugger->Step(stepCount, type); break;
				case CpuType::Spc: _spcDebugger->Step(stepCount, type); break;
				case CpuType::Sa1: _sa1Debugger->Step(stepCount, type); break;
				case CpuType::Gsu: _gsuDebugger->Step(stepCount, type); break;
				
				case CpuType::NecDsp: 
				case CpuType::Cx4: 
					throw std::runtime_error("Step(): Unsupported CPU type.");
			}
			break;
	}

	_step.reset(new StepRequest(step));
	_waitForBreakResume = false;
}

bool Debugger::IsExecutionStopped()
{
	return _executionStopped;
}

void Debugger::BreakRequest(bool release)
{
	if(release) {
		_breakRequestCount--;
	} else {
		_breakRequestCount++;
	}
}

void Debugger::SuspendDebugger(bool release)
{
	if(release) {
		_suspendRequestCount--;
	} else {
		_suspendRequestCount++;
	}
}

void Debugger::GetState(DebugState &state, bool partialPpuState)
{
	state.MasterClock = _memoryManager->GetMasterClock();
	state.Cpu = _cpu->GetState();
	_ppu->GetState(state.Ppu, partialPpuState);
	state.Spc = _spc->GetState();
	if(_cart->GetDsp()) {
		state.Dsp = _cart->GetDsp()->GetState();
	}
	if(_cart->GetSa1()) {
		state.Sa1 = _cart->GetSa1()->GetCpuState();
	}
	if(_cart->GetGsu()) {
		state.Gsu = _cart->GetGsu()->GetState();
	}
	if(_cart->GetCx4()) {
		state.Cx4 = _cart->GetCx4()->GetState();
	}
}

AddressInfo Debugger::GetAbsoluteAddress(AddressInfo relAddress)
{
	if(relAddress.Type == SnesMemoryType::CpuMemory) {
		if(_memoryManager->IsRegister(relAddress.Address)) {
			return { relAddress.Address & 0xFFFF, SnesMemoryType::Register };
		} else {
			return _memoryManager->GetMemoryMappings()->GetAbsoluteAddress(relAddress.Address);
		}
	} else if(relAddress.Type == SnesMemoryType::SpcMemory) {
		return _spc->GetAbsoluteAddress(relAddress.Address);
	} else if(relAddress.Type == SnesMemoryType::Sa1Memory) {
		return _cart->GetSa1()->GetMemoryMappings()->GetAbsoluteAddress(relAddress.Address);
	} else if(relAddress.Type == SnesMemoryType::GsuMemory) {
		return _cart->GetGsu()->GetMemoryMappings()->GetAbsoluteAddress(relAddress.Address);
	}

	throw std::runtime_error("Unsupported address type");
}

AddressInfo Debugger::GetRelativeAddress(AddressInfo absAddress)
{
	switch(absAddress.Type) {
		case SnesMemoryType::PrgRom:
		case SnesMemoryType::WorkRam:
		case SnesMemoryType::SaveRam:
			return { _memoryManager->GetRelativeAddress(absAddress), SnesMemoryType::CpuMemory };

		case SnesMemoryType::SpcRam:
		case SnesMemoryType::SpcRom:
			return { _spc->GetRelativeAddress(absAddress), SnesMemoryType::SpcMemory };

		case SnesMemoryType::Register:
			return { -1, SnesMemoryType::Register };

		default: 
			return { -1, SnesMemoryType::Register };

			throw std::runtime_error("Unsupported address type");
	}
}

void Debugger::SetCdlData(uint8_t *cdlData, uint32_t length)
{
	DebugBreakHelper helper(this);
	_codeDataLogger->SetCdlData(cdlData, length);
	RefreshCodeCache();
}

void Debugger::RefreshCodeCache()
{
	_disassembler->ResetPrgCache();
	uint32_t prgRomSize = _cart->DebugGetPrgRomSize();
	AddressInfo addrInfo;
	addrInfo.Type = SnesMemoryType::PrgRom;

	for(uint32_t i = 0; i < prgRomSize; i++) {
		if(_codeDataLogger->IsCode(i)) {
			addrInfo.Address = (int32_t)i;
			i += _disassembler->BuildCache(addrInfo, _codeDataLogger->GetCpuFlags(i), CpuType::Cpu) - 1;
		}
	}
}

shared_ptr<TraceLogger> Debugger::GetTraceLogger()
{
	return _traceLogger;
}

shared_ptr<MemoryDumper> Debugger::GetMemoryDumper()
{
	return _memoryDumper;
}

shared_ptr<MemoryAccessCounter> Debugger::GetMemoryAccessCounter()
{
	return _memoryAccessCounter;
}

shared_ptr<CodeDataLogger> Debugger::GetCodeDataLogger()
{
	return _codeDataLogger;
}

shared_ptr<Disassembler> Debugger::GetDisassembler()
{
	return _disassembler;
}

shared_ptr<BreakpointManager> Debugger::GetBreakpointManager()
{
	return _breakpointManager;
}

shared_ptr<PpuTools> Debugger::GetPpuTools()
{
	return _ppuTools;
}

shared_ptr<EventManager> Debugger::GetEventManager()
{
	return _eventManager;
}

shared_ptr<LabelManager> Debugger::GetLabelManager()
{
	return _labelManager;
}

shared_ptr<ScriptManager> Debugger::GetScriptManager()
{
	return _scriptManager;
}

shared_ptr<CallstackManager> Debugger::GetCallstackManager(CpuType cpuType)
{
	switch(cpuType) {
		case CpuType::Cpu: return _cpuDebugger->GetCallstackManager();
		case CpuType::Spc: return _spcDebugger->GetCallstackManager();
		case CpuType::Sa1: return _sa1Debugger->GetCallstackManager();
		
		case CpuType::Gsu:
		case CpuType::NecDsp:
		case CpuType::Cx4:
			break;
	}
	throw std::runtime_error("GetCallstackManager() - Unsupported CPU type");
}

shared_ptr<Console> Debugger::GetConsole()
{
	return _console;
}

template void Debugger::ProcessMemoryRead<CpuType::Cpu>(uint32_t addr, uint8_t value, MemoryOperationType opType);
template void Debugger::ProcessMemoryRead<CpuType::Sa1>(uint32_t addr, uint8_t value, MemoryOperationType opType);
template void Debugger::ProcessMemoryRead<CpuType::Spc>(uint32_t addr, uint8_t value, MemoryOperationType opType);
template void Debugger::ProcessMemoryRead<CpuType::Gsu>(uint32_t addr, uint8_t value, MemoryOperationType opType);

template void Debugger::ProcessMemoryWrite<CpuType::Cpu>(uint32_t addr, uint8_t value, MemoryOperationType opType);
template void Debugger::ProcessMemoryWrite<CpuType::Sa1>(uint32_t addr, uint8_t value, MemoryOperationType opType);
template void Debugger::ProcessMemoryWrite<CpuType::Spc>(uint32_t addr, uint8_t value, MemoryOperationType opType);
template void Debugger::ProcessMemoryWrite<CpuType::Gsu>(uint32_t addr, uint8_t value, MemoryOperationType opType);

template void Debugger::ProcessInterrupt<CpuType::Cpu>(uint32_t originalPc, uint32_t currentPc, bool forNmi);
template void Debugger::ProcessInterrupt<CpuType::Sa1>(uint32_t originalPc, uint32_t currentPc, bool forNmi);
