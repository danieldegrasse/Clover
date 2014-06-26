/*
 * Copyright (c) 2011-2012 Frank Peng. All rights reserved.
 *
 */

#include "Platform.h"
#include "LoaderUefi.h"
#include "device_tree.h"

#include "kernel_patcher.h"
#include "sse3_patcher.h"
#include "sse3_5_patcher.h"

#define KERNEL_DEBUG 0

#if KERNEL_DEBUG
#define DBG(...)    AsciiPrint(__VA_ARGS__);
#else
#define DBG(...)
#endif

// runtime debug
#define DBG_RT(...)    if (gSettings.KPDebug) { AsciiPrint(__VA_ARGS__); }


EFI_PHYSICAL_ADDRESS    KernelRelocBase = 0;
BootArgs1   *bootArgs1 = NULL;
BootArgs2   *bootArgs2 = NULL;
CHAR8       *dtRoot = NULL;
VOID        *KernelData = NULL;
UINT32      KernelSlide = 0;
BOOLEAN     isKernelcache = FALSE;
BOOLEAN     is64BitKernel = FALSE;
BOOLEAN     SSSE3;

BOOLEAN     PatcherInited = FALSE;

// notes:
// - 64bit segCmd64->vmaddr is 0xffffff80xxxxxxxx and we are taking
//   only lower 32bit part into PrelinkTextAddr
// - PrelinkTextAddr is segCmd64->vmaddr + KernelRelocBase
UINT32     PrelinkTextLoadCmdAddr = 0;
UINT32     PrelinkTextAddr = 0;
UINT32     PrelinkTextSize = 0;

// notes:
// - 64bit sect->addr is 0xffffff80xxxxxxxx and we are taking
//   only lower 32bit part into PrelinkInfoAddr
// - PrelinkInfoAddr is sect->addr + KernelRelocBase
UINT32     PrelinkInfoLoadCmdAddr = 0;
UINT32     PrelinkInfoAddr = 0;
UINT32     PrelinkInfoSize = 0;


/*
typedef struct kernSymbols_t
{
    CHAR8* symbol;
    UINT64 addr;
    struct kernSymbols_t* next;
} kernSymbols_t;

kernSymbols_t* kernelSymbols = NULL;

VOID register_kernel_symbol(CONST CHAR8* name)
{
    if(kernelSymbols == NULL)
    {
        kernelSymbols = AllocateZeroPool(sizeof(kernSymbols_t));
        kernelSymbols->next = NULL;
        kernelSymbols->symbol = (CHAR8*)name;
        kernelSymbols->addr = 0;
    }
    else 
    {
        kernSymbols_t *symbol = kernelSymbols;
        while(symbol->next != NULL)
        {
            symbol = symbol->next;
        }
        
        symbol->next = AllocateZeroPool(sizeof(kernSymbols_t));
        symbol = symbol->next;

        symbol->next = NULL;
        symbol->symbol = (CHAR8*)name;
        symbol->addr = 0;
    }
}

kernSymbols_t* lookup_kernel_symbol(CONST CHAR8* name)
{
    kernSymbols_t *symbol = kernelSymbols;

    while(symbol && (AsciiStrCmp(symbol->symbol, name)!=0))
    {
        symbol = symbol->next;
    }
    
    if(!symbol)
    {
        return NULL;
    }
    else
    {
        return symbol;
    }

}

UINT64 symbol_handler(CHAR8* symbolName, UINT64 addr)
{
    // Locate the symbol in the list, if it exists, update it's address
    kernSymbols_t *symbol = lookup_kernel_symbol(symbolName);
    
    if(symbol)
    {
        symbol->addr = addr;
    }
    
    return 0x7FFFFFFF; // fixme
}

INTN locate_symbols(VOID* kernelData)
{
    //DecodeMachO(kernelData);
    return 1;
}

*/

VOID SetKernelRelocBase()
{
  EFI_STATUS      Status;
  UINTN           DataSize = sizeof(KernelRelocBase);
  
  KernelRelocBase = 0;
  // OsxAptioFixDrv will set this
  Status = gRT->GetVariable(L"OsxAptioFixDrv-RelocBase", &gEfiAppleBootGuid, NULL, &DataSize, &KernelRelocBase);
  // KernelRelocBase is now either read or 0
  return;
}



