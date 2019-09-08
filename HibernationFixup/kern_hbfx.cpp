//
//  kern_hbfx.cpp
//  HibernationFixup
//
//  Copyright © 2017 lvs1974. All rights reserved.
//

#include <Library/LegacyIOService.h>
#include "LegacyRootDomain.h"

#include <Headers/kern_api.hpp>
#include <Headers/kern_file.hpp>
#include <Headers/kern_compat.hpp>
#include <Headers/kern_compression.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_rtc.hpp>

#include "kern_config.hpp"
#include "kern_hbfx.hpp"

#include <kern/clock.h>
#include "gmtime.h"


#define FILE_NVRAM_NAME                 "/nvram.plist"

// Only used in apple-driven callbacks
static HBFX *callbackHBFX = nullptr;

static const char *kextIOPCIFamilyPath[]   { "/System/Library/Extensions/IOPCIFamily.kext/IOPCIFamily" };
static const char *kextX86PlatformPlugin[] { "/System/Library/Extensions/IOPlatformPluginFamily.kext/Contents/PlugIns/X86PlatformPlugin.kext/Contents/MacOS/X86PlatformPlugin" };

static KernelPatcher::KextInfo kextList[] {
	{"com.apple.iokit.IOPCIFamily",        kextIOPCIFamilyPath,   arrsize(kextIOPCIFamilyPath),   {true}, {}, KernelPatcher::KextInfo::Unloaded},
	{"com.apple.driver.X86PlatformPlugin", kextX86PlatformPlugin, arrsize(kextX86PlatformPlugin), {true}, {}, KernelPatcher::KextInfo::Unloaded}
};

//==============================================================================

bool HBFX::init()
{
	callbackHBFX = this;

	lilu.onPatcherLoadForce(
	[](void *user, KernelPatcher &patcher) {
		callbackHBFX->processKernel(patcher);
	}, this);

	lilu.onKextLoadForce(kextList, arrsize(kextList),
	[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		callbackHBFX->processKext(patcher, index, address, size);
	}, this);


	return true;
}

//==============================================================================

void HBFX::deinit()
{
	nvstorage.deinit();
}

//==============================================================================

IOReturn HBFX::IOHibernateSystemSleep(void)
{
	IOReturn result = FunctionCast(IOHibernateSystemSleep, callbackHBFX->orgIOHibernateSystemSleep)();
	DBGLOG("HBFX", "IOHibernateSystemSleep is called, result is: %x", result);

	uint32_t ioHibernateState = kIOHibernateStateInactive;
	OSData *data = OSDynamicCast(OSData, IOService::getPMRootDomain()->getProperty(kIOHibernateStateKey));
	if (data != nullptr)
	{
		ioHibernateState = *((uint32_t *)data->getBytesNoCopy());
		DBGLOG("HBFX", "Current hibernate state from IOPMRootDomain is: %d", ioHibernateState);
	}

	if (result == KERN_SUCCESS || ioHibernateState == kIOHibernateStateHibernating)
	{
		OSData *rtc = OSDynamicCast(OSData, IOService::getPMRootDomain()->getProperty(kIOHibernateRTCVariablesKey));
		if (rtc && !callbackHBFX->nvstorage.exists(kIOHibernateRTCVariablesKey))
		{
			if (!callbackHBFX->nvstorage.write(kIOHibernateRTCVariablesKey, rtc, NVStorage::OptRaw))
				SYSLOG("HBFX", "IOHibernateRTCVariablesKey can't be written to NVRAM.");
		}

		OSData *smc = OSDynamicCast(OSData, IOService::getPMRootDomain()->getProperty(kIOHibernateSMCVariablesKey));
		if (smc && !callbackHBFX->nvstorage.exists(kIOHibernateSMCVariablesKey))
		{
			if (!callbackHBFX->nvstorage.write(kIOHibernateSMCVariablesKey, smc, NVStorage::OptRaw))
				SYSLOG("HBFX", "IOHibernateSMCVariablesKey can't be written to NVRAM.");
		}

		if (ADDPR(hbfx_config).dumpNvram)
		{
			uint32_t size;
			if (uint8_t *buf = callbackHBFX->nvstorage.read(kGlobalBoot0082Key, size, NVStorage::OptRaw))
			{
				if (!callbackHBFX->nvstorage.write(kBoot0082Key, buf, size, NVStorage::OptRaw))
					SYSLOG("HBFX", "%s can't be written!", kBoot0082Key);
				Buffer::deleter(buf);
			}
			else
				SYSLOG("HBFX", "Variable %s can't be found!", kBoot0082Key);

			if (uint8_t *buf = callbackHBFX->nvstorage.read(kGlobalBootNextKey, size, NVStorage::OptRaw))
			{
				if (!callbackHBFX->nvstorage.write(kBootNextKey, buf, size, NVStorage::OptRaw))
					SYSLOG("HBFX", "%s can't be written!", kBootNextKey);
				Buffer::deleter(buf);
			}
			else
				SYSLOG("HBFX", "Variable %s can't be found!", kBootNextKey);

			callbackHBFX->nvstorage.save(FILE_NVRAM_NAME);

			if (callbackHBFX->sync)
				callbackHBFX->sync(kernproc, nullptr, nullptr);

			callbackHBFX->nvstorage.remove(kBoot0082Key);
			callbackHBFX->nvstorage.remove(kBootNextKey);
		}
	}

	return result;
}

