// Copyright (C) 2003-2009 Dolphin Project.
 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.
 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.
 
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/
 
// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/
 
 
#ifdef _WIN32
	#include <windows.h>
#else
#endif

#include "Setup.h" // Common
#include "Thread.h"
#include "Timer.h"
#include "Common.h"
#include "StringUtil.h"
#include "MathUtil.h"

#include "Console.h"
#include "Core.h"
#include "CPUDetect.h"
#include "CoreTiming.h"
#include "Boot/Boot.h"
 
#include "HW/Memmap.h"
#include "HW/PeripheralInterface.h"
#include "HW/GPFifo.h"
#include "HW/CPU.h"
#include "HW/HW.h"
#include "HW/DSP.h"
#include "HW/GPFifo.h"
#include "HW/AudioInterface.h"
#include "HW/VideoInterface.h"
#include "HW/CommandProcessor.h"
#include "HW/PixelEngine.h"
#include "HW/SystemTimers.h"
 
#include "PowerPC/PowerPC.h"
 
#include "PluginManager.h"
#include "ConfigManager.h"
 
#include "MemTools.h"
#include "Host.h"
#include "LogManager.h"
 
#include "State.h"
 
#ifndef _WIN32
#define WINAPI
#endif

namespace Core
{
 

// Declarations and definitions

 
// Function forwarding
//void Callback_VideoRequestWindowSize(int _iWidth, int _iHeight, BOOL _bFullscreen);
void Callback_VideoLog(const TCHAR* _szMessage, int _bDoBreak);
void Callback_VideoCopiedToXFB();
void Callback_DSPLog(const TCHAR* _szMessage, int _v);
const char *Callback_ISOName(void);
void Callback_DSPInterrupt();
void Callback_PADLog(const TCHAR* _szMessage);
void Callback_WiimoteLog(const TCHAR* _szMessage, int _v);
void Callback_WiimoteInput(u16 _channelID, const void* _pData, u32 _Size);
 
// For keyboard shortcuts.
void Callback_KeyPress(int key, bool shift, bool control);
TPeekMessages Callback_PeekMessages = NULL;
TUpdateFPSDisplay g_pUpdateFPSDisplay = NULL;
 
// Function declarations
THREAD_RETURN EmuThread(void *pArg);

void Stop();
 
bool g_bHwInit = false;
bool g_bRealWiimote = false;
HWND g_pWindowHandle = NULL;
Common::Thread* g_EmuThread = NULL;

SCoreStartupParameter g_CoreStartupParameter;

// This event is set when the emuthread starts.
Common::Event emuThreadGoing;
Common::Event cpuRunloopQuit;


#ifdef SETUP_TIMER_WAITING

	bool VideoThreadRunning = false;
	bool StopUpToVideoDone = false;
	bool EmuThreadReachedEnd = false;
	bool StopReachedEnd = false;
	static Common::Event VideoThreadEvent;
	static Common::Event VideoThreadEvent2;
	void EmuThreadEnd();
#endif


 
 

// Display messages and return values
// 
bool PanicAlertToVideo(const char* text, bool yes_no)
{
	DisplayMessage(text, 3000);
	return true;
}
 
void DisplayMessage(const std::string &message, int time_in_ms)
{
    CPluginManager::GetInstance().GetVideo()->Video_AddMessage(message.c_str(),
							       time_in_ms);
}
 
void DisplayMessage(const char *message, int time_in_ms)
{
    CPluginManager::GetInstance().GetVideo()->Video_AddMessage(message, 
							    time_in_ms);
}
 
void Callback_DebuggerBreak()
{
	CCPU::Break();
}
 
void *GetWindowHandle()
{
    return g_pWindowHandle;
}
 
bool GetRealWiimote()
{
    return g_bRealWiimote;
}

// This can occur when the emulator is not running and the nJoy configuration window is opened
void ReconnectPad()
{
	CPluginManager &Plugins = CPluginManager::GetInstance();
	Plugins.FreePad(0);
	Plugins.GetPad(0)->Config(g_pWindowHandle);
	INFO_LOG(CONSOLE, "ReconnectPad()\n");
}

// This doesn't work yet, I don't understand how the connection work yet
void ReconnectWiimote()
{
	// This seems to be a hack that just sets some IPC registers to zero. Dubious.
	/* JP: Yes, it's basically nothing right now, I could not figure out how to reset the Wiimote
	   for reconnection */
	HW::InitWiimote();
	INFO_LOG(CONSOLE, "ReconnectWiimote()\n");
}


#ifdef SETUP_TIMER_WAITING