VOID KernelPatcher_64(VOID* kernelData, CHAR8 *OSVersion)
{
  
    UINT8       *bytes = (UINT8*)kernelData;
    UINT32      patchLocation=0, patchLocation1=0;
    UINT32      i;
    UINT32      switchaddr=0;
    UINT32      mask_family=0, mask_model=0;
    UINT32      cpuid_family_addr=0, cpuid_model_addr=0;
  
    DBG_RT("\nLooking for _cpuid_set_info _panic ...\n");
    
    // Determine location of _cpuid_set_info _panic call for reference
    // basically looking for info_p->cpuid_model = bitfield32(reg[eax],  7,  4);
    for (i=0; i<0x1000000; i++) {
        if (bytes[i+ 0] == 0xC7 && bytes[i+ 1] == 0x05 && bytes[i+ 5] == 0x00 && bytes[i+ 6] == 0x07 && bytes[i+ 7] == 0x00 && bytes[i+ 8] == 0x00 && bytes[i+ 9] == 0x00 &&
            bytes[i+10] == 0xC7 && bytes[i+11] == 0x05 && bytes[i+15] == 0x00 && bytes[i+16] == 0x04 && bytes[i+17] == 0x00 && bytes[i+18] == 0x00 && bytes[i+19] == 0x00) {
            // matching bytes[i-5] == 0xE8 for _panic call doesn't seem to always work
            // i made sure _panic call is only place wit this sequence in all the kernels i've looked at
            patchLocation = i-5;
            break;
        }
    }
    
    if (!patchLocation) {
        DBG_RT("_cpuid_set_info Unsupported CPU _panic not fount \n");
        return;
    }

    // make sure only kernels for OSX 10.6.0 to 10.7.3 are being patched by this approach
  if ((AsciiStrnCmp(OSVersion,"10.6",4)==0) ||
      ((AsciiStrnCmp(OSVersion,"10.7",4)==0) && (OSVersion[5] < '4'))) {
 //   if (AsciiStrnCmp(OSVersion,"10.6",4)>=0 && AsciiStrnCmp(OSVersion,"10.7.3",6)<=0) {

        DBG_RT("will patch kernel for OSX 10.6.0 to 10.7.3\n");
        
        // remove tsc_init: unknown CPU family panic for kernels prior to 10.6.2 which still had Atom support
        if (AsciiStrnCmp(OSVersion,"10.6.0",6)==0 || AsciiStrnCmp(OSVersion,"10.6.1",6)==0) {
            for (i=0; i<0x1000000; i++) {
                // find _tsc_init panic address by byte sequence 488d3df4632a00
                if (bytes[i] == 0x48 && bytes[i+1] == 0x8D && bytes[i+2] == 0x3D && bytes[i+3] == 0xF4 &&
                    bytes[i+4] == 0x63 && bytes[i+5] == 0x2A && bytes[i+6] == 0x00) {
                    patchLocation1 = i+9;
                    DBG_RT("Found _tsc_init _panic address at 0x%08x\n",patchLocation1);
                    break;
                }
            }
        
            // NOP _panic call
            if (patchLocation1) {
                bytes[patchLocation1 + 0] = 0x90;
                bytes[patchLocation1 + 1] = 0x90;
                bytes[patchLocation1 + 2] = 0x90;
                bytes[patchLocation1 + 3] = 0x90;
                bytes[patchLocation1 + 4] = 0x90;
            }
        }
        else { // assume patching logic for OSX 10.6.2 to 10.7.3
            
            // Determine cpuid_model address
            
            // 10.6.2 to 10.6.8 kernels
            if (AsciiStrnCmp(OSVersion,"10.6",4)==0) {
                // C1E004           shl        eax, 0x4
                // 000575E44900     add        byte [ds:0xffffff80006c69cd], al
                for (i=0; i<0x1000000; i++) {
                    if(bytes[i+0] == 0xC0 && bytes[i+1] == 0xE0 && bytes[i+2] == 0x04 &&
                       bytes[i+3] == 0x00 && bytes[i+4] == 0x5) {
                        
                        cpuid_model_addr =
                        bytes[i + 5] <<  0 |
                        bytes[i + 6] <<  8 |
                        bytes[i + 7] << 16 |
                        bytes[i + 8] << 24;
                        cpuid_model_addr = cpuid_model_addr + i + 9;
                    }
                }
            }
            else { // 10.7.0 to 10.7.3 kernels
                // C0E004           shl        al, 0x4
                // 0005B82D5F00     add        byte [ds:0xffffff80008a216d], al
                for (i=0; i<0x1000000; i++) {
                    if(bytes[i+0] == 0xC0 && bytes[i+1] == 0xE0 && bytes[i+2] == 0x04 &&
                       bytes[i+3] == 0x00 && bytes[i+4] == 0x5) {
                
                        cpuid_model_addr =
                        bytes[i + 5] <<  0 |
                        bytes[i + 6] <<  8 |
                        bytes[i + 7] << 16 |
                        bytes[i + 8] << 24;
                        cpuid_model_addr = cpuid_model_addr + i + 9;
                    }
                }
            }

            /*
             Here is our case from CPUID switch statement, it sets CPUFAMILY_UNKNOWN
             C7051C2C5F0000000000   mov     dword [ds:0xffffff80008a22c0], 0x0 (example from 10.7)
             */
            switchaddr = patchLocation-19;
            DBG_RT("switch statement patch location is 0x%08x\n", (switchaddr+6));
        
            if (bytes[switchaddr+0] == 0xC7 && bytes[switchaddr+1] == 0x05 &&
                bytes[switchaddr+5] == 0x00 && bytes[switchaddr+6] == 0x00 &&
                bytes[switchaddr+7] == 0x00 && bytes[switchaddr+8] == 0x00) {

                if (cpuid_model_addr) {
                    
                    DBG_RT("cpuid_model address is 0x%08x\n", cpuid_model_addr);
                
                    switchaddr += 6; // offset 6 bytes in mov operation to write a dword instead of zero
                    
                    // calculate mask for patching, cpuid_family mask not needed as we offset on a valid mask
                    mask_model   = cpuid_model_addr - (switchaddr+14);
                    DBG_RT("model mask 0x%08x\n", mask_model);
                
                    DBG_RT("overriding cpuid_family and cpuid_model as CPUID_INTEL_PENRYN\n");
                    bytes[switchaddr+0] = (CPUFAMILY_INTEL_PENRYN & 0x000000FF) >>  0;
                    bytes[switchaddr+1] = (CPUFAMILY_INTEL_PENRYN & 0x0000FF00) >>  8;
                    bytes[switchaddr+2] = (CPUFAMILY_INTEL_PENRYN & 0x00FF0000) >> 16;
                    bytes[switchaddr+3] = (CPUFAMILY_INTEL_PENRYN & 0xFF000000) >> 24;

                    // mov  dword [ds:0xffffff80008a216d], 0x2000117
                    bytes[switchaddr+4] = 0xC7;
                    bytes[switchaddr+5] = 0x05;
                    bytes[switchaddr+6] = (mask_model & 0x000000FF) >> 0;
                    bytes[switchaddr+7] = (mask_model & 0x0000FF00) >> 8;
                    bytes[switchaddr+8] = (mask_model & 0x00FF0000) >> 16;
                    bytes[switchaddr+9] = (mask_model & 0xFF000000) >> 24;
                    bytes[switchaddr+10] = 0x17; // cpuid_model (Penryn)
                    bytes[switchaddr+11] = 0x01; // cpuid_extmodel
                    bytes[switchaddr+12] = 0x00; // cpuid_extfamily
                    bytes[switchaddr+13] = 0x02; // cpuid_stepping
                    
                    // fill remainder with 4 NOPs
                    for (i=14; i<18; i++) {
                        bytes[switchaddr+i] = 0x90;
                    }
                }
            }
        }
  
        // patch ssse3
        if (!SSSE3 && (AsciiStrnCmp(OSVersion,"10.6",4)==0)) {
            Patcher_SSE3_6((VOID*)bytes);
        }
        if (!SSSE3 && (AsciiStrnCmp(OSVersion,"10.7",4)==0)) {
            Patcher_SSE3_7((VOID*)bytes);
        }
    }
    
    // all 10.7.4+ kernels share common CPUID switch statement logic,
    // it needs to be exploited in diff manner due to the lack of space
    if (((AsciiStrnCmp(OSVersion,"10.7",4)==0) && (OSVersion[5] > '3')) || (OSVersion[3] > '7')) {
        
        DBG_RT("will patch kernel for OSX 10.7.4+\n");
        
        /*
         Here is our switchaddress location ... it should be case 20 from CPUID switch statement
         833D78945F0000  cmp        dword [ds:0xffffff80008a21d0], 0x0;
         7417            je         0xffffff80002a8d71
         */
        switchaddr = patchLocation-45;
        DBG_RT("switch statement patch location is 0x%08x\n", switchaddr);
        
        if(bytes[switchaddr+0] == 0x83 && bytes[switchaddr+1] == 0x3D &&
           bytes[switchaddr+5] == 0x00 && bytes[switchaddr+6] == 0x00 &&
           bytes[switchaddr+7] == 0x74) {
            
            // Determine cpuid_family address
            // 891D4F945F00    mov        dword [ds:0xffffff80008a21a0], ebx
            cpuid_family_addr =
            bytes[switchaddr - 4] <<  0 |
            bytes[switchaddr - 3] <<  8 |
            bytes[switchaddr - 2] << 16 |
            bytes[switchaddr - 1] << 24;
            cpuid_family_addr = cpuid_family_addr + switchaddr;

            
            // Determine cpuid_model address
            if (AsciiStrnCmp(OSVersion,"10.8",4)==0 || AsciiStrnCmp(OSVersion,"10.9",4)==0) {
                // C0EB04       shr bl, 0x4
                // 881D2B675E00 mov byte [ds:0xffffff80008b204d], bl
                for (i=0; i<0x1000000; i++) {
                    if(bytes[i+0] == 0xC0 && bytes[i+1] == 0xEB && bytes[i+2] == 0x04 &&
                       bytes[i+3] == 0x88 && bytes[i+4] == 0x1D) {
                    
                        cpuid_model_addr =
                        bytes[i + 5] <<  0 |
                        bytes[i + 6] <<  8 |
                        bytes[i + 7] << 16 |
                        bytes[i + 8] << 24;
                        cpuid_model_addr = cpuid_model_addr + i + 9;
                    }
                }
            }
            else { // special case for 10.7.4+ kernels
                // C0E004           shl        al, 0x4
                // 0005B82D5F00     add        byte [ds:0xffffff80008a216d], al
                for (i=0; i<0x1000000; i++) {
                    if(bytes[i+0] == 0xC0 && bytes[i+1] == 0xE0 && bytes[i+2] == 0x04 &&
                       bytes[i+3] == 0x00 && bytes[i+4] == 0x5) {
                    
                        cpuid_model_addr =
                        bytes[i + 5] <<  0 |
                        bytes[i + 6] <<  8 |
                        bytes[i + 7] << 16 |
                        bytes[i + 8] << 24;
                        cpuid_model_addr = cpuid_model_addr + i + 9;
                    }
                }
            }
            
            if (cpuid_family_addr && cpuid_model_addr) {
                
                DBG_RT("cpuid_family address is 0x%08x\n", cpuid_family_addr);
                DBG_RT("cpuid_model address is 0x%08x\n",  cpuid_model_addr);
                
                // Calculate masks for patching
                mask_family  = cpuid_family_addr - (switchaddr +15);
                mask_model   = cpuid_model_addr -  (switchaddr +25);
                DBG_RT("family mask 0x%08x and model mask 0x%08x\n", mask_family, mask_model);
                
                DBG_RT("overriding cpuid_family and cpuid_model as CPUID_INTEL_PENRYN\n");
                // retain original
                // test ebx, ebx
                bytes[switchaddr+0] = 0x85;
                bytes[switchaddr+1] = 0xDB;
                // retain original
                // jne for above test
                bytes[switchaddr+2] = 0x75;
                bytes[switchaddr+3] = 0x2E;
                // mov ebx, 0x78ea4fbc
                bytes[switchaddr+4] = 0xBB;
                bytes[switchaddr+5] = (CPUFAMILY_INTEL_PENRYN & 0x000000FF) >>  0;
                bytes[switchaddr+6] = (CPUFAMILY_INTEL_PENRYN & 0x0000FF00) >>  8;
                bytes[switchaddr+7] = (CPUFAMILY_INTEL_PENRYN & 0x00FF0000) >> 16;
                bytes[switchaddr+8] = (CPUFAMILY_INTEL_PENRYN & 0xFF000000) >> 24;
                
                // mov dword, ebx
                bytes[switchaddr+9]  = 0x89;
                bytes[switchaddr+10] = 0x1D;
                // cpuid_cpufamily address 0xffffff80008a21a0
                bytes[switchaddr+11] = (mask_family & 0x000000FF) >> 0;
                bytes[switchaddr+12] = (mask_family & 0x0000FF00) >> 8;
                bytes[switchaddr+13] = (mask_family & 0x00FF0000) >> 16;
                bytes[switchaddr+14] = (mask_family & 0xFF000000) >> 24;
                
                // mov dword
                bytes[switchaddr+15] = 0xC7;
                bytes[switchaddr+16] = 0x05;
                // cpuid_model address 0xffffff80008b204d
                bytes[switchaddr+17] = (mask_model & 0x000000FF) >> 0;
                bytes[switchaddr+18] = (mask_model & 0x0000FF00) >> 8;
                bytes[switchaddr+19] = (mask_model & 0x00FF0000) >> 16;
                bytes[switchaddr+20] = (mask_model & 0xFF000000) >> 24;
                
                bytes[switchaddr+21] = 0x17; // cpuid_model
                bytes[switchaddr+22] = 0x01; // cpuid_extmodel
                bytes[switchaddr+23] = 0x00; // cpuid_extfamily
                bytes[switchaddr+24] = 0x02; // cpuid_stepping
                
                // fill remainder with 25 NOPs
                for (i=25; i<25+25; i++) {
                    bytes[switchaddr+i] = 0x90;
                }
            }
        }
    }
}