//==============================================================================

IOReturn HBFX::IOHibernateSystemWake(void)
{
	callbackHBFX->sleepServiceWake = false;
	callbackHBFX->wakeCalendarSet  = false;
	
	IOReturn result = FunctionCast(IOHibernateSystemWake, callbackHBFX->orgIOHibernateSystemWake)();
	DBGLOG("HBFX", "IOHibernateSystemWake is called, result is: %x", result);
	
	OSString * wakeType = OSDynamicCast(OSString, IOService::getPMRootDomain()->getProperty(kIOPMRootDomainWakeTypeKey));
	OSString * wakeReason = OSDynamicCast(OSString, IOService::getPMRootDomain()->getProperty(kIOPMRootDomainWakeReasonKey));
	if (result == KERN_SUCCESS && wakeType && wakeReason)
	{
		DBGLOG("HBFX", "IOHibernateSystemWake: wake type is: %s", wakeType->getCStringNoCopy());
		DBGLOG("HBFX", "IOHibernateSystemWake: wake reason is: %s", wakeReason->getCStringNoCopy());
	
		if (strstr(wakeReason->getCStringNoCopy(), "RTC", strlen("RTC")) &&
			(!strcmp(wakeType->getCStringNoCopy(), kIOPMRootDomainWakeTypeSleepService) || !strcmp(wakeType->getCStringNoCopy(), kIOPMRootDomainWakeTypeMaintenance))
			)
		{
			callbackHBFX->sleepServiceWake = true;
			DBGLOG("HBFX", "IOHibernateSystemWake: Maintenance/SleepService wake");
		}
	}
	
	return result;
}

//==============================================================================

IOReturn HBFX::setMaintenanceWakeCalendar(IOPMrootDomain* that, IOPMCalendarStruct * calendar)
{
	DBGLOG("HBFX", "Calendar time %02d.%02d.%04d %02d:%02d:%02d, selector: %d", calendar->day, calendar->month, calendar->year,
		   calendar->hour, calendar->minute, calendar->second, calendar->selector);
	
	callbackHBFX->wakeCalendarSet = false;
	bool pmset_non_default_mode = (callbackHBFX->latestHibernateMode != (kIOHibernateModeOn | kIOHibernateModeSleep));
	if (callbackHBFX->sleepPhase == 0 && callbackHBFX->latestStandbyDelay != 0 && !pmset_non_default_mode)
	{
		struct tm tm;
		struct timeval tv;
		microtime(&tv);
		
		gmtime_r(tv.tv_sec, &tm);
		DBGLOG("HBFX", "Current time: %02d.%02d.%04d %02d:%02d:%02d", tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);
		
		tv.tv_sec += callbackHBFX->latestStandbyDelay;
		gmtime_r(tv.tv_sec, &tm);
		DBGLOG("HBFX", "Postpone wake to: %02d.%02d.%04d %02d:%02d:%02d", tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec);
		
		*calendar = { static_cast<UInt32>(tm.tm_year), static_cast<UInt8>(tm.tm_mon), static_cast<UInt8>(tm.tm_mday),
			static_cast<UInt8>(tm.tm_hour), static_cast<UInt8>(tm.tm_min), static_cast<UInt8>(tm.tm_sec), calendar->selector };
		callbackHBFX->wakeCalendarSet = true;
	}
	
	return FunctionCast(setMaintenanceWakeCalendar, callbackHBFX->orgSetMaintenanceWakeCalendar)(that, calendar);
}

