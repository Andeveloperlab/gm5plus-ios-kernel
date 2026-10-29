/*
 * MiniIOWatchdog: stand-in for Apple's IOWatchdog driver.
 *
 * watchdogd (a critical launchd daemon with _PanicOnCrash) opens
 * IOServiceMatching("IOWatchdog") with type 1, and searches for an IOService
 * with "IOPMUPrimary" = 1 to write device boot stages.
 * If either is missing, watchdogd panics the system.
 *
 * This driver registers IOWatchdog with IOPMUPrimary=1 and satisfies
 * all check-in calls safely without ever firing.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

class IOWatchdog : public IOService {
	OSDeclareDefaultStructors(IOWatchdog);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdog, IOService);

IOReturn
IOWatchdog::setProperties(__unused OSObject *properties)
{
	return kIOReturnSuccess;
}

bool
IOWatchdog::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOWatchdogUserClient");
	setProperty("IOPMUPrimary", 1ULL, 32);
	registerService();
	IOLog("Hakios WDT: IOWatchdog registered (accepts check-ins, never fires)\n");
	return true;
}

class IOWatchdogUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOWatchdogUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOWatchdogUserClient, IOUserClient);

IOReturn
IOWatchdogUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

static uint64_t wdt_logged;       /* selectors already logged (< 64) */

IOReturn
IOWatchdogUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		IOByteCount len = args->structureOutputDescriptor->getLength();
		for (IOByteCount off = 0; off < len; off += 8) {
			static const uint64_t zero = 0;
			args->structureOutputDescriptor->writeBytes(off, &zero, len - off < 8 ? len - off : 8);
		}
	} else if (args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}

	if (selector >= 64 || !(wdt_logged & (1ULL << selector))) {
		if (selector < 64) {
			wdt_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("Hakios WDT: %s sel %u in %u/%u out %u/%u -> ok\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

void
MiniIOWatchdogStart(void)
{
	IOService * resources = IOService::getResourceService();
	IOWatchdog * wdt = OSTypeAlloc(IOWatchdog);

	if (resources == NULL || wdt == NULL || !wdt->init() || !wdt->attach(resources)) {
		OSSafeReleaseNULL(wdt);
		IOLog("MiniIOWatchdog: cannot create service\n");
		return;
	}
	if (!wdt->start(resources)) {
		wdt->detach(resources);
	}
	wdt->release();
}