VOID KernelPatcher_32(VOID* kernelData, CHAR8 *OSVersion)
{
  UINT8* bytes = (UINT8*)kernelData;
  UINT32 patchLocation=0, patchLocation1=0;
  UINT32 i;
  UINT32 jumpaddr;
  
  DBG("Found _cpuid_set_info _panic Start\n");
  // _cpuid_set_info _panic address
  for (i=0; i<0x1000000; i++) {   
    if (bytes[i] == 0xC7 && bytes[i+1] == 0x05 && bytes[i+6] == 0x07 && bytes[i+7] == 0x00 &&
        bytes[i+8] == 0x00 && bytes[i+9] == 0x00 && bytes[i+10] == 0xC7 && bytes[i+11] == 0x05 &&
        bytes[i-5] == 0xE8) {
      patchLocation = i-5;
      DBG("Found _cpuid_set_info _panic address at 0x%08x\n",patchLocation);
      break;
    }
  }
  
  if (!patchLocation) {
    DBG("Can't find _cpuid_set_info _panic address, patch kernel abort.\n",i);
    return;
  }
  
  // this for 10.6.0 and 10.6.1 kernel and remove tsc.c unknow cpufamily panic
  //  c70424540e5900
  // find _tsc_init panic address
  for (i=0; i<0x1000000; i++) {
    // _cpuid_set_info _panic address
    if (bytes[i] == 0xC7 && bytes[i+1] == 0x04 && bytes[i+2] == 0x24 && bytes[i+3] == 0x54 &&
        bytes[i+4] == 0x0E && bytes[i+5] == 0x59 && bytes[i+6] == 0x00) {
      patchLocation1 = i+7;
      DBG("Found _tsc_init _panic address at 0x%08x\n",patchLocation1);
      break;
    }
  }
  
  // found _tsc_init panic addres and patch it
  if (patchLocation1) {
    bytes[patchLocation1 + 0] = 0x90;
    bytes[patchLocation1 + 1] = 0x90;
    bytes[patchLocation1 + 2] = 0x90;
    bytes[patchLocation1 + 3] = 0x90;
    bytes[patchLocation1 + 4] = 0x90;
  }
  // end tsc.c panic
  
  //first move panic code total 5 bytes, if patch cpuid fail still can boot with kernel
  bytes[patchLocation + 0] = 0x90;
  bytes[patchLocation + 1] = 0x90;
  bytes[patchLocation + 2] = 0x90;
  bytes[patchLocation + 3] = 0x90;
  bytes[patchLocation + 4] = 0x90;
  
  jumpaddr = patchLocation;
  
  for (i=0;i<500;i++) {
    if (bytes[jumpaddr-i-3] == 0x85 && bytes[jumpaddr-i-2] == 0xC0 &&
        bytes[jumpaddr-i-1] == 0x75 ) {
      jumpaddr -= i;
      bytes[jumpaddr-1] = 0x77;
      if(bytes[patchLocation - 17] == 0xC7)
        bytes[jumpaddr] -=10;
      
      break;
    } 
  }
  
  if (jumpaddr == patchLocation) {
    DBG("Can't Found jumpaddr address.\n");
    return;  //can't find jump location
  }
  // patch info_p->cpufamily to CPUFAMILY_INTEL_MEROM
  
  if (bytes[patchLocation - 17] == 0xC7) {
    bytes[patchLocation - 11] = (CPUFAMILY_INTEL_MEROM & 0x000000FF) >>  0;
    bytes[patchLocation - 10] = (CPUFAMILY_INTEL_MEROM & 0x0000FF00) >>  8;
    bytes[patchLocation -  9] = (CPUFAMILY_INTEL_MEROM & 0x00FF0000) >> 16;
    bytes[patchLocation -  8] = (CPUFAMILY_INTEL_MEROM & 0xFF000000) >> 24;
  } 
  
  //patch info->cpuid_cpufamily
  bytes[patchLocation -  7] = 0xC7;
  bytes[patchLocation -  6] = 0x05;
  bytes[patchLocation -  5] = bytes[jumpaddr + 3];
  bytes[patchLocation -  4] = bytes[jumpaddr + 4];
  bytes[patchLocation -  3] = bytes[jumpaddr + 5];
  bytes[patchLocation -  2] = bytes[jumpaddr + 6];
  
  bytes[patchLocation -  1] = CPUIDFAMILY_DEFAULT; //cpuid_family  need alway set 0x06
  bytes[patchLocation +  0] = CPUID_MODEL_MEROM;   //cpuid_model set CPUID_MODEL_MEROM
  bytes[patchLocation +  1] = 0x01;                //cpuid_extmodel alway set 0x01
  bytes[patchLocation +  2] = 0x00;                //cpuid_extfamily alway set 0x00
  bytes[patchLocation +  3] = 0x90;
  bytes[patchLocation +  4] = 0x90;
  
  if (OSVersion) {
    if (AsciiStrnCmp(OSVersion,"10.7",4)==0) return;
    
    if (!SSSE3 && (AsciiStrnCmp(OSVersion,"10.6",4)==0)) {
      Patcher_SSE3_6((VOID*)bytes);
    }
    if (!SSSE3 && (AsciiStrnCmp(OSVersion,"10.5",4)==0)) {
      Patcher_SSE3_5((VOID*)bytes);
    }
  }
}

// Power management patch for kernel 13.0
STATIC UINT8 KernelPatchPmSrc[] = {
  0x55, 0x48, 0x89, 0xe5, 0x41, 0x89, 0xd0, 0x85,
  0xf6, 0x74, 0x6c, 0x48, 0x83, 0xc7, 0x28, 0x90,
  0x8b, 0x05, 0x5e, 0x30, 0x5e, 0x00, 0x85, 0x47,
  0xdc, 0x74, 0x54, 0x8b, 0x4f, 0xd8, 0x45, 0x85,
  0xc0, 0x74, 0x08, 0x44, 0x39, 0xc1, 0x44, 0x89,
  0xc1, 0x75, 0x44, 0x0f, 0x32, 0x89, 0xc0, 0x48,
  0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x48, 0x89,
  0x57, 0xf8, 0x48, 0x8b, 0x47, 0xe8, 0x48, 0x85,
  0xc0, 0x74, 0x06, 0x48, 0xf7, 0xd0, 0x48, 0x21,
  0xc2, 0x48, 0x0b, 0x57, 0xf0, 0x49, 0x89, 0xd1,
  0x49, 0xc1, 0xe9, 0x20, 0x89, 0xd0, 0x8b, 0x4f,
  0xd8, 0x4c, 0x89, 0xca, 0x0f, 0x30, 0x8b, 0x4f,
  0xd8, 0x0f, 0x32, 0x89, 0xc0, 0x48, 0xc1, 0xe2,
  0x20, 0x48, 0x09, 0xc2, 0x48, 0x89, 0x17, 0x48,
  0x83, 0xc7, 0x30, 0xff, 0xce, 0x75, 0x99, 0x5d,
  0xc3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};