//==============================================================================

IOReturn HBFX::X86PlatformPlugin_sleepPolicyHandler(void * target, IOPMSystemSleepPolicyVariables * vars, IOPMSystemSleepParameters * params)
{
	callbackHBFX->forceHibernate = false;
	
	IOReturn result = FunctionCast(X86PlatformPlugin_sleepPolicyHandler, callbackHBFX->orgSleepPolicyHandler)(target, vars, params);
	if (result != KERN_SUCCESS)
		SYSLOG("HBFX", "orgSleepPolicyHandler returned error 0x%x", result);
	else {
		DBGLOG("HBFX", "X86PlatformPlugin_sleepPolicyHandler sleepReason: %d, sleepPhase: %d, hibernateMode: %d, sleepType: %d",
			   vars->sleepReason, vars->sleepPhase, vars->hibernateMode, params->sleepType);
		DBGLOG("HBFX", "X86PlatformPlugin_sleepPolicyHandler standbyDelay: %d, standbyTimer: %d, poweroffDelay: %d, poweroffTimer: %d",
			   vars->standbyDelay, vars->standbyTimer, vars->poweroffDelay, vars->poweroffTimer);
		DBGLOG("HBFX", "X86PlatformPlugin_sleepPolicyHandler ecWakeTimer: %d, ecPoweroffTimer: %d", params->ecWakeTimer, params->ecPoweroffTimer);
		callbackHBFX->latestHibernateMode = vars->hibernateMode;
		callbackHBFX->latestStandbyDelay  = vars->standbyDelay;
		callbackHBFX->sleepPhase          = vars->sleepPhase;
		if (vars->sleepPhase == 0)
		{
			callbackHBFX->sleepFactors = vars->sleepFactors;
			callbackHBFX->sleepReason  = vars->sleepReason;
			callbackHBFX->sleepType    = params->sleepType;
			callbackHBFX->sleepFlags   = params->sleepFlags;
		}
		
		while ((params->sleepType == kIOPMSleepTypeDeepIdle || params->sleepType == kIOPMSleepTypeStandby) && callbackHBFX->power_source)
		{
			auto autoHibernateMode = ADDPR(hbfx_config).autoHibernateMode;
			bool whenLidIsClosed = (autoHibernateMode & Configuration::WhenLidIsClosed);
			bool whenExternalPowerIsDisconnected = (autoHibernateMode & Configuration::WhenExternalPowerIsDisconnected);
			bool whenBatteryIsNotCharging = (autoHibernateMode & Configuration::WhenBatteryIsNotCharging);
			bool whenBatteryIsAtWarnLevel = (autoHibernateMode & Configuration::WhenBatteryIsAtWarnLevel);
			bool whenBatteryAtCriticalLevel = (autoHibernateMode & Configuration::WhenBatteryAtCriticalLevel);
			int  minimalRemainingCapacity = ((autoHibernateMode & 0xF00) >> 8);
	
			if (callbackHBFX->power_source->batteryInstalled()) {
				if (whenExternalPowerIsDisconnected && callbackHBFX->power_source->externalConnected()) {
					DBGLOG("HBFX", "Auto hibernate: external is connected, do not force to hibernate");
					break;
				}
	
				if (whenBatteryIsNotCharging && callbackHBFX->power_source->isCharging()) {
					DBGLOG("HBFX", "Auto hibernate: battery is charging, do not force to hibernate");
					break;
				}
				
				if (!callbackHBFX->power_source->isCharging())
				{
					DBGLOG("HBFX", "Auto hibernate: warning level = %d, critical level = %d, capacity remaining = %d, minimal capaity = %d",
						   callbackHBFX->power_source->atWarnLevel(), callbackHBFX->power_source->atCriticalLevel(),
						   callbackHBFX->power_source->capacityPercentRemaining(), minimalRemainingCapacity);
					if (whenBatteryIsAtWarnLevel && callbackHBFX->power_source->atWarnLevel()) {
						DBGLOG("HBFX", "Auto hibernate: Battery is at warning level, capacity remaining: %d, force to hibernate", callbackHBFX->power_source->capacityPercentRemaining());
						callbackHBFX->forceHibernate = true;
					}
		
					if (whenBatteryAtCriticalLevel && callbackHBFX->power_source->atCriticalLevel()) {
						DBGLOG("HBFX", "Auto hibernate: battery is at critical level, capacity remaining: %d, force to hibernate", callbackHBFX->power_source->capacityPercentRemaining());
						callbackHBFX->forceHibernate = true;
					}
					
					if (!callbackHBFX->forceHibernate && (whenBatteryIsAtWarnLevel || whenBatteryAtCriticalLevel) && minimalRemainingCapacity != 0 &&
						callbackHBFX->power_source->capacityPercentRemaining() <= minimalRemainingCapacity)
					{
						DBGLOG("HBFX", "Auto hibernate: capacity remaining: %d less than minimal: %d, force to hibernate", callbackHBFX->power_source->capacityPercentRemaining());
						callbackHBFX->forceHibernate = true;
					}
				}
			}

			bool lidIsOpen = OSDynamicCast(OSBoolean, IOService::getPMRootDomain()->getProperty(kAppleClamshellStateKey)) != kOSBooleanTrue;
			if (!callbackHBFX->forceHibernate && whenLidIsClosed && lidIsOpen) {
				DBGLOG("HBFX", "Auto hibernate: clamshell is open, do not force to hibernate");
				break;
			}
			
			if (callbackHBFX->sleepPhase == 1 && (!callbackHBFX->wakeCalendarSet || callbackHBFX->forceHibernate))
			{
				vars->sleepFactors = kIOPMSleepFactorStandbyForced | kIOPMSleepFactorStandbyNoDelay;
				vars->sleepReason  = kIOPMSleepReasonLowPower;
				params->sleepType  = kIOPMSleepTypeStandby;
				params->sleepFlags = kIOPMSleepFlagHibernate;
				DBGLOG("HBFX", "Auto hibernate: sleep phase 1, set hibernate values");
			}
			else if (callbackHBFX->sleepPhase == 2)
			{
				if (callbackHBFX->forceHibernate || !callbackHBFX->wakeCalendarSet || callbackHBFX->sleepServiceWake)
				{
					vars->sleepFactors = kIOPMSleepFactorStandbyForced | kIOPMSleepFactorStandbyNoDelay;
					vars->sleepReason  = kIOPMSleepReasonLowPower;
					params->sleepType  = kIOPMSleepTypeStandby;
					params->sleepFlags = kIOPMSleepFlagHibernate;
					DBGLOG("HBFX", "Auto hibernate: sleep phase 2, hibernate now");
				}
				else
				{
					vars->sleepFactors = callbackHBFX->sleepFactors;
					vars->sleepReason  = callbackHBFX->sleepReason;
					params->sleepType  = callbackHBFX->sleepType;
					params->sleepFlags = callbackHBFX->sleepFlags;
					DBGLOG("HBFX", "Auto hibernate: sleep phase 2, postpone hibernate");
				}
				
				callbackHBFX->sleepServiceWake = false;
				callbackHBFX->wakeCalendarSet  = false;
			}
			
			DBGLOG("HBFX", "Auto hibernate: force hibernate...");
			callbackHBFX->forceHibernate = true;
			break;
		}
	}

	return result;
}

