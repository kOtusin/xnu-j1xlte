/* J1 DW-MMC block storage device (read-only PIO). HD2-architecture port.
 * Exposes SD card as IOBlockStorageDevice so IOS7LeoHFSFile (J1 guard)
 * can locate IOS7/ROOTFS.HFS via fat_locator. */
#if defined(BOARD_CONFIG_EXYNOS3475_J1XLTE)
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorage.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSUnserialize.h>
#include <kern/debug.h>
#include "j1_sdmmc.h"
#include "IOS7LabStoragePersonalities.h"
extern "C" {
#include <mach/mach_time.h>
}
class IOS7J1SDMMC : public IOBlockStorageDevice {
 OSDeclareDefaultStructors(IOS7J1SDMMC)
 IOMemoryDescriptor *mmio; IOMemoryMap *map; volatile UInt32 *regs;
 IOLock *mutex; struct j1_sd_card card; bool initialized;
 static uint32_t readMMIO(void *,unsigned);
 static void writeMMIO(void *,unsigned,uint32_t);
 static int delayUS(void *,uint32_t);
 static uint64_t nowNS(void *,int);
public:
 virtual bool init(OSDictionary *properties=0);
 virtual bool start(IOService *provider);
 virtual void free();
 virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *,UInt64,UInt64,IOStorageAttributes *,IOStorageCompletion *);
 virtual IOReturn doEjectMedia(){return kIOReturnUnsupported;}
 virtual IOReturn doFormatMedia(UInt64){return kIOReturnUnsupported;}
 virtual UInt32 doGetFormatCapacities(UInt64 *,UInt32)const{return 0;}
 virtual IOReturn doLockUnlockMedia(bool){return kIOReturnUnsupported;}
 virtual IOReturn doSynchronizeCache(){return kIOReturnUnsupported;}
 virtual char *getVendorString();virtual char *getProductString();
 virtual char *getRevisionString();virtual char *getAdditionalDeviceInfoString();
 virtual IOReturn reportBlockSize(UInt64 *);
 virtual IOReturn reportMaxValidBlock(UInt64 *);
 virtual IOReturn reportEjectability(bool *);
 virtual IOReturn reportLockability(bool *);
 virtual IOReturn reportRemovability(bool *);
 virtual IOReturn reportMediaState(bool *,bool *);
 virtual IOReturn reportPollRequirements(bool *,bool *);
 virtual IOReturn reportWriteProtection(bool *);
};
OSDefineMetaClassAndStructors(IOS7J1SDMMC,IOBlockStorageDevice)
uint32_t IOS7J1SDMMC::readMMIO(void *o,unsigned off)
{
 IOS7J1SDMMC *s=(IOS7J1SDMMC*)o;
 __asm__ __volatile__("dsb sy":::"memory");
 UInt32 v=s->regs[off/4]; __asm__ __volatile__("dsb sy":::"memory"); return v;
}
void IOS7J1SDMMC::writeMMIO(void *o,unsigned off,uint32_t v)
{
 IOS7J1SDMMC *s=(IOS7J1SDMMC*)o;
 __asm__ __volatile__("dsb sy":::"memory");
 s->regs[off/4]=v; __asm__ __volatile__("dsb sy":::"memory");
}
uint64_t IOS7J1SDMMC::nowNS(void *,int){ return mach_absolute_time(); }
int IOS7J1SDMMC::delayUS(void *,uint32_t us)
{
 UInt64 st=mach_absolute_time(), lim=(UInt64)us*1000U;
 for(unsigned n=0;n<5000000U;n++){ if(mach_absolute_time()-st>=lim) return 1; }
 return 0;
}
bool IOS7J1SDMMC::init(OSDictionary *p)
{
 mmio=0; map=0; regs=0; mutex=0; initialized=false;
 struct j1_sd_io io={this,readMMIO,writeMMIO,delayUS,nowNS,3000000U};
 j1_sd_init(&card,&io);
 return IOBlockStorageDevice::init(p);
}
bool IOS7J1SDMMC::start(IOService *provider)
{
 if(!IOBlockStorageDevice::start(provider)) return false;
 mutex=IOLockAlloc(); if(!mutex) return false;
 mmio=IOMemoryDescriptor::withPhysicalAddress(0x13560000U,0x1000,kIODirectionInOut);
 if(!mmio) return false;
 map=mmio->map(kIOMapInhibitCache);
 if(!map||!map->getAddress()) return false;
 regs=(volatile UInt32*)map->getAddress();
 if(j1_sd_probe(&card)!=J1_SD_OK){
  IOLog("J1 SDMMC probe failed\n"); return false;
 }
 initialized=true;
 IOLog("J1 SDMMC: sectors=%llu sdhc=%d\n",(unsigned long long)card.sectors,card.sdhc);
 /* Publish personalities so HFS file + partition scheme attach. */
 OSObject *dec=OSUnserializeXML(ios7labStoragePersonalities);
 OSArray *drv=OSDynamicCast(OSArray,dec);
 if(!drv||!gIOCatalogue->addDrivers(drv,false)) { if(dec)dec->release(); return false; }
 dec->release();
 return true;
}
void IOS7J1SDMMC::free(){ if(mutex)IOLockFree(mutex); if(map)map->release(); if(mmio)mmio->release(); IOBlockStorageDevice::free(); }
char *IOS7J1SDMMC::getVendorString(){static char t[]="Samsung";return t;}
char *IOS7J1SDMMC::getProductString(){static char t[]="J1 DW-MMC SD";return t;}
char *IOS7J1SDMMC::getRevisionString(){static char t[]="PIO";return t;}
char *IOS7J1SDMMC::getAdditionalDeviceInfoString(){static char t[]="J1 SDMMC";return t;}
IOReturn IOS7J1SDMMC::reportBlockSize(UInt64 *v){if(!v)return kIOReturnBadArgument;*v=512;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::reportMaxValidBlock(UInt64 *v){if(!v||!initialized)return kIOReturnNotReady;*v=card.sectors-1;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::reportEjectability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::reportLockability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::reportRemovability(bool *v){if(!v)return kIOReturnBadArgument;*v=false;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::reportMediaState(bool *p,bool *c){if(!p||!c)return kIOReturnBadArgument;*p=initialized;*c=false;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::reportPollRequirements(bool *r,bool *e){if(!r||!e)return kIOReturnBadArgument;*r=false;*e=false;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::reportWriteProtection(bool *v){if(!v)return kIOReturnBadArgument;*v=true;return kIOReturnSuccess;}
IOReturn IOS7J1SDMMC::doAsyncReadWrite(IOMemoryDescriptor *buffer,UInt64 block,UInt64 count,
 IOStorageAttributes *attributes,IOStorageCompletion *completion)
{
 UInt64 bytes=0;
 if(!buffer||!completion||!completion->action||!initialized||
    block>=card.sectors||count==0||block+count>card.sectors||
    count*512U!=buffer->getLength())return kIOReturnBadArgument;
 if(buffer->getDirection()!=kIODirectionIn)return kIOReturnNotWritable;
 if(attributes&&(attributes->options||attributes->reserved0032||attributes->reserved0064))return kIOReturnUnsupported;
 bytes=count*512U;
 IOStorageCompletion saved=*completion;UInt64 completed=0;IOReturn result=kIOReturnSuccess;
 uint8_t *tmp=(uint8_t*)IOMalloc((unsigned)(count*512U));
 if(!tmp)return kIOReturnNoMemory;
 IOLockLock(mutex);
 {
  int rc=j1_sd_read_multi(&card,block,tmp,(uint32_t)count);
  if(rc!=J1_SD_OK)result=kIOReturnIOError;
  else if(buffer->writeBytes(0,tmp,(unsigned)bytes)!=(unsigned)bytes)result=kIOReturnUnderrun;
  else completed=bytes;
 }
 IOLockUnlock(mutex);
 IOFree(tmp,(unsigned)bytes);
#if defined(BOARD_CONFIG_EXYNOS3475_J1XLTE)
 /* Rate-limit the per-sector trace: the cache load is ~300k single-block
  * reads, and logging each one saturates the serial console and throttles
  * the whole boot. Keep every 4096th read (plus the first) as a heartbeat. */
 {
  static unsigned long j1_sdrw_n;
  if(((++j1_sdrw_n) & 0xfffUL)==1UL)
   IOLog("J1SDRW blk=%llu n=%llu result=%d done=%llu rd#%lu\n",
         (unsigned long long)block,(unsigned long long)count,result,
         (unsigned long long)completed,j1_sdrw_n);
 }
#endif
 IOStorage::complete(&saved,result,completed);return kIOReturnSuccess;
}
void ios7j1_register_sd(IOService *platform)
{
 IOS7J1SDMMC *device=new IOS7J1SDMMC;if(!device){panic("J1 SDMMC mandatory object allocation failed");return;}
 bool attached=false,success=false;
 if(device->init())attached=device->attach(platform);
 if(attached&&device->start(platform)){
  device->registerService();success=true;
 }
 if(!success)IOLog("J1 SDMMC: object init/attach/map/start failed\n");
 if(attached&&!success)device->detach(platform);
 device->release();
}
#endif