STATIC UINT8 KernelPatchPmRepl[] = {
  0x55, 0x48, 0x89, 0xe5, 0x41, 0x89, 0xd0, 0x85,
  0xf6, 0x74, 0x73, 0x48, 0x83, 0xc7, 0x28, 0x90,
  0x8b, 0x05, 0x5e, 0x30, 0x5e, 0x00, 0x85, 0x47,
  0xdc, 0x74, 0x5b, 0x8b, 0x4f, 0xd8, 0x45, 0x85,
  0xc0, 0x74, 0x08, 0x44, 0x39, 0xc1, 0x44, 0x89,
  0xc1, 0x75, 0x4b, 0x0f, 0x32, 0x89, 0xc0, 0x48,
  0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2, 0x48, 0x89,
  0x57, 0xf8, 0x48, 0x8b, 0x47, 0xe8, 0x48, 0x85,
  0xc0, 0x74, 0x06, 0x48, 0xf7, 0xd0, 0x48, 0x21,
  0xc2, 0x48, 0x0b, 0x57, 0xf0, 0x49, 0x89, 0xd1,
  0x49, 0xc1, 0xe9, 0x20, 0x89, 0xd0, 0x8b, 0x4f,
  0xd8, 0x4c, 0x89, 0xca, 0x66, 0x81, 0xf9, 0xe2,
  0x00, 0x74, 0x02, 0x0f, 0x30, 0x8b, 0x4f, 0xd8,
  0x0f, 0x32, 0x89, 0xc0, 0x48, 0xc1, 0xe2, 0x20,
  0x48, 0x09, 0xc2, 0x48, 0x89, 0x17, 0x48, 0x83,
  0xc7, 0x30, 0xff, 0xce, 0x75, 0x92, 0x5d, 0xc3
};
// Power management patch for kernel 12.5
STATIC UINT8 KernelPatchPmSrc2[] = {
  0x55, 0x48, 0x89, 0xe5, 0x41, 0x89, 0xd0, 0x85,
  0xf6, 0x74, 0x69, 0x48, 0x83, 0xc7, 0x28, 0x90,
  0x8b, 0x05, 0xfe, 0xce, 0x5f, 0x00, 0x85, 0x47,
  0xdc, 0x74, 0x51, 0x8b, 0x4f, 0xd8, 0x45, 0x85,
  0xc0, 0x74, 0x05, 0x44, 0x39, 0xc1, 0x75, 0x44,
  0x0f, 0x32, 0x89, 0xc0, 0x48, 0xc1, 0xe2, 0x20,
  0x48, 0x09, 0xc2, 0x48, 0x89, 0x57, 0xf8, 0x48,
  0x8b, 0x47, 0xe8, 0x48, 0x85, 0xc0, 0x74, 0x06,
  0x48, 0xf7, 0xd0, 0x48, 0x21, 0xc2, 0x48, 0x0b,
  0x57, 0xf0, 0x49, 0x89, 0xd1, 0x49, 0xc1, 0xe9,
  0x20, 0x89, 0xd0, 0x8b, 0x4f, 0xd8, 0x4c, 0x89,
  0xca, 0x0f, 0x30, 0x8b, 0x4f, 0xd8, 0x0f, 0x32,
  0x89, 0xc0, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09,
  0xc2, 0x48, 0x89, 0x17, 0x48, 0x83, 0xc7, 0x30,
  0xff, 0xce, 0x75, 0x9c, 0x5d, 0xc3, 0x90, 0x90,
  0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

STATIC UINT8 KernelPatchPmRepl2[] = {
  0x55, 0x48, 0x89, 0xe5, 0x41, 0x89, 0xd0, 0x85,
  0xf6, 0x74, 0x70, 0x48, 0x83, 0xc7, 0x28, 0x90,
  0x8b, 0x05, 0xfe, 0xce, 0x5f, 0x00, 0x85, 0x47,
  0xdc, 0x74, 0x58, 0x8b, 0x4f, 0xd8, 0x45, 0x85,
  0xc0, 0x74, 0x05, 0x44, 0x39, 0xc1, 0x75, 0x4b,
  0x0f, 0x32, 0x89, 0xc0, 0x48, 0xc1, 0xe2, 0x20,
  0x48, 0x09, 0xc2, 0x48, 0x89, 0x57, 0xf8, 0x48,
  0x8b, 0x47, 0xe8, 0x48, 0x85, 0xc0, 0x74, 0x06,
  0x48, 0xf7, 0xd0, 0x48, 0x21, 0xc2, 0x48, 0x0b,
  0x57, 0xf0, 0x49, 0x89, 0xd1, 0x49, 0xc1, 0xe9,
  0x20, 0x89, 0xd0, 0x8b, 0x4f, 0xd8, 0x4c, 0x89,
  0xca, 0x66, 0x81, 0xf9, 0xe2, 0x00, 0x74, 0x02,
  0x0f, 0x30, 0x8b, 0x4f, 0xd8, 0x0f, 0x32, 0x89,
  0xc0, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2,
  0x48, 0x89, 0x17, 0x48, 0x83, 0xc7, 0x30, 0xff,
  0xce, 0x75, 0x95, 0x5d, 0xc3, 0x90, 0x90, 0x90
};

STATIC UINT8 KernelPatchPmSrc1010[] = {
  0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
  0x53, 0x50, 0x41, 0x89, 0xd6, 0x41, 0x89, 0xf7, 0x48, 0x89, 0xfb, 0x45,
  0x85, 0xff, 0x0f, 0x84, 0x9b, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc3, 0x28,
  0x4c, 0x8d, 0x25, 0x89, 0xae, 0x53, 0x00, 0x4c, 0x8d, 0x2d, 0x9c, 0xae,
  0x53, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x8b, 0x05, 0x96, 0x75, 0x69, 0x00, 0x85, 0x43,
  0xdc, 0x74, 0x67, 0x45, 0x85, 0xf6, 0x74, 0x06, 0x44, 0x39, 0x73, 0xd8,
  0x75, 0x5c, 0x83, 0x3d, 0x13, 0xed, 0x5f, 0x00, 0x00, 0x74, 0x10, 0x8b,
  0x53, 0xd8, 0x31, 0xc0, 0x4c, 0x89, 0xe7, 0x4c, 0x89, 0xee, 0xe8, 0xa1,
  0xb0, 0x4d, 0x00, 0x8b, 0x4b, 0xd8, 0x0f, 0x32, 0x89, 0xd1, 0x48, 0xc1,
  0xe1, 0x20, 0x89, 0xc2, 0x48, 0x09, 0xca, 0x48, 0x89, 0x53, 0xf8, 0x48,
  0x8b, 0x43, 0xe8, 0x48, 0x85, 0xc0, 0x74, 0x06, 0x48, 0xf7, 0xd0, 0x48,
  0x21, 0xc2, 0x48, 0x0b, 0x53, 0xf0, 0x8b, 0x4b, 0xd8, 0x89, 0xd0, 0x48,
  0xc1, 0xea, 0x20, 0x0f, 0x30, 0x8b, 0x4b, 0xd8, 0x0f, 0x32, 0x48, 0xc1,
  0xe2, 0x20, 0x89, 0xc0, 0x48, 0x09, 0xd0, 0x48, 0x89, 0x03, 0x48, 0x83,
  0xc3, 0x30, 0x41, 0xff, 0xcf, 0x75, 0x85, 0x48, 0x83, 0xc4, 0x08, 0x5b,
  0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3, 0x66, 0x0f,
  0x1f, 0x44, 0x00, 0x00
};

static UINT8 KernelPatchPmRepl1010[] = {
  0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
  0x53, 0x50, 0x41, 0x89, 0xd6, 0x41, 0x89, 0xf7, 0x48, 0x89, 0xfb, 0x45,
  0x85, 0xff, 0x0f, 0x84, 0x9f, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc3, 0x28,
  0x4c, 0x8d, 0x25, 0x89, 0xae, 0x53, 0x00, 0x4c, 0x8d, 0x2d, 0x9c, 0xae,
  0x53, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x8b, 0x05, 0x96, 0x75, 0x69, 0x00, 0x85, 0x43,
  0xdc, 0x74, 0x6b, 0x45, 0x85, 0xf6, 0x74, 0x06, 0x44, 0x39, 0x73, 0xd8,
  0x74, 0x60, 0x83, 0x3d, 0x13, 0xed, 0x5f, 0x00, 0x00, 0x74, 0x10, 0x8b,
  0x53, 0xd8, 0x31, 0xc0, 0x4c, 0x89, 0xe7, 0x4c, 0x89, 0xee, 0xe8, 0xa1,
  0xb0, 0x4d, 0x00, 0x8b, 0x4b, 0xd8, 0x0f, 0x32, 0x89, 0xd1, 0x48, 0xc1,
  0xe1, 0x20, 0x89, 0xc2, 0x48, 0x09, 0xca, 0x48, 0x89, 0x53, 0xf8, 0x48,
  0x8b, 0x43, 0xe8, 0x48, 0x85, 0xc0, 0x74, 0x06, 0x48, 0xf7, 0xd0, 0x48,
  0x21, 0xc2, 0x48, 0x0b, 0x53, 0xf0, 0x8b, 0x4b, 0xd8, 0x89, 0xd0, 0x48,
  0xc1, 0xea, 0x20, 0x85, 0xc9, 0x74, 0x02, 0x0f, 0x30, 0x8b, 0x4b, 0xd8,
  0x0f, 0x32, 0x48, 0xc1, 0xe2, 0x20, 0x89, 0xc0, 0x48, 0x09, 0xd0, 0x48,
  0x89, 0x03, 0x48, 0x83, 0xc3, 0x30, 0x41, 0xff, 0xcf, 0x75, 0x81, 0x48,
  0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f,
  0x5d, 0xc3, 0x90, 0x90
};


#define KERNEL_PATCH_SIGNATURE     0x85d08941e5894855ULL
#define KERNEL_YOS_PATCH_SIGNATURE 0x56415741e5894855ULL

BOOLEAN KernelPatchPm(VOID *kernelData)
{
  UINT8  *Ptr = (UINT8 *)kernelData;
  UINT8  *End = Ptr + 0x1000000;
  if (Ptr == NULL) {
    return FALSE;
  }
  // Credits to RehabMan for the kernel patch information
  DBG("Patching kernel power management...\n");
  while (Ptr < End) {
    if (KERNEL_PATCH_SIGNATURE == (*((UINT64 *)Ptr))) {
      // Bytes 19,20 of KernelPm patch for kernel 13.x change between kernel versions, so we skip them in search&replace
      if ((CompareMem(Ptr + sizeof(UINT64),   KernelPatchPmSrc + sizeof(UINT64),   18*sizeof(UINT8) - sizeof(UINT64)) == 0) && 
          (CompareMem(Ptr + 20*sizeof(UINT8), KernelPatchPmSrc + 20*sizeof(UINT8), sizeof(KernelPatchPmSrc) - 20*sizeof(UINT8)) == 0)) {
        // Don't copy more than the source here!
        CopyMem(Ptr, KernelPatchPmRepl, 18*sizeof(UINT8));
        CopyMem(Ptr + 20*sizeof(UINT8), KernelPatchPmRepl + 20*sizeof(UINT8), sizeof(KernelPatchPmSrc) - 20*sizeof(UINT8));
        DBG("Kernel power management patch region 1 found and patched\n");
        return TRUE;
      } else if (CompareMem(Ptr + sizeof(UINT64), KernelPatchPmSrc2 + sizeof(UINT64), sizeof(KernelPatchPmSrc2) - sizeof(UINT64)) == 0) {
        // Don't copy more than the source here!
        CopyMem(Ptr, KernelPatchPmRepl2, sizeof(KernelPatchPmSrc2));
        DBG("Kernel power management patch region 2 found and patched\n");
        return TRUE;
      }
    }
    //rehabman: for 10.10.dp1 (code portion)
    else if (KERNEL_YOS_PATCH_SIGNATURE == (*((UINT64 *)Ptr))) {
      if (0 == CompareMem(Ptr + sizeof(UINT64), KernelPatchPmSrc1010+sizeof(UINT64), sizeof(KernelPatchPmSrc1010)-sizeof(UINT64))) {
        // copy assuming find/replace are same sizes
        CopyMem(Ptr, KernelPatchPmRepl1010, sizeof(KernelPatchPmRepl1010));
        DBG("Kernel power management patch 10.10(code) found and patched\n");
      }
    }
    //rehabman: for 10.10.dp1 (data portion)
    else if (0x00000002000000E2ULL == (*((UINT64 *)Ptr))) {
      (*((UINT64 *)Ptr)) = 0x0000000200000000ULL;
      DBG("Kernel power management patch 10.10(data1) found and patched\n");
    }
    else if (0x0000004C000000E2ULL == (*((UINT64 *)Ptr))) {
      (*((UINT64 *)Ptr)) = 0x0000004C00000000ULL;
      DBG("Kernel power management patch 10.10(data2) found and patched\n");
    }
    else if (0x00000190000000E2ULL == (*((UINT64 *)Ptr))) {
      (*((UINT64 *)Ptr)) = 0x0000019000000000ULL;
      DBG("Kernel power management patch 10.10(data3) found and patched\n");
      return TRUE;
    }
    Ptr += 16;
  }
  DBG("Kernel power management patch region not found!\n");
  return FALSE;
}

BOOLEAN KernelLapicPatch_64(VOID *kernelData)
{
  // Credits to donovan6000 and sherlocks for providing the lapic kernel patch source used to build this function
  
  UINT8       *bytes = (UINT8*)kernelData;
  UINT32      patchLocation=0;
  UINT32      i;
  
  DBG("Looking for Lapic panic call (64-bit) Start\n");
  
  for (i=0; i<0x1000000; i++) {   
    if (bytes[i+0]  == 0x65 && bytes[i+1]  == 0x8B && bytes[i+2]  == 0x04 && bytes[i+3]  == 0x25 &&
        bytes[i+4]  == 0x14 && bytes[i+5]  == 0x00 && bytes[i+6]  == 0x00 && bytes[i+7]  == 0x00 &&
        bytes[i+35] == 0x65 && bytes[i+36] == 0x8B && bytes[i+37] == 0x04 && bytes[i+38] == 0x25 &&
        bytes[i+39] == 0x14 && bytes[i+40] == 0x00 && bytes[i+41] == 0x00 && bytes[i+42] == 0x00) {
      patchLocation = i+30;
      DBG("Found Lapic panic at 0x%08x\n", patchLocation);
      break;
    } else if (bytes[i+0] == 0x65 && bytes[i+1] == 0x8B && bytes[i+2] == 0x04 && bytes[i+3] == 0x25 &&
               bytes[i+4] == 0x1C && bytes[i+5] == 0x00 && bytes[i+6] == 0x00 && bytes[i+7] == 0x00 &&
               bytes[i+36] == 0x65 && bytes[i+37] == 0x8B && bytes[i+38] == 0x04 && bytes[i+39] == 0x25 &&
               bytes[i+40] == 0x1C && bytes[i+41] == 0x00 && bytes[i+42] == 0x00 && bytes[i+43] == 0x00) {
      patchLocation = i+31;
      DBG("Found MV Lapic panic at 0x%08x\n", patchLocation);
      break;
      //rehabman: 10.10.DP1 lapic
    } else if (bytes[i+0] == 0x65 && bytes[i+1] == 0x8B && bytes[i+2] == 0x04 && bytes[i+3] == 0x25 &&
               bytes[i+4] == 0x1C && bytes[i+5] == 0x00 && bytes[i+6] == 0x00 && bytes[i+7] == 0x00 &&
               bytes[i+33] == 0x65 && bytes[i+34] == 0x8B && bytes[i+35] == 0x04 && bytes[i+36] == 0x25 &&
               bytes[i+37] == 0x1C && bytes[i+38] == 0x00 && bytes[i+39] == 0x00 && bytes[i+40] == 0x00) {
      patchLocation = i+28;
      DBG("Found Yos Lapic panic at 0x%08x\n", patchLocation);
      break;
    }
  }
  
  if (!patchLocation) {
    DBG("Can't find Lapic panic, kernel patch aborted.\n");
    return FALSE;
  }
  
  // Already patched?  May be running a non-vanilla kernel already?
  
  if (bytes[patchLocation + 0] == 0x90 && bytes[patchLocation + 1] == 0x90 && 
      bytes[patchLocation + 2] == 0x90 && bytes[patchLocation + 3] == 0x90 &&
      bytes[patchLocation + 4] == 0x90) {
    DBG("Lapic panic already patched, kernel file manually patched?\n");
    return FALSE;
  } else {
    bytes[patchLocation + 0] = 0x90;
    bytes[patchLocation + 1] = 0x90;
    bytes[patchLocation + 2] = 0x90;
    bytes[patchLocation + 3] = 0x90;
    bytes[patchLocation + 4] = 0x90;
  }
  return TRUE;
}

BOOLEAN KernelLapicPatch_32(VOID *kernelData)
{
  // Credits to donovan6000 and sherlocks for providing the lapic kernel patch source used to build this function
  
  UINT8       *bytes = (UINT8*)kernelData;
  UINT32      patchLocation=0;
  UINT32      i;
  
  DBG("Looking for Lapic panic call (32-bit) Start\n");
  
  for (i=0; i<0x1000000; i++) {   
    if (bytes[i+0]  == 0x65 && bytes[i+1]  == 0xA1 && bytes[i+2]  == 0x0C && bytes[i+3]  == 0x00 &&
        bytes[i+4]  == 0x00 && bytes[i+5]  == 0x00 &&
        bytes[i+30] == 0x65 && bytes[i+31] == 0xA1 && bytes[i+32] == 0x0C && bytes[i+33] == 0x00 &&
        bytes[i+34] == 0x00 && bytes[i+35] == 0x00) {
      patchLocation = i+25;
      DBG("Found Lapic panic at 0x%08x\n", patchLocation);
      break;
    }
  }
  
  if (!patchLocation) {
    DBG("Can't find Lapic panic, kernel patch aborted.\n");
    return FALSE;
  }
  
  // Already patched?  May be running a non-vanilla kernel already?
  
  if (bytes[patchLocation + 0] == 0x90 && bytes[patchLocation + 1] == 0x90 && 
      bytes[patchLocation + 2] == 0x90 && bytes[patchLocation + 3] == 0x90 &&
      bytes[patchLocation + 4] == 0x90) {
    DBG("Lapic panic already patched, kernel file manually patched?\n");
    return FALSE;
  } else {
    bytes[patchLocation + 0] = 0x90;
    bytes[patchLocation + 1] = 0x90;
    bytes[patchLocation + 2] = 0x90;
    bytes[patchLocation + 3] = 0x90;
    bytes[patchLocation + 4] = 0x90;
  }
  return TRUE;
}
	

VOID Patcher_SSE3_6(VOID* kernelData)
{
  UINT8* bytes = (UINT8*)kernelData;
  UINT32 patchLocation1 = 0;
  UINT32 patchLocation2 = 0;
  UINT32 patchlast = 0;
  UINT32 i; 
  //UINT32 Length = sizeof(kernelData);
  
  DBG("Start find SSE3 address\n");
  i=0;
  //for (i=0;i<Length;i++) 
  while(TRUE) {
    if (bytes[i] == 0x66 && bytes[i+1] == 0x0F && bytes[i+2] == 0x6F && 
        bytes[i+3] == 0x44 && bytes[i+4] == 0x0E && bytes[i+5] == 0xF1 &&
        bytes[i-1664-32] == 0x55
        ) {
      patchLocation1 = i-1664-32;
      DBG("Found SSE3 data address at 0x%08x\n",patchLocation1);
    }
    
    // hasSSE2+..... title
    if (bytes[i] == 0xE3 && bytes[i+1] == 0x07 && bytes[i+2] == 0x00 &&
        bytes[i+3] == 0x00 && bytes[i+4] == 0x80 && bytes[i+5] == 0x07 &&
        bytes[i+6] == 0xFF && bytes[i+7] == 0xFF && bytes[i+8] == 0x24 &&
        bytes[i+9] == 0x01) {
      patchLocation2 = i;
      DBG("Found SSE3 Title address at 0x%08x\n",patchLocation2);
      break;
    }
    i++;
  }            
  
  if (!patchLocation1 || !patchLocation2) {
    DBG("Can't found SSE3 data addres or Title address at 0x%08x 0x%08x\n", patchLocation1, patchLocation2);
    return;
  }
  
  DBG("Found SSE3 last data addres Start\n");
  i = patchLocation1 + 1500;
  //for (i=(patchLocation1+1500); i<(patchLocation1+3000); i++)
  while(TRUE) {
    if (bytes[i] == 0x90 && bytes[i+1] == 0x90 && bytes[i+2] == 0x55 ) {
      patchlast = (i+1) - patchLocation1;
      DBG("Found SSE3 last data addres at 0x%08x\n", patchlast);
      break;
    }
    i++;
  }
  
  if (!patchlast) {
    DBG("Can't found SSE3 data last addres at 0x%08x\n", patchlast);
    return;
  }
  // patch sse3_64 data
  
  for (i=0; i<patchlast; i++) {
    if (i<sizeof(sse3_patcher)) {
      bytes[patchLocation1 + i] = sse3_patcher[i];
    } else {
      bytes[patchLocation1 + i] = 0x90;
    }
  }
  
  // patch kHasSSE3 title
  bytes[patchLocation2 + 0] = 0xFC;
  bytes[patchLocation2 + 1] = 0x05;
  bytes[patchLocation2 + 8] = 0x2C;
  bytes[patchLocation2 + 9] = 0x00;
  
}

VOID Patcher_SSE3_5(VOID* kernelData)
{
  UINT8* bytes = (UINT8*)kernelData;
  UINT32 patchLocation1 = 0;
  UINT32 patchLocation2 = 0;
  UINT32 patchlast=0;
  UINT32 Length = sizeof(kernelData);
  UINT32 i; 
  
  DBG("Start find SSE3 address\n");
  
  for (i=256; i<(Length-256); i++) {
    if (bytes[i] == 0x66 && bytes[i+1] == 0x0F && bytes[i+2] == 0x6F && 
        bytes[i+3] == 0x44 && bytes[i+4] == 0x0E && bytes[i+5] == 0xF1 &&
        bytes[i-1680-32] == 0x55) {
      patchLocation1 = i-1680-32;
      DBG("Found SSE3 data address at 0x%08x\n",patchLocation1);
    }
    
    // khasSSE2+..... title
    if (bytes[i] == 0xF3 && bytes[i+1] == 0x07 && bytes[i+2] == 0x00 &&
        bytes[i+3] == 0x00 && bytes[i+4] == 0x80 && bytes[i+5] == 0x07 &&
        bytes[i+6] == 0xFF && bytes[i+7] == 0xFF && bytes[i+8] == 0x24 &&
        bytes[i+9] == 0x01) {
      patchLocation2 = i;
      DBG("Found SSE3 Title address at 0x%08x\n",patchLocation2);
      break;
    }
  }            
  
  if (!patchLocation1 || !patchLocation2) {
    DBG("Can't found SSE3 data addres or Title address at 0x%08x 0x%08x\n", patchLocation1, patchLocation2);
    return;
  }
  
  DBG("Found SSE3 last data addres Start\n");
  
  for (i=(patchLocation1+1500);i<Length;i++) {
    if (bytes[i] == 0x90 && bytes[i+1] == 0x90 && bytes[i+2] == 0x55) {
      patchlast = (i+1) - patchLocation1;
      DBG("Found SSE3 last data addres at 0x%08x\n", patchlast);
      break;
    }
  }
  
  if (!patchlast) {
    DBG("Can't found SSE3 data last addres at 0x%08x\n", patchlast);
    return;
  }
  
  // patech sse3_64 data
  
  for (i=0; i<patchlast; i++) {
    if (i<sizeof(sse3_5_patcher)) {
      bytes[patchLocation1 + i] = sse3_5_patcher[i];
    } else {
      bytes[patchLocation1 + i] = 0x90;
    }
  }
  
  // patch kHasSSE3 title
  bytes[patchLocation2 + 0] = 0x0C;
  bytes[patchLocation2 + 1] = 0x06;
  bytes[patchLocation2 + 8] = 0x2C;
  bytes[patchLocation2 + 9] = 0x00;
  
} 

VOID Patcher_SSE3_7(VOID* kernelData)
{
     // not support yet
     return;
}

VOID Get_PreLink()
{
  UINT32  ncmds, cmdsize;
  UINT32  binaryIndex;
  UINTN   cnt;
  UINT8*  binary = (UINT8*)KernelData;
  struct load_command         *loadCommand;
  struct  segment_command     *segCmd;
  struct segment_command_64   *segCmd64;
  
  
  if (is64BitKernel) {
    binaryIndex = sizeof(struct mach_header_64);
  } else {
    binaryIndex = sizeof(struct mach_header);
  }
  
  ncmds = MACH_GET_NCMDS(binary);
  
  for (cnt = 0; cnt < ncmds; cnt++) {
    loadCommand = (struct load_command *)(binary + binaryIndex);
    cmdsize = loadCommand->cmdsize;
    
    switch (loadCommand->cmd) {
      case LC_SEGMENT_64: 
        segCmd64 = (struct segment_command_64 *)loadCommand;
        //DBG("segCmd64->segname = %a\n",segCmd64->segname);
        //DBG("segCmd64->vmaddr = 0x%08x\n",segCmd64->vmaddr)
        //DBG("segCmd64->vmsize = 0x%08x\n",segCmd64->vmsize); 
        if (AsciiStrCmp(segCmd64->segname, kPrelinkTextSegment) == 0) {
          DBG("Found PRELINK_TEXT, 64bit\n");
          if (segCmd64->vmsize > 0) {
            // 64bit segCmd64->vmaddr is 0xffffff80xxxxxxxx
            // PrelinkTextAddr = xxxxxxxx + KernelRelocBase
            PrelinkTextAddr = (UINT32)(segCmd64->vmaddr ? segCmd64->vmaddr + KernelRelocBase : 0);
            PrelinkTextSize = (UINT32)segCmd64->vmsize;
            PrelinkTextLoadCmdAddr = (UINT32)(UINTN)segCmd64;
          }
          DBG("at %p: vmaddr = 0x%lx, vmsize = 0x%lx\n", segCmd64, segCmd64->vmaddr, segCmd64->vmsize);
          DBG("PrelinkTextLoadCmdAddr = 0x%x, PrelinkTextAddr = 0x%x, PrelinkTextSize = 0x%x\n",
              PrelinkTextLoadCmdAddr, PrelinkTextAddr, PrelinkTextSize);
          //DBG("cmd = 0x%08x\n",segCmd64->cmd);
          //DBG("cmdsize = 0x%08x\n",segCmd64->cmdsize);
          //DBG("vmaddr = 0x%08x\n",segCmd64->vmaddr);
          //DBG("vmsize = 0x%08x\n",segCmd64->vmsize);
          //DBG("fileoff = 0x%08x\n",segCmd64->fileoff);
          //DBG("filesize = 0x%08x\n",segCmd64->filesize);
          //DBG("maxprot = 0x%08x\n",segCmd64->maxprot);
          //DBG("initprot = 0x%08x\n",segCmd64->initprot);
          //DBG("nsects = 0x%08x\n",segCmd64->nsects);
          //DBG("flags = 0x%08x\n",segCmd64->flags);
        }
        if (AsciiStrCmp(segCmd64->segname, kPrelinkInfoSegment) == 0) {
          UINT32 sectionIndex;
          struct section_64 *sect;
          
          DBG("Found PRELINK_INFO, 64bit\n");
          //DBG("cmd = 0x%08x\n",segCmd64->cmd);
          //DBG("cmdsize = 0x%08x\n",segCmd64->cmdsize);
          DBG("vmaddr = 0x%08x\n",segCmd64->vmaddr);
          DBG("vmsize = 0x%08x\n",segCmd64->vmsize);
          //DBG("fileoff = 0x%08x\n",segCmd64->fileoff);
          //DBG("filesize = 0x%08x\n",segCmd64->filesize);
          //DBG("maxprot = 0x%08x\n",segCmd64->maxprot);
          //DBG("initprot = 0x%08x\n",segCmd64->initprot);
          //DBG("nsects = 0x%08x\n",segCmd64->nsects);
          //DBG("flags = 0x%08x\n",segCmd64->flags);
          sectionIndex = sizeof(struct segment_command_64);
          
          while(sectionIndex < segCmd64->cmdsize) {
            sect = (struct section_64 *)((UINT8*)segCmd64 + sectionIndex);
            sectionIndex += sizeof(struct section_64);
            
            if(AsciiStrCmp(sect->sectname, kPrelinkInfoSection) == 0 && AsciiStrCmp(sect->segname, kPrelinkInfoSegment) == 0) {
              if (sect->size > 0) {
                // 64bit sect->addr is 0xffffff80xxxxxxxx
                // PrelinkInfoAddr = xxxxxxxx + KernelRelocBase
                PrelinkInfoLoadCmdAddr = (UINT32)(UINTN)sect;
                PrelinkInfoAddr = (UINT32)(sect->addr ? sect->addr + KernelRelocBase : 0);
                PrelinkInfoSize = (UINT32)sect->size;
              }
              DBG("__info found at %p: addr = 0x%lx, size = 0x%lx\n", sect, sect->addr, sect->size);
              DBG("PrelinkInfoLoadCmdAddr = 0x%x, PrelinkInfoAddr = 0x%x, PrelinkInfoSize = 0x%x\n",
                  PrelinkInfoLoadCmdAddr, PrelinkInfoAddr, PrelinkInfoSize);
            }
          }
        }
        break;
        
      case LC_SEGMENT:
        segCmd = (struct segment_command *)loadCommand;
        //DBG("segCmd->segname = %a\n",segCmd->segname);
        //DBG("segCmd->vmaddr = 0x%08x\n",segCmd->vmaddr)
        //DBG("segCmd->vmsize = 0x%08x\n",segCmd->vmsize);
        if (AsciiStrCmp(segCmd->segname, kPrelinkTextSegment) == 0) {
          DBG("Found PRELINK_TEXT, 32bit\n");
          if (segCmd->vmsize > 0) {
            // PrelinkTextAddr = vmaddr + KernelRelocBase
            PrelinkTextAddr = (UINT32)(segCmd->vmaddr ? segCmd->vmaddr + KernelRelocBase : 0);
            PrelinkTextSize = (UINT32)segCmd->vmsize;
            PrelinkTextLoadCmdAddr = (UINT32)(UINTN)segCmd;
          }
          DBG("at %p: vmaddr = 0x%lx, vmsize = 0x%lx\n", segCmd, segCmd->vmaddr, segCmd->vmsize);
          DBG("PrelinkTextLoadCmdAddr = 0x%x, PrelinkTextAddr = 0x%x, PrelinkTextSize = 0x%x\n",
              PrelinkTextLoadCmdAddr, PrelinkTextAddr, PrelinkTextSize);
          //gBS->Stall(30*1000000);
        }
        if (AsciiStrCmp(segCmd->segname, kPrelinkInfoSegment) == 0) {
          UINT32 sectionIndex;
          struct section *sect;
          
          DBG("Found PRELINK_INFO, 32bit\n");
          //DBG("cmd = 0x%08x\n",segCmd->cmd);
          //DBG("cmdsize = 0x%08x\n",segCmd->cmdsize);
          DBG("vmaddr = 0x%08x\n",segCmd->vmaddr);
          DBG("vmsize = 0x%08x\n",segCmd->vmsize);
          //DBG("fileoff = 0x%08x\n",segCmd->fileoff);
          //DBG("filesize = 0x%08x\n",segCmd->filesize);
          //DBG("maxprot = 0x%08x\n",segCmd->maxprot);
          //DBG("initprot = 0x%08x\n",segCmd->initprot);
          //DBG("nsects = 0x%08x\n",segCmd->nsects);
          //DBG("flags = 0x%08x\n",segCmd->flags);
          sectionIndex = sizeof(struct segment_command);
          
          while(sectionIndex < segCmd->cmdsize) {
            sect = (struct section *)((UINT8*)segCmd + sectionIndex);
            sectionIndex += sizeof(struct section);
            
            if(AsciiStrCmp(sect->sectname, kPrelinkInfoSection) == 0 && AsciiStrCmp(sect->segname, kPrelinkInfoSegment) == 0) {
              if (sect->size > 0) {
                // PrelinkInfoAddr = sect->addr + KernelRelocBase
                PrelinkInfoLoadCmdAddr = (UINT32)(UINTN)sect;
                PrelinkInfoAddr = (UINT32)(sect->addr ? sect->addr + KernelRelocBase : 0);
                PrelinkInfoSize = (UINT32)sect->size;
              }
              DBG("__info found at %p: addr = 0x%lx, size = 0x%lx\n", sect, sect->addr, sect->size);
              DBG("PrelinkInfoLoadCmdAddr = 0x%x, PrelinkInfoAddr = 0x%x, PrelinkInfoSize = 0x%x\n",
                  PrelinkInfoLoadCmdAddr, PrelinkInfoAddr, PrelinkInfoSize);
              //gBS->Stall(30*1000000);
            }
          }
        }
        break;
        
      default:
        break;
    }
    binaryIndex += cmdsize;
  }
  
  //gBS->Stall(20*1000000);
  return;
}

VOID
FindBootArgs(VOID)
{
  UINT8           *ptr;
  UINT8           archMode = sizeof(UINTN) * 8;
  
  // start searching from 0x200000.
  ptr = (UINT8*)(UINTN)0x200000;
  
  
  while(TRUE) {
    
    // check bootargs for 10.7 and up
    bootArgs2 = (BootArgs2*)ptr;
    
    if (bootArgs2->Version==2 && bootArgs2->Revision==0
        // plus additional checks - some values are not inited by boot.efi yet
        && bootArgs2->efiMode == archMode
        && bootArgs2->kaddr == 0 && bootArgs2->ksize == 0
        && bootArgs2->efiSystemTable == 0
        ) {
      // set vars
      dtRoot = (CHAR8*)(UINTN)bootArgs2->deviceTreeP;
      KernelSlide = bootArgs2->kslide;
      
      DBG("Found bootArgs2 at 0x%08x, DevTree at %p\n", ptr, dtRoot);
      //DBG("bootArgs2->kaddr = 0x%08x and bootArgs2->ksize =  0x%08x\n", bootArgs2->kaddr, bootArgs2->ksize);
      //DBG("bootArgs2->efiMode = 0x%02x\n", bootArgs2->efiMode);
      DBG("bootArgs2->CommandLine = %a\n", bootArgs2->CommandLine);
      DBG("bootArgs2->flags = %x %x\n", bootArgs2->flags);
      DBG("bootArgs2->kslide = %x\n", bootArgs2->kslide);
      //gBS->Stall(5000000);
      
      // disable other pointer
      bootArgs1 = NULL;
      break;
    }
    
    // check bootargs for 10.4 - 10.6.x  
    bootArgs1 = (BootArgs1*)ptr;
    
    if (bootArgs1->Version==1
        && (bootArgs1->Revision==6 || bootArgs1->Revision==5 || bootArgs1->Revision==4)
        // plus additional checks - some values are not inited by boot.efi yet
        && bootArgs1->efiMode == archMode
        && bootArgs1->kaddr == 0 && bootArgs1->ksize == 0
        && bootArgs1->efiSystemTable == 0
        ) {
      // set vars
      dtRoot = (CHAR8*)(UINTN)bootArgs1->deviceTreeP;
      
      DBG("Found bootArgs1 at 0x%08x, DevTree at %p\n", ptr, dtRoot);
      //DBG("bootArgs1->kaddr = 0x%08x and bootArgs1->ksize =  0x%08x\n", bootArgs1->kaddr, bootArgs1->ksize);
      //DBG("bootArgs1->efiMode = 0x%02x\n", bootArgs1->efiMode);
      
      // disable other pointer
      bootArgs2 = NULL;
      break;
    }
    
    ptr += 0x1000;
  }
}

VOID
KernelAndKextPatcherInit(VOID)
{
  if (PatcherInited) {
    return;
  }
  
  PatcherInited = TRUE;
  
  // KernelRelocBase will normally be 0
  // but if OsxAptioFixDrv is used, then it will be > 0
  SetKernelRelocBase();
  DBG("KernelRelocBase = %lx\n", KernelRelocBase);
  
  // Find bootArgs - we need then for proper detection
  // of kernel Mach-O header
  FindBootArgs();
  if (bootArgs1 == NULL && bootArgs2 == NULL) {
    DBG("BootArgs not found - skipping patches!\n");
    return;
  }
  
  // Find kernel Mach-O header:
  // for ML: bootArgs2->kslide + 0x00200000
  // for older versions: just 0x200000
  // for AptioFix booting - it's always at KernelRelocBase + 0x200000
  KernelData = (VOID*)(UINTN)(KernelSlide + KernelRelocBase + 0x00200000);
  
  // check that it is Mach-O header and detect architecture
  if(MACH_GET_MAGIC(KernelData) == MH_MAGIC || MACH_GET_MAGIC(KernelData) == MH_CIGAM) {
    DBG("Found 32 bit kernel at 0x%p\n", KernelData);
    is64BitKernel = FALSE;
  } else if (MACH_GET_MAGIC(KernelData) == MH_MAGIC_64 || MACH_GET_MAGIC(KernelData) == MH_CIGAM_64) {
    DBG("Found 64 bit kernel at 0x%p\n", KernelData);
    is64BitKernel = TRUE;
  } else {
    // not valid Mach-O header - exiting
    DBG("Kernel not found at 0x%p - skipping patches!", KernelData);
    KernelData = NULL;
    return;
  }
  
  // find __PRELINK_TEXT and __PRELINK_INFO
  Get_PreLink();
    
  isKernelcache = PrelinkTextSize > 0 && PrelinkInfoSize > 0;
  DBG("isKernelcache: %s\n", isKernelcache ? L"Yes" : L"No");
}

VOID
KernelAndKextsPatcherStart(IN LOADER_ENTRY *Entry)
{
  // we will call KernelAndKextPatcherInit() only if needed
  
  DBG_RT("\nKernelCpu patch: ");
  if (gSettings.KPKernelCpu) {    
    //
    // Kernel patches
    //
    DBG_RT("Enabled: ");
 /*   if ((gCPUStructure.Family!=0x06 && AsciiStrStr(OSVersion,"10.7")!=0)||
        (gCPUStructure.Model==CPU_MODEL_ATOM &&
         ((AsciiStrStr(OSVersion,"10.7")!=0) || AsciiStrStr(OSVersion,"10.6")!=0)) ||
        (gCPUStructure.Model==CPU_MODEL_IVY_BRIDGE && AsciiStrStr(OSVersion,"10.7")!=0) ||
        (gCPUStructure.Model==CPU_MODEL_IVY_BRIDGE_E5 && AsciiStrStr(OSVersion,"10.7")!=0)
        ) { */
      KernelAndKextPatcherInit();
      if (KernelData == NULL) {
        if (gSettings.KPDebug) {
          DBG_RT("ERROR: Kernel not found\n");
          gBS->Stall(5000000);
        }
        return;
      }
      
      if(is64BitKernel) {
        DBG_RT("64 bit patch ...");
        KernelPatcher_64(KernelData, Entry->OSVersion);
      } else {
        DBG_RT("32 bit patch ...");
        KernelPatcher_32(KernelData, Entry->OSVersion);
      }
      DBG_RT(" OK\n");
 /*   } else {
      DBG_RT(" Not executed!\n");
    } */
  } else {
    DBG_RT("Not done - Disabled.\n");
  }

  // CPU power management patch for haswell with locked msr
  DBG_RT("\nKernelPm patch: ");
  if (gSettings.KPKernelPm) {
    DBG_RT("Enabled: ");
    KernelAndKextPatcherInit();
    if (KernelData == NULL) {
      if (gSettings.KPDebug) {
        DBG_RT("ERROR: Kernel not found\n");
        gBS->Stall(5000000);
      }
      return;
    }
    if (is64BitKernel) {
      KernelPatchPm(KernelData);
    }
  } else {
    DBG_RT("Not done - Disabled.\n");
  }
  
  // Lapic Panic Kernel Patch
  
  DBG_RT("\nKernelLapic patch: ");
  if (gSettings.KPLapicPanic) {
    BOOLEAN patchedOk;
    KernelAndKextPatcherInit();
    if (KernelData == NULL) {
      if (gSettings.KPDebug) {
        DBG_RT("ERROR: Kernel not found\n");
        gBS->Stall(5000000);
      }
      return;
    }

    if(is64BitKernel) {
      DBG_RT("64-bit patch ...\n");
      patchedOk = KernelLapicPatch_64(KernelData);
    } else {
      DBG_RT("32-bit patch ...\n");
      patchedOk = KernelLapicPatch_32(KernelData);
    }
    if(patchedOk) {
      DBG_RT(" \nOK\n");
    } else {
      DBG_RT(" FAILED!\n");
    }
  } else {
    DBG_RT("Not done - Disabled.\n");
  }
  
  if (gSettings.KPDebug) {
    gBS->Stall(5000000);
  }
  
  //
  // Kext patches
  //
  
  // we need to scan kexts if "InjectKexts if no FakeSMC"
  if (gSettings.WithKextsIfNoFakeSMC) {
    DBG_RT("\nInjectKexts if no FakeSMC specified - we need kext patching to search for FakeSMC\n");
    gSettings.KPKextPatchesNeeded = TRUE;
    gSettings.KextPatchesAllowed = TRUE;
  }
  
  DBG_RT("\nKextPatches Needed: %c, Allowed: %c ... ",
         (gSettings.KPKextPatchesNeeded ? L'Y' : L'n'),
         (gSettings.KextPatchesAllowed ? L'Y' : L'n')
         );
  if (gSettings.KPKextPatchesNeeded && gSettings.KextPatchesAllowed) {
    KernelAndKextPatcherInit();
    if (KernelData == NULL) {
      if (gSettings.KPDebug) {
        DBG_RT("ERROR: Kernel not found\n");
        gBS->Stall(5000000);
      }
      return;
    }
    
    DBG_RT("\nKext patching STARTED\n");
    KextPatcherStart();
    DBG_RT("\nKext patching ENDED\n");
    
  } else {
    DBG_RT("Not needed or not allowed\n");
  }
  if (gSettings.KPDebug) {
    DBG_RT("Pausing 10 secs ...\n\n");
    gBS->Stall(10000000);
  }
  
  //
  // Kext add
  //
  if (Entry != 0 && gSettings.WithKextsIfNoFakeSMC && gSettings.FakeSMCFound) {
    // disable kext injection if FakeSMC is already present
    Entry->Flags = OSFLAG_UNSET(Entry->Flags, OSFLAG_WITHKEXTS);
    if (gSettings.KPDebug) {
      DBG_RT("\nInjectKexts if no FakeSMC is requested and FakeSMC is found - disabling kext injection\n");
      gBS->Stall(5000000);
    }
  }
  if ((Entry != 0) && OSFLAG_ISSET(Entry->Flags, OSFLAG_WITHKEXTS))
  {
    UINT32      deviceTreeP;
    UINT32      deviceTreeLength;
    EFI_STATUS  Status;
    UINTN       DataSize;
    
    // check if FSInject already injected kexts
    DataSize = 0;
    Status = gRT->GetVariable (L"FSInject.KextsInjected", &gEfiGlobalVariableGuid, NULL, &DataSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      // var exists - just exit
      if (gSettings.KPDebug) {
        DBG_RT("\nInjectKexts: skipping, FSInject already injected them\n");
        gBS->Stall(5000000);
      }
      return;
    }
    
    KernelAndKextPatcherInit();
    if (KernelData == NULL) {
      return;
    }
    
    if (bootArgs1 != NULL) {
      deviceTreeP = bootArgs1->deviceTreeP;
      deviceTreeLength = bootArgs1->deviceTreeLength;
    } else if (bootArgs2 != NULL) {
      deviceTreeP = bootArgs2->deviceTreeP;
      deviceTreeLength = bootArgs2->deviceTreeLength;
    } else return;
    
    Status = InjectKexts(deviceTreeP, &deviceTreeLength);
    
    if (!EFI_ERROR(Status)) KernelBooterExtensionsPatch(KernelData);
  }
  
}