//==============================================================================

int HBFX::packA(char *inbuf, uint32_t length, uint32_t buflen)
{
	char key[128];
	unsigned int bufpos = 0;
	if (callbackHBFX->orgPackA)
		bufpos = FunctionCast(packA, callbackHBFX->orgPackA)(inbuf, length, buflen);
	
	if (callbackHBFX->ml_at_interrupt_context && !callbackHBFX->ml_at_interrupt_context())
	{
		const bool interrupts_enabled = callbackHBFX->ml_get_interrupts_enabled();
		if (!interrupts_enabled)
			callbackHBFX->ml_set_interrupts_enabled(TRUE);
		if (callbackHBFX->ml_get_interrupts_enabled())
		{
			int counter = 10;
			const bool preemption_enabled = callbackHBFX->preemption_enabled();
			while (!callbackHBFX->preemption_enabled() && --counter >= 0)
			{
				callbackHBFX->enable_preemption();
				IOSleep(1);
			}
			
			if (callbackHBFX->preemption_enabled())
			{
				unsigned int pi_size = bufpos ? bufpos : length;
				const unsigned int max_size = 768;
				counter = 0;
				while (pi_size > 0)
				{
					unsigned int part_size = (pi_size > max_size) ? max_size : pi_size;
					snprintf(key, sizeof(key), "AAPL,PanicInfo%04d", counter++);
					callbackHBFX->nvstorage.write(key, reinterpret_cast<const uint8_t*>(inbuf), part_size, NVStorage::OptRaw);
					pi_size -= part_size;
					inbuf += part_size;
				}
				
				callbackHBFX->nvstorage.save(FILE_NVRAM_NAME);
				callbackHBFX->sync(kernproc, nullptr, nullptr);
			}
			
			if (!preemption_enabled)
				callbackHBFX->disable_preemption();
			if (!interrupts_enabled)
				callbackHBFX->ml_set_interrupts_enabled(FALSE);
		}
	}
	
	return bufpos;
}