	void VideoThreadEnd()
	{
		VideoThreadRunning = false;
		VideoThreadEvent.SetTimer();
		VideoThreadEvent2.SetTimer();
		//INFO_LOG(CONSOLE, "VideoThreadEnd\n");
	}
#endif





// This is called from the GUI thread. See the booting call schedule in BootManager.cpp

bool Init()
{
	if (g_EmuThread != NULL)
	{
		PanicAlert("ERROR: Emu Thread already running. Report this bug.");
		return false;
	}

	Common::InitThreading();

	// Get a handle to the current instance of the plugin manager
	CPluginManager &pManager = CPluginManager::GetInstance();
	SCoreStartupParameter &_CoreParameter = SConfig::GetInstance().m_LocalCoreStartupParameter;

	g_CoreStartupParameter = _CoreParameter;
	// FIXME DEBUG_LOG(BOOT, dump_params());
	Host_SetWaitCursor(true);

	// Start the thread again
	_dbg_assert_(HLE, g_EmuThread == NULL);

	// Check that all plugins exist, potentially call LoadLibrary() for unloaded plugins
	if (!pManager.InitPlugins())
		return false;

	emuThreadGoing.Init();
	// This will execute EmuThread() further down in this file
	g_EmuThread = new Common::Thread(EmuThread, NULL);
	emuThreadGoing.MsgWait();
	emuThreadGoing.Shutdown();
	#ifdef SETUP_TIMER_WAITING
		VideoThreadRunning = true;
	#endif

	// All right, the event is set and killed. We are now running.
	Host_SetWaitCursor(false);
	return true;
}

// Called from GUI thread or VI thread (why VI??? That must be bad. Window close? TODO: Investigate.)
// JP: No, when you press Stop this is run from the Main Thread it seems
// - Hammertime!
void Stop()  
{
	const SCoreStartupParameter& _CoreParameter = SConfig::GetInstance().m_LocalCoreStartupParameter;

#ifdef SETUP_TIMER_WAITING
	if (!StopUpToVideoDone)
	{
	INFO_LOG(CONSOLE, "Stop [Main Thread]:    Shutting down...\n");
	// Reset variables
	StopReachedEnd = false;
	EmuThreadReachedEnd = false;
#endif
	Host_SetWaitCursor(true);  // hourglass!
	if (PowerPC::GetState() == PowerPC::CPU_POWERDOWN)
		return;

	// Stop the CPU
	PowerPC::Stop();
	CCPU::StepOpcode();  // Kick it if it's waiting (code stepping wait loop)
	
	// Wait until the CPU finishes exiting the main run loop
	cpuRunloopQuit.Wait();
	cpuRunloopQuit.Shutdown();
	// At this point, we must be out of the CPU:s runloop.
	
	// Stop audio thread.
	CPluginManager::GetInstance().GetDSP()->DSP_StopSoundStream();
 
	// If dual core mode, the CPU thread should immediately exit here.
	if (_CoreParameter.bUseDualCore) {
		INFO_LOG(CONSOLE, "Stop [Main Thread]:    Wait for Video Loop to exit...\n");
		CPluginManager::GetInstance().GetVideo()->Video_ExitLoop();
	}

#ifdef SETUP_TIMER_WAITING
	StopUpToVideoDone = true;
	}
	// Call this back
	//if (!VideoThreadEvent.TimerWait(Stop, 1, EmuThreadReachedEnd) || !EmuThreadReachedEnd) return;
	if (!VideoThreadEvent.TimerWait(Stop, 1)) return;

	//INFO_LOG(CONSOLE, "Stop() will continue\n");
#endif

	// Video_EnterLoop() should now exit so that EmuThread() will continue concurrently with the rest
	// of the commands in this function. We no longer rely on Postmessage. */

	// Close the trace file
	Core::StopTrace();
	NOTICE_LOG(BOOT, "Shutting down core");

	// Update mouse pointer
	Host_SetWaitCursor(false);
	#ifdef SETUP_AVOID_CHILD_WINDOW_RENDERING_HANG
		/* This may hang when we are rendering to a child window, but currently it doesn't, at least
		   not on my system, but I'll leave this option for a while anyway */
		if (GetParent((HWND)g_pWindowHandle) == NULL)
	#endif
#ifndef SETUP_TIMER_WAITING // This is moved
#ifdef _WIN32
	g_EmuThread->WaitForDeath(5000);
#else
	g_EmuThread->WaitForDeath();
#endif
	delete g_EmuThread;  // Wait for emuthread to close.
	g_EmuThread = 0;
#endif
#ifdef SETUP_TIMER_WAITING
	Host_UpdateGUI();
	StopUpToVideoDone = false;
	StopReachedEnd = true;
#endif
}

 

// Create the CPU thread. For use with Single Core mode only.

THREAD_RETURN CpuThread(void *pArg)
{
	Common::SetCurrentThreadName("CPU thread");
 
	const SCoreStartupParameter& _CoreParameter = SConfig::GetInstance().m_LocalCoreStartupParameter;
	if (!_CoreParameter.bUseDualCore)
	{
		//wglMakeCurrent
		CPluginManager::GetInstance().GetVideo()->Video_Prepare(); 
	}
 
	if (_CoreParameter.bLockThreads)
		Common::Thread::SetCurrentThreadAffinity(1);  // Force to first core
 
	if (_CoreParameter.bUseFastMem)
	{
		#ifdef _M_X64
			// Let's run under memory watch
			#ifndef JITTEST
			EMM::InstallExceptionHandler();
			#endif
		#else
			PanicAlert("32-bit platforms do not support fastmem yet. Report this bug.");
		#endif
	}
 
	// Enter CPU run loop. When we leave it - we are done.
	CCPU::Run();
	cpuRunloopQuit.Set();
 	return 0;
}


 

// Initalize plugins and create emulation thread

// Call browser: Init():g_EmuThread(). See the BootManager.cpp file description for a complete call schedule.
THREAD_RETURN EmuThread(void *pArg)
{
	cpuRunloopQuit.Init();

	Common::SetCurrentThreadName("Emuthread - starting");
	const SCoreStartupParameter& _CoreParameter = SConfig::GetInstance().m_LocalCoreStartupParameter;

	CPluginManager &Plugins = CPluginManager::GetInstance();
	if (_CoreParameter.bLockThreads)
		Common::Thread::SetCurrentThreadAffinity(2); // Force to second core
 
	INFO_LOG(OSREPORT, "Starting core = %s mode", _CoreParameter.bWii ? "Wii" : "Gamecube");
	INFO_LOG(OSREPORT, "Dualcore = %s", _CoreParameter.bUseDualCore ? "Yes" : "No");

	HW::Init();	

	emuThreadGoing.Set();
 
	// Load the VideoPlugin
 	SVideoInitialize VideoInitialize;
	VideoInitialize.pGetMemoryPointer	= Memory::GetPointer;
	VideoInitialize.pSetPEToken			= PixelEngine::SetToken;
	VideoInitialize.pSetPEFinish		= PixelEngine::SetFinish;
	// This is first the m_Panel handle, then it is updated to have the new window handle
	VideoInitialize.pWindowHandle		= _CoreParameter.hMainWindow;
	VideoInitialize.pLog				= Callback_VideoLog;
	VideoInitialize.pSysMessage			= Host_SysMessage;
	VideoInitialize.pRequestWindowSize	= NULL; //Callback_VideoRequestWindowSize;
	VideoInitialize.pCopiedToXFB		= Callback_VideoCopiedToXFB;
	VideoInitialize.pPeekMessages       = NULL;
	VideoInitialize.pUpdateFPSDisplay   = NULL;
	VideoInitialize.pCPFifo             = (SCPFifoStruct*)&CommandProcessor::fifo;
	VideoInitialize.pUpdateInterrupts   = &(CommandProcessor::UpdateInterruptsFromVideoPlugin);
	VideoInitialize.pMemoryBase         = Memory::base;
	VideoInitialize.pKeyPress           = Callback_KeyPress;
	VideoInitialize.bWii                = _CoreParameter.bWii;
	VideoInitialize.bUseDualCore		= _CoreParameter.bUseDualCore;
	// May be needed for Stop and Start
	#ifdef SETUP_FREE_VIDEO_PLUGIN_ON_BOOT
		Plugins.FreeVideo();
	#endif
	Plugins.GetVideo()->Initialize(&VideoInitialize); // Call the dll
 
	// Under linux, this is an X11 Display, not an HWND!
	g_pWindowHandle = (HWND)VideoInitialize.pWindowHandle;
	Callback_PeekMessages = VideoInitialize.pPeekMessages;
	g_pUpdateFPSDisplay = VideoInitialize.pUpdateFPSDisplay;

    // Load and init DSPPlugin	
	DSPInitialize dspInit;
	dspInit.hWnd = g_pWindowHandle;
	dspInit.pARAM_Read_U8 = (u8  (__cdecl *)(const u32))DSP::ReadARAM; 
	dspInit.pARAM_Write_U8 = (void (__cdecl *)(const u8, const u32))DSP::WriteARAM; 
	dspInit.pGetARAMPointer = DSP::GetARAMPtr;
	dspInit.pGetMemoryPointer = Memory::GetPointer;
	dspInit.pLog = Callback_DSPLog;
	dspInit.pName = Callback_ISOName;
	dspInit.pDebuggerBreak = Callback_DebuggerBreak;
	dspInit.pGenerateDSPInterrupt = Callback_DSPInterrupt;
	dspInit.pGetAudioStreaming = AudioInterface::Callback_GetStreaming;
	dspInit.pEmulatorState = (int *)PowerPC::GetStatePtr();
	dspInit.bWii = _CoreParameter.bWii;
	dspInit.bOnThread = _CoreParameter.bDSPThread;
	// May be needed for Stop and Start
	#ifdef SETUP_FREE_DSP_PLUGIN_ON_BOOT
		Plugins.FreeDSP();
	#endif
	Plugins.GetDSP()->Initialize((void *)&dspInit);

	// Load and Init PadPlugin
	for (int i = 0; i < MAXPADS; i++)
	{			
		SPADInitialize PADInitialize;
		PADInitialize.hWnd = g_pWindowHandle;
		PADInitialize.pLog = Callback_PADLog;
		PADInitialize.padNumber = i;
		// This is may be needed to avoid a SDL problem
		//Plugins.FreeWiimote();
		// Check if we should init the plugin
		if (Plugins.OkayToInitPlugin(i))
		{
			Plugins.GetPad(i)->Initialize(&PADInitialize);

			// Check if joypad open failed, in that case try again
			if (PADInitialize.padNumber == -1)
			{
				Plugins.GetPad(i)->Shutdown();
				Plugins.FreePad(i);
				Plugins.GetPad(i)->Initialize(&PADInitialize);
			}
		}
	}

	// Load and Init WiimotePlugin - only if we are booting in wii mode	
	if (_CoreParameter.bWii)
	{
		SWiimoteInitialize WiimoteInitialize;
		WiimoteInitialize.hWnd = g_pWindowHandle;
		// Add the ISO Id
		WiimoteInitialize.ISOId = Ascii2Hex(_CoreParameter.m_strUniqueID);
		WiimoteInitialize.pLog = Callback_WiimoteLog;
		WiimoteInitialize.pWiimoteInput = Callback_WiimoteInput;
		// Wait for Wiiuse to find the number of connected Wiimotes
		Plugins.GetWiimote(0)->Initialize((void *)&WiimoteInitialize);
	}
 
	// The hardware is initialized.
	g_bHwInit = true;

	DisplayMessage("CPU: " + cpu_info.Summarize(), 8000);
	DisplayMessage(_CoreParameter.m_strFilename, 3000);
 
	// Load GCM/DOL/ELF whatever ... we boot with the interpreter core
	PowerPC::SetMode(PowerPC::MODE_INTERPRETER);
	CBoot::BootUp();
 
	if (g_pUpdateFPSDisplay != NULL)
        g_pUpdateFPSDisplay(("Loading " + _CoreParameter.m_strFilename).c_str());
 
	// Setup our core, but can't use dynarec if we are compare server
	if (_CoreParameter.bUseJIT && (!_CoreParameter.bRunCompareServer || _CoreParameter.bRunCompareClient))
		PowerPC::SetMode(PowerPC::MODE_JIT);
	else
		PowerPC::SetMode(PowerPC::MODE_INTERPRETER);
 
	// Update the window again because all stuff is initialized
	Host_UpdateDisasmDialog();
	Host_UpdateMainFrame();
 
	// This thread, after creating the EmuWindow, spawns a CPU thread,
	// then takes over and becomes the graphics thread
 
	// In single core mode, this thread is the CPU thread and also does the graphics.
 
	// Spawn the CPU thread
	Common::Thread *cpuThread = NULL;
 
	// ENTER THE VIDEO THREAD LOOP
	if (!_CoreParameter.bUseDualCore)
	{
#ifdef _WIN32
		cpuThread = new Common::Thread(CpuThread, pArg);
		// Common::SetCurrentThreadName("Idle thread");

		// TODO(ector) : investigate using GetMessage instead .. although
		// then we lose the powerdown check. ... unless powerdown sends a message :P
		while (PowerPC::GetState() != PowerPC::CPU_POWERDOWN)
		{
			if (Callback_PeekMessages)
				Callback_PeekMessages();
			Common::SleepCurrentThread(20);
		}
#else
		// In single-core mode, the Emulation main thread is also the CPU thread
		CpuThread(pArg);
#endif
	}
	else
	{
		Plugins.GetVideo()->Video_Prepare(); // wglMakeCurrent
		cpuThread = new Common::Thread(CpuThread, pArg);
		Common::SetCurrentThreadName("Video thread");

		Plugins.GetVideo()->Video_EnterLoop();
	}
#ifdef SETUP_TIMER_WAITING

	VideoThreadEvent2.TimerWait(EmuThreadEnd, 2);
	//INFO_LOG(CONSOLE, "Video loop [Video Thread]:   Stopped\n");
	return 0;
}

void EmuThreadEnd()
{
	CPluginManager &Plugins = CPluginManager::GetInstance();
	const SCoreStartupParameter& _CoreParameter = SConfig::GetInstance().m_LocalCoreStartupParameter;

	//INFO_LOG(CONSOLE, "Video loop [Video Thread]:   EmuThreadEnd [StopEnd:%i]\n", StopReachedEnd);

	//if (!VideoThreadEvent2.TimerWait(EmuThreadEnd, 2)) return;
	if (!VideoThreadEvent2.TimerWait(EmuThreadEnd, 2, StopReachedEnd) || !StopReachedEnd)
	{
		INFO_LOG(CONSOLE, "Stop [Video Thread]:   Waiting for Stop() and Video Loop to end...\n");
		return;
	}

	//INFO_LOG(CONSOLE, "EmuThreadEnd() will continue\n");

	/* There will be a few problems with the OpenGL ShutDown() after this, for example the "Release
	   Device Context Failed" error message */
#endif

	INFO_LOG(CONSOLE, "Stop [Video Thread]:   Stop() and Video Loop Ended\n");
	INFO_LOG(CONSOLE, "Stop [Video Thread]:   Shutting down HW and Plugins\n");

	// We have now exited the Video Loop and will shut down

	// Write message
	if (g_pUpdateFPSDisplay != NULL)
		g_pUpdateFPSDisplay("Stopping...");

#ifndef SETUP_TIMER_WAITING
	if (cpuThread)
	{
		// There is a CPU thread - join it.
#ifdef _WIN32
		cpuThread->WaitForDeath(5000);
#else
		cpuThread->WaitForDeath();
#endif
		delete cpuThread;
		// Returns after game exited
		cpuThread = NULL;
	}
#endif

	// The hardware is uninitialized
	g_bHwInit = false;
	
	HW::Shutdown();
	Plugins.ShutdownPlugins();

 	INFO_LOG(MASTER_LOG, "EmuThread exited");
	// The CPU should return when a game is stopped and cleanup should be done here, 
	// so we can restart the plugins (or load new ones) for the next game.
	if (_CoreParameter.hMainWindow == g_pWindowHandle)
		Host_UpdateMainFrame();
#ifdef SETUP_TIMER_WAITING
	EmuThreadReachedEnd = true;
	Host_UpdateGUI();
	INFO_LOG(CONSOLE, "Stop [Video Thread]:   Done\n");
	delete g_EmuThread;  // Wait for emuthread to close.
	g_EmuThread = 0;
#endif
#ifndef SETUP_TIMER_WAITING
	return 0;
#endif
}
 
 

// Set or get the running state

void SetState(EState _State)
{
	switch (_State)
	{
	case CORE_UNINITIALIZED:
        Stop();
		break;
	case CORE_PAUSE:
		CCPU::EnableStepping(true);  // Break
		break;
	case CORE_RUN:
		CCPU::EnableStepping(false);
		break;
	default:
		PanicAlert("Invalid state");
		break;
	}
}
 
EState GetState()
{
	if (g_bHwInit)
	{
		if (CCPU::IsStepping())
			return CORE_PAUSE;
		else
			return CORE_RUN;
	}
	return CORE_UNINITIALIZED;
}
 
// Save or recreate the emulation state
void SaveState() {
    State_Save(0);
}
 
void LoadState() {
    State_Load(0);
}

 
// --- Callbacks for plugins / engine ---
 

// Callback_VideoLog
// WARNING - THIS IS EXECUTED FROM VIDEO THREAD
void Callback_VideoLog(const TCHAR *_szMessage, int _bDoBreak)
{
	INFO_LOG(VIDEO, _szMessage);
}
 

// Callback_VideoCopiedToXFB
// WARNING - THIS IS EXECUTED FROM VIDEO THREAD
// We do not write to anything outside this function here
void Callback_VideoCopiedToXFB()
{ 
	#ifdef RERECORDING
		FrameUpdate();
	#endif

    SCoreStartupParameter& _CoreParameter = SConfig::GetInstance().m_LocalCoreStartupParameter;
	//count FPS
	static Common::Timer Timer;
	static u32 frames = 0;
	static u64 ticks = 0;
	static u64 idleTicks = 0;


	u32 targetfps = (SConfig::GetInstance().m_Framelimit)*5;
	static u64 old_frametime=0;
	u64 new_frametime;
	s16 wait_frametime;

	frames++;

	
	// Custom frame limiter
	// ŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻ
	if (targetfps > 0)
	{	
		new_frametime = Timer.GetTimeDifference() - old_frametime;
		old_frametime = Timer.GetTimeDifference();
		wait_frametime = (1000/targetfps) - (u16)new_frametime;
		if (targetfps < 35)
			wait_frametime--;
		if (wait_frametime > 0)
			Common::SleepCurrentThread(wait_frametime*2);
	}
	

		
	// Is it possible to calculate the CPU-GPU synced ticks for the dual core mode too?
	// And possible the idle skipping mode too?
	// ŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻŻ
	static int Diff = 0, DistOld = 0;
	Diff = CommandProcessor::fifo.CPReadWriteDistance - DistOld;
	// If the CPReadWriteDistance has increased since the last frame we assume the CPU has raced
	// ahead of the GPU and we adjust the ticks. Why multiply the difference with 700? I don't know,
	// please fix it if possible.
	if (Diff > 0) VideoInterface::SyncTicksProgress -= Diff * 700;
	DistOld = CommandProcessor::fifo.CPReadWriteDistance;
	

	if (Timer.GetTimeDifference() >= 1000)
	{
		// reset timer for framelimiter, placed here so no additional check for 1000ms is required -> don't delete please :)
		old_frametime = 0;

		// Time passed
		float t = (float)(Timer.GetTimeDifference()) / 1000.f;

		// Use extended or summary information. The summary information does not print the ticks data,
		// that's more of a debugging interest, it can always be optional of course if someone is interested.
		//#define EXTENDED_INFO
		#ifdef EXTENDED_INFO
			u64 newTicks = CoreTiming::GetTicks();
			u64 newIdleTicks = CoreTiming::GetIdleTicks();
	 
			s64 diff = (newTicks - ticks) / 1000000;
			s64 idleDiff = (newIdleTicks - idleTicks) / 1000000;
	 
			ticks = newTicks;
			idleTicks = newIdleTicks;	 
			
			float TicksPercentage = (float)diff / (float)(SystemTimers::GetTicksPerSecond() / 1000000) * 100;
		#endif

		float FPS = (float)frames / t;
		float FPS_To_VPS_Rate = ((float)FPS / VideoInterface::ActualRefreshRate);
		
		// For the sake of the dual core mode calculate an average to somewhat reduce the variations
		// in the FPS/VPS rate
		
		/**/
		if (_CoreParameter.bUseDualCore)
		{
			static std::vector <float> FPSVPSList;
			static float AverageOver = 5.0;
			if (FPSVPSList.size() == AverageOver) FPSVPSList.erase(FPSVPSList.begin());
			FPSVPSList.push_back(FPS_To_VPS_Rate);
			if (FPSVPSList.size() == AverageOver)
				FPS_To_VPS_Rate = MathFloatVectorSum(FPSVPSList) / AverageOver;
		}
		
		
		// Correct the FPS/VPS rate for temporary CPU-GPU timing variations. This rate can only be 1/Integer
		// so we set it to either 0.33, 0.5 or 1.0 depending on which it's closest to.
		/*
			1. Notice: This rate can currently not be calculated with any accuracy at all when idle skipping
			   is on (the suggested tick rate in that case is much to high in proportion to the actual FPS)
			2. When dual core is enabled the CommandProcessor allow some elasticity in the FPS/VPS rate. This
			   is especially noticable in Zelda TP who's FPS/VPS for me varies between 0.25 and 0.6 as a
			   result of this.
			3. PAL 50Hz games: Are 'patched' so that they still run at the correct speed. So if the NTSC 60Hz
			   version has a FPS/VPS of 0.5 the 50Hz game will run at 0.6.
		*/
		
		/**/
		if (FPS_To_VPS_Rate > 0 && FPS_To_VPS_Rate < ((1.0f/3.0f + 1.0f/2.0f)/2)) FPS_To_VPS_Rate = 1.0f/3.0f;
		else if (FPS_To_VPS_Rate > ((1.0f/3.0f + 1.0f/2.0f)/2) && FPS_To_VPS_Rate < ((1.0f/2.0f + 1.0f/1.0f)/2)) FPS_To_VPS_Rate = 1.0/2.0;
		else FPS_To_VPS_Rate = 1.0;	
		// PAL patch adjustment
		if (VideoInterface::TargetRefreshRate == 50) FPS_To_VPS_Rate = FPS_To_VPS_Rate * 1.2f;
		
		
		float TargetFPS = FPS_To_VPS_Rate * (float)VideoInterface::TargetRefreshRate;
		float FPSPercentage = (FPS / TargetFPS) * 100.0f;
		float VPSPercentage = (VideoInterface::ActualRefreshRate / (float)VideoInterface::TargetRefreshRate) * 100.0f;
		
		// Settings are shown the same for both extended and summary info
		std::string SSettings = StringFromFormat(" | Core: %s %s",
			#if defined(JITTEST) && JITTEST
				#ifdef _M_IX86
							_CoreParameter.bUseJIT ? "JIT32IL" : "Int32", 
				#else
							_CoreParameter.bUseJIT ? "JIT64IL" : "Int64", 
				#endif
			#else
				#ifdef _M_IX86
							_CoreParameter.bUseJIT ? "JIT32" : "Int32",
				#else
							_CoreParameter.bUseJIT ? "JIT64" : "Int64",
				#endif
			#endif
			_CoreParameter.bUseDualCore ? "DC" : "SC");
		// Show ~ to indicate that the value may be wrong because the ticks are wrong
		std::string IdleSkipMessage = "";
		if (_CoreParameter.bSkipIdle || _CoreParameter.bUseDualCore) IdleSkipMessage = "~";
		#ifdef EXTENDED_INFO
			std::string SFPS = StringFromFormat("FPS: %4.1f/%s%2.0f (%s%3.0f%% | %s%1.2f) VPS:%4.0f/%i (%3.0f%%)",
				FPS, IdleSkipMessage.c_str(), TargetFPS,
				IdleSkipMessage.c_str(), FPSPercentage, IdleSkipMessage.c_str(), FPS_To_VPS_Rate, 
				VideoInterface::ActualRefreshRate, VideoInterface::TargetRefreshRate, VPSPercentage);
			std::string STicks = StringFromFormat(" | CPU: %s%i MHz [Real: %i + IdleSkip: %i] / %i MHz (%s%3.0f%%)",
					IdleSkipMessage.c_str(),
					(int)(diff),
					(int)(diff - idleDiff),
					(int)(idleDiff),
					SystemTimers::GetTicksPerSecond() / 1000000,
					IdleSkipMessage.c_str(),
					TicksPercentage);
		// Summary information
		#else
			std::string SFPS = StringFromFormat("FPS: %4.1f/%s%2.0f (%s%3.0f%%)",
				FPS, IdleSkipMessage.c_str(), TargetFPS, IdleSkipMessage.c_str(), FPSPercentage);
			std::string STicks = ""; 
		#endif

			std::string SMessage = StringFromFormat("%s%s%s", SFPS.c_str(), SSettings.c_str(), STicks.c_str());

		// Show message
		if (g_pUpdateFPSDisplay != NULL)
			g_pUpdateFPSDisplay(SMessage.c_str()); 
		Host_UpdateStatusBar(SMessage.c_str());
		// Reset frame counter
		frames = 0;
        Timer.Update();
	}
}


// Callback_DSPLog
// WARNING - THIS MAY EXECUTED FROM DSP THREAD
	void Callback_DSPLog(const TCHAR* _szMessage, int _v)
{
	GENERIC_LOG(LogTypes::AUDIO, (LogTypes::LOG_LEVELS)_v, _szMessage);
}
 

// Callback_DSPInterrupt
// WARNING - THIS MAY EXECUTED FROM DSP THREAD
void Callback_DSPInterrupt()
{
	DSP::GenerateDSPInterruptFromPlugin(DSP::INT_DSP);
}
 

// Callback_PADLog 
//
void Callback_PADLog(const TCHAR* _szMessage)
{
	// FIXME add levels
	INFO_LOG(SERIALINTERFACE, _szMessage);
}
 

// Callback_ISOName: Let the DSP plugin get the game name
//
const char *Callback_ISOName()
{
    SCoreStartupParameter& params = SConfig::GetInstance().m_LocalCoreStartupParameter;
	if (params.m_strName.length() > 0)
		return params.m_strName.c_str();
	else	
          return "";
}


// Called from ANY thread!
void Callback_KeyPress(int key, bool shift, bool control)
{
	// 0x70 == VK_F1
	if (key >= 0x70 && key < 0x79) {
		// F-key
		int slot_number = key - 0x70 + 1;
		if (shift) {
			State_Save(slot_number);
		} else {
			State_Load(slot_number);
		}
	}
}
 
// Callback_WiimoteLog
//
void Callback_WiimoteLog(const TCHAR* _szMessage, int _v)
{
	GENERIC_LOG(LogTypes::WII_IPC_WIIMOTE, (LogTypes::LOG_LEVELS)_v, _szMessage);
}
 
// TODO: Get rid of at some point
const SCoreStartupParameter& GetStartupParameter() {
    return SConfig::GetInstance().m_LocalCoreStartupParameter;
}

} // end of namespace Core