//==============================================================================

IOReturn HBFX::restoreMachineState(IOService *that, IOOptionBits options, IOService * device)
{
	if (kMachineRestoreDehibernate & options)
		callbackHBFX->correct_pci_config_command = true;

	IOReturn result = FunctionCast(restoreMachineState, callbackHBFX->orgRestoreMachineState)(that, options, device);

	if (kMachineRestoreDehibernate & options)
		callbackHBFX->correct_pci_config_command = false;
	
	return result;
}

//==============================================================================

void HBFX::extendedConfigWrite16(IOService *that, UInt64 offset, UInt16 data)
{
	if (callbackHBFX->correct_pci_config_command && offset == WIOKit::PCIRegister::kIOPCIConfigCommand)
	{
		if (strlen(ADDPR(hbfx_config).ignored_device_list) == 0 || strstr(ADDPR(hbfx_config).ignored_device_list, that->getName()) != nullptr)
		{
			if (!(data & kIOPCICommandMemorySpace))
			{
				DBGLOG("HBFX", "HBFX will add flag kIOPCICommandMemorySpace for deivce %s, offset = %08llX, data = %04X", that->getName(), offset, data);
				data |= kIOPCICommandMemorySpace;
			}
		}
	}

	FunctionCast(extendedConfigWrite16, callbackHBFX->orgExtendedConfigWrite16)(that, offset, data);
}

//==============================================================================
//
// Find service by name in a specified registry plane (gIO80211FamilyPlane or gIOServicePlane)
//
IOService* LIBKERN_RETURNS_NOT_RETAINED findService(const IORegistryPlane* plane, const char *service_name)
{
	IOService            * service = 0;
	IORegistryIterator   * iter = IORegistryIterator::iterateOver(plane, kIORegistryIterateRecursively);
	OSOrderedSet         * all = 0;
	
	if ( iter)
	{
		do
		{
			if (all)
				all->release();
			all = iter->iterateAll();
		}
		while (!iter->isValid());
		iter->release();
		
		if (all)
		{
			while ((service = OSDynamicCast(IOService, all->getFirstObject())))
			{
				all->removeObject(service);
				if (strcmp(service->getName(), service_name) == 0)
					break;
			}
			
			all->release();
		}
	}
	
	return service;
}

//==============================================================================

void HBFX::processKernel(KernelPatcher &patcher)
{
	bool auto_hibernate_mode_on = (ADDPR(hbfx_config).autoHibernateMode & Configuration::EnableAutoHibernation);
	if (!ADDPR(hbfx_config).dumpNvram && emuVariableIsDetected())
		ADDPR(hbfx_config).dumpNvram = true;
	bool nvram_patches_required = (ADDPR(hbfx_config).dumpNvram == true || !checkRTCExtendedMemory());
	if (!nvram_patches_required)
	{
		DBGLOG("HBFX", "all nvram kernel patches will be skipped since the second bank of RTC memory is available");
		if (!auto_hibernate_mode_on)
			return;
	}
	
	if (!(progressState & ProcessingState::KernelRouted))
	{
		if (auto_hibernate_mode_on) {
			KernelPatcher::RouteRequest requests[] = {
				{"__ZN14IOPMrootDomain26setMaintenanceWakeCalendarEPK18IOPMCalendarStruct", setMaintenanceWakeCalendar, orgSetMaintenanceWakeCalendar},
				{"_IOHibernateSystemWake", IOHibernateSystemWake, orgIOHibernateSystemWake}
			};
			if (!patcher.routeMultiple(KernelPatcher::KernelID, requests, arrsize(requests)))
				SYSLOG("HBFX", "patcher.routeMultiple for %s is failed with error %d", requests[0].symbol, patcher.getError());
		}
		
		if (nvram_patches_required && initialize_nvstorage()) {
			KernelPatcher::RouteRequest request	{"_IOHibernateSystemSleep", IOHibernateSystemSleep, orgIOHibernateSystemSleep};
			if (!patcher.routeMultiple(KernelPatcher::KernelID, &request, 1))
				SYSLOG("HBFX", "patcher.routeMultiple for %s is failed with error %d", request.symbol, patcher.getError());

			ml_at_interrupt_context = reinterpret_cast<t_ml_at_interrupt_context>(patcher.solveSymbol(KernelPatcher::KernelID, "_ml_at_interrupt_context"));
			if (!ml_at_interrupt_context)
			{
				SYSLOG("HBFX", "failed to resolve _ml_at_interrupt_context %d", patcher.getError());
				patcher.clearError();
			}
			
			ml_get_interrupts_enabled = reinterpret_cast<t_ml_get_interrupts_enabled>(patcher.solveSymbol(KernelPatcher::KernelID, "_ml_get_interrupts_enabled"));
			if (!ml_get_interrupts_enabled)
			{
				SYSLOG("HBFX", "failed to resolve _ml_get_interrupts_enabled %d", patcher.getError());
				patcher.clearError();
			}

			ml_set_interrupts_enabled = reinterpret_cast<t_ml_set_interrupts_enabled>(patcher.solveSymbol(KernelPatcher::KernelID, "_ml_set_interrupts_enabled"));
			if (!ml_set_interrupts_enabled)
			{
				SYSLOG("HBFX", "failed to resolve _ml_set_interrupts_enabled %d", patcher.getError());
				patcher.clearError();
			}

			if (ADDPR(hbfx_config).dumpNvram)
			{
				sync = reinterpret_cast<t_sync>(patcher.solveSymbol(KernelPatcher::KernelID, "_sync"));
				if (!sync) {
					SYSLOG("HBFX", "failed to resolve _sync %d", patcher.getError());
					patcher.clearError();
				}
				
				preemption_enabled = reinterpret_cast<t_preemption_enabled>(patcher.solveSymbol(KernelPatcher::KernelID, "_preemption_enabled"));
				if (!preemption_enabled) {
					SYSLOG("HBFX", "failed to resolve _preemption_enabled %d", patcher.getError());
					patcher.clearError();
				}
				
				enable_preemption = reinterpret_cast<t_enable_preemption>(patcher.solveSymbol(KernelPatcher::KernelID, "__enable_preemption"));
				if (!enable_preemption) {
					SYSLOG("HBFX", "failed to resolve __enable_preemption %d", patcher.getError());
					patcher.clearError();
				}
				
				disable_preemption = reinterpret_cast<t_disable_preemption>(patcher.solveSymbol(KernelPatcher::KernelID, "__disable_preemption"));
				if (!disable_preemption) {
					SYSLOG("HBFX", "failed to resolve __disable_preemption %d", patcher.getError());
					patcher.clearError();
				}
				
				if (sync && preemption_enabled && enable_preemption && disable_preemption && ml_at_interrupt_context &&
					ml_get_interrupts_enabled && ml_set_interrupts_enabled)
				{
					KernelPatcher::RouteRequest request {"_packA", packA, orgPackA};
					patcher.routeMultiple(KernelPatcher::KernelID, &request, 1);
				}
			}
		}
		
		progressState |= ProcessingState::KernelRouted;
	}

	// Ignore all the errors for other processors
	patcher.clearError();
}

//==============================================================================

void HBFX::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
	if (!ADDPR(hbfx_config).dumpNvram && emuVariableIsDetected())
		ADDPR(hbfx_config).dumpNvram = true;
	bool nvram_patches_required = (ADDPR(hbfx_config).dumpNvram == true || !checkRTCExtendedMemory());
	if (nvram_patches_required)
		initialize_nvstorage();
	
	if (progressState != ProcessingState::EverythingDone)
	{
		bool auto_hibernate_mode_on = (ADDPR(hbfx_config).autoHibernateMode & Configuration::EnableAutoHibernation);
		
		for (size_t i = 0; i < arrsize(kextList); i++)
		{
			if (kextList[i].loadIndex == index)
			{
				if (ADDPR(hbfx_config).patchPCIFamily && i == 0 && !(progressState & ProcessingState::IOPCIFamilyRouted))
				{
					KernelPatcher::RouteRequest requests[] {
						{"__ZN11IOPCIBridge19restoreMachineStateEjP11IOPCIDevice", restoreMachineState, orgRestoreMachineState},
						{"__ZN11IOPCIDevice21extendedConfigWrite16Eyt", extendedConfigWrite16, orgExtendedConfigWrite16},
					};

					if (!patcher.routeMultiple(index, requests, address, size))
						SYSLOG("HBFX", "patcher.routeMultiple for %s is failed with error %d", "IOPCIBridge", patcher.getError());
					progressState |= ProcessingState::IOPCIFamilyRouted;
				}
				
				if (auto_hibernate_mode_on && i == 1 && !(progressState & ProcessingState::X86PluginRouted))
				{
					auto matching = IOService::serviceMatching("IOPMPowerSource");
					if (matching) {
						auto matching_copy = IOService::copyMatchingService(matching);
						power_source = OSDynamicCast(IOPMPowerSource, matching_copy);
						matching->release();
						if (!power_source) {
							SYSLOG("HBFX", "failed to get IOPMPowerSource");
							OSSafeReleaseNULL(matching_copy);
						}
					} else {
						SYSLOG("HBFX", "failed to allocate IOPMPowerSource service matching");
					}
					
					if (power_source) {
						KernelPatcher::RouteRequest requests[] {
							{"__ZN17X86PlatformPlugin18sleepPolicyHandlerEPK30IOPMSystemSleepPolicyVariablesP25IOPMSystemSleepParameters", X86PlatformPlugin_sleepPolicyHandler, orgSleepPolicyHandler}
						};
						
						if (!patcher.routeMultiple(index, requests, address, size))
							SYSLOG("HBFX", "patcher.routeMultiple for %s is failed with error %d", "X86PlatformPlugin", patcher.getError());
						progressState |= ProcessingState::X86PluginRouted;
					}
				}
		    }
		}
	}
}

//==============================================================================

bool HBFX::checkRTCExtendedMemory()
{
	bool result = false;
	RTCStorage rtc_storage;
	if (rtc_storage.init())
	{
		result = rtc_storage.checkExtendedMemory();
		rtc_storage.deinit();
	}
	
	return result;
}

//==============================================================================

bool HBFX::initialize_nvstorage()
{
	static bool nvstorage_initialized = false;
	if (!nvstorage_initialized)
	{
		if (nvstorage.init())
		{
			DBGLOG("HBFX", "NVStorage was initialized");
			
			nvstorage_initialized = true;
			
#if defined(DEBUG) && defined(DEBUG_NVSTORAGE)
			// short NVStorage test
			uint8_t value[] = {0x01, 0x02, 0x03, 0x04, 0xFF, 0x04, 0x03, 0x02, 0x01,
							   0x01, 0x02, 0x03, 0x04, 0xFF, 0x04, 0x03, 0x02, 0x01,
							   0x01, 0x02, 0x03, 0x04, 0xFF, 0x04, 0x03, 0x02, 0x01,
							   0x01, 0x02, 0x03, 0x04, 0xFF, 0x04, 0x03, 0x02, 0x01,
							   0x01, 0x02, 0x03, 0x04, 0xFF, 0x04, 0x03, 0x02, 0x01};
			
			uint8_t enckey[] = {0xFF, 0x10, 0x08, 0x04, 0x02, 0x05, 0x09};
			uint32_t size = 0, dstlen = 1024;
			uint8_t *buf, *compressed_data, *decompressed_data;
			
			PANIC_COND((compressed_data = Compression::compress(Compression::ModeLZSS, dstlen, value, sizeof(value), nullptr)) == nullptr, "HBFX", "Compression::compress failed");
			PANIC_COND((decompressed_data = Compression::decompress(Compression::ModeLZSS, sizeof(value), compressed_data, dstlen)) == nullptr, "HBFX", "Compression::decompress failed");
			PANIC_COND(memcmp(decompressed_data, value, sizeof(value)) != 0, "HBFX", "memory is different from original");
			Buffer::deleter(decompressed_data);
			Buffer::deleter(compressed_data);
			
			dstlen = sizeof(value);
			PANIC_COND((compressed_data = nvstorage.compress(value, dstlen)) == nullptr, "HBFX", "NVStorage.compress failed");
			PANIC_COND((decompressed_data = nvstorage.decompress(compressed_data, dstlen)) == nullptr, "HBFX", "NVStorage.decompress failed");
			PANIC_COND(memcmp(decompressed_data, value, sizeof(value)) != 0, "HBFX", "memory is different from original");
			Buffer::deleter(decompressed_data);
			Buffer::deleter(compressed_data);
			
			const char* key = "NVStorageTestVar1";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptChecksum, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed");
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptAuto, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar2";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptChecksum | NVStorage::OptCompressed, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed");
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptAuto, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar3";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptChecksum | NVStorage::OptEncrypted, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed for %s", key);
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptAuto, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar4";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptChecksum | NVStorage::OptEncrypted | NVStorage::OptCompressed, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed for %s", key);
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptAuto, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar5";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptAuto, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed for %s", key);
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptAuto, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar6";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptCompressed, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed for %s", key);
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptAuto, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar7";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptEncrypted, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed for %s", key);
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptAuto, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar8";
			PANIC_COND(!nvstorage.write(key, value, sizeof(value), NVStorage::OptRaw, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed for %s", key);
			PANIC_COND((buf = nvstorage.read(key, size, NVStorage::OptRaw, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(size != sizeof(value), "HBFX", "read returned failed size for %s", key);
			PANIC_COND(memcmp(buf, value, sizeof(value)) != 0, "HBFX", "memory is different from original for %s", key);
			nvstorage.remove(key);
			
			key = "NVStorageTestVar9";
			OSData *data = OSData::withBytes("some string to be written in nvram", 32);
			OSData *newdata;
			PANIC_COND(!nvstorage.write(key, data, NVStorage::OptRaw, enckey), "HBFX", "write failed for %s", key);
			PANIC_COND(!nvstorage.exists(key), "HBFX", "exists failed for %s", key);
			PANIC_COND((newdata = nvstorage.read(key, NVStorage::OptRaw, enckey)) == nullptr, "HBFX", "read failed for %s", key);
			PANIC_COND(!newdata->isEqualTo(data), "HBFX", "memory is different from original for %s", key);
			data->release();
			newdata->release();

			DBGLOG("HBFX", "tests were finished");
#endif
		}
		else
		{
			SYSLOG("HBFX", "failed to initialize NVStorage");
		}
	}
	
	return nvstorage_initialized;
}

//==============================================================================

bool HBFX::emuVariableIsDetected()
{
	IOService* nvram_service = findService(gIOServicePlane, "AppleEFINVRAM");
	if (nvram_service != nullptr)
	{
		auto emu_variable  = OSDynamicCast(OSData, nvram_service->getProperty("EmuVariableUefiPresent"));
		if (emu_variable != nullptr && emu_variable->isEqualTo("Yes", 3))
		{
			DBGLOG("HBFX", "EmuVariableUefiPresent is detected");
			return true;
		}
		else
			DBGLOG("HBFX", "EmuVariableUefiPresent is not detected");
	}
	else
		SYSLOG("HBFX", "AppleEFINVRAM is not found");
	
	return false;
}
