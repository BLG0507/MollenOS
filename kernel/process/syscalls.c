/* MollenOS
 *
 * Copyright 2011 - 2017, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * MollenOS MCore - System Calls
 */
#define __MODULE "SCIF"
//#define __TRACE

/* Includes 
 * - System */
#include <system/interrupts.h>
#include <system/iospace.h>
#include <system/thread.h>
#include <system/utils.h>
#include <system/video.h>

#include <process/process.h>
#include <threading.h>
#include <scheduler.h>
#include <interrupts.h>
#include <machine.h>
#include <timers.h>
#include <video.h>
#include <debug.h>
#include <heap.h>
#include <log.h>

/* Includes
 * - Library */
#include "../../../librt/libc/os/driver/private.h"
#include <ds/mstring.h>
#include <os/ipc/ipc.h>
#include <os/file.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>

/* Shorthand */
#define DefineSyscall(_Sys) ((uintptr_t)&_Sys)

/* ScSystemDebug 
 * Debug/trace printing for userspace application and drivers */
OsStatus_t
ScSystemDebug(
    _In_ int Type,
    _In_ __CONST char *Module,
    _In_ __CONST char *Message)
{
    // Validate params
    if (Module == NULL || Message == NULL) {
        return OsError;
    }

    // Switch based on type
    if (Type == 0) {
        LogAppendMessage(LogTrace, Module, Message);
    }
    else if (Type == 1) {
        LogAppendMessage(LogDebug, Module, Message);
    }
    else {
        LogAppendMessage(LogError, Module, Message);
    }
    return OsSuccess;
}

/*******************************************************************************
 * Process Extensions
 *******************************************************************************/

/* ScProcessSpawn
 * Spawns a new process with the given path
 * and the given arguments, returns UUID_INVALID on failure */
UUId_t
ScProcessSpawn(
    _In_ const char*                            Path,
    _In_ const ProcessStartupInformation_t*     StartupInformation,
    _In_ int                                    Asynchronous)
{
    // Variables
    MCorePhoenixRequest_t *Request  = NULL;
    MString_t *mPath                = NULL;
    UUId_t Result                   = UUID_INVALID;

    // Only the path cannot be null
    // Arguments are allowed to be null
    if (Path == NULL || StartupInformation == NULL) {
        return UUID_INVALID;
    }

    // Allocate resources for the spawn
    Request = (MCorePhoenixRequest_t*)kmalloc(sizeof(MCorePhoenixRequest_t));
    mPath = MStringCreate((void*)Path, StrUTF8);

    // Reset structure and set it up
    memset(Request, 0, sizeof(MCorePhoenixRequest_t));
    Request->Base.Type = AshSpawnProcess;
    Request->Path = mPath;
    Request->Base.Cleanup = Asynchronous;
    
    // Copy startup-information
    if (StartupInformation->ArgumentPointer != NULL
        && StartupInformation->ArgumentLength != 0) {
        Request->StartupInformation.ArgumentLength = 
            StartupInformation->ArgumentLength;
        Request->StartupInformation.ArgumentPointer = 
            (__CONST char*)kmalloc(StartupInformation->ArgumentLength);
        memcpy((void*)Request->StartupInformation.ArgumentPointer,
            StartupInformation->ArgumentPointer, 
            StartupInformation->ArgumentLength);
    }
    if (StartupInformation->InheritanceBlockPointer != NULL
        && StartupInformation->InheritanceBlockLength != 0) {
        Request->StartupInformation.InheritanceBlockLength = 
            StartupInformation->InheritanceBlockLength;
        Request->StartupInformation.InheritanceBlockPointer = 
            (__CONST char*)kmalloc(StartupInformation->InheritanceBlockLength);
        memcpy((void*)Request->StartupInformation.InheritanceBlockPointer,
            StartupInformation->InheritanceBlockPointer, 
            StartupInformation->InheritanceBlockLength);
    }

    // If it's an async request we return immediately
    // We return an invalid UUID as it cannot be used
    // for queries
    PhoenixCreateRequest(Request);
    if (Asynchronous != 0) {
        return UUID_INVALID;
    }

    // Otherwise wait for request to complete
    // and then cleanup and return the process id
    PhoenixWaitRequest(Request, 0);
    MStringDestroy(mPath);

    // Store result and cleanup
    Result = Request->AshId;
    kfree(Request);
    return Result;
}

/* ScProcessJoin
 * Wait for a given process to exit, optionaly specifying a timeout.
 * The exit-code for the process will be returned. */
OsStatus_t
ScProcessJoin(
    _In_  UUId_t    ProcessId,
    _In_  size_t    Timeout,
    _Out_ int*      ExitCode)
{
    // Variables
    MCoreAsh_t *Process = PhoenixGetAsh(ProcessId);
    int SleepResult     = 0;
    
    if (Process == NULL) {
        return OsError;
    }
    SleepResult = SchedulerThreadSleep((uintptr_t*)Process, Timeout);
    if (SleepResult == SCHEDULER_SLEEP_OK) {
        if (ExitCode != NULL) {
            *ExitCode = Process->Code;
        }
        return OsSuccess;
    }
    return OsError;
}

/* ScProcessKill
 * Kills the given process-id, it does not guarantee instant kill. */
OsStatus_t
ScProcessKill(
    _In_ UUId_t ProcessId)
{
    // Variables
    MCorePhoenixRequest_t Request;

    // Initialize the request
    memset(&Request, 0, sizeof(MCorePhoenixRequest_t));
    Request.Base.Type = AshKill;
    Request.AshId = ProcessId;

    // Create and wait with 1 second timeout
    PhoenixCreateRequest(&Request);
    PhoenixWaitRequest(&Request, 1000);
    if (Request.Base.State == EventOk) {
        return OsSuccess;
    }
    else {
        return OsError;
    }
}

/* ScProcessExit
 * Kills the entire process and all non-detached threads that
 * has been spawned by the process. */
OsStatus_t
ScProcessExit(
    _In_ int ExitCode)
{
    // Variables
    MCoreAsh_t *Process     = PhoenixGetCurrentAsh();
    if (Process == NULL) {
        return OsError;
    }
    
    // Debug
    WARNING("Process %s terminated with code %i", MStringRaw(Process->Name), ExitCode);

    // Terminate ash and current thread
    PhoenixTerminateAsh(Process, ExitCode, 0, 1);
    ThreadingKillThread(ThreadingGetCurrentThreadId(), ExitCode, 1);
    
    // @unreachable
    return OsSuccess;
}

/* ScProcessGetCurrentId 
 * Retrieves the current process identifier. */
OsStatus_t 
ScProcessGetCurrentId(
    UUId_t *ProcessId)
{
    MCoreAsh_t *Process = PhoenixGetCurrentAsh();
    if (ProcessId == NULL || Process == NULL) {
        return OsError;
    }
    return Process->Id;
}

/* ScProcessGetCurrentName
 * Retreieves the current process name */
OsStatus_t
ScProcessGetCurrentName(const char *Buffer, size_t MaxLength) {
    MCoreAsh_t *Process = PhoenixGetCurrentAsh();
    if (Process == NULL) {
        return OsError;
    }
    memset((void*)Buffer, 0, MaxLength);
    memcpy((void*)Buffer, MStringRaw(Process->Name), MIN(MStringSize(Process->Name), MaxLength));
    return OsSuccess;
}

/* ScProcessSignal
 * Installs a default signal handler for the given process. */
OsStatus_t
ScProcessSignal(
    _In_ uintptr_t Handler) 
{
    // Process
    MCoreAsh_t *Process = NULL;

    // Get current process
    Process = PhoenixGetCurrentAsh();
    if (Process == NULL) {
        return OsError;
    }

    Process->SignalHandler = Handler;
    return OsSuccess;
}

/* Dispatches a signal to the target process id 
 * It will get handled next time it's selected for execution 
 * so we yield instantly as well. If processid is -1, we select self */
OsStatus_t
ScProcessRaise(
    _In_ UUId_t ProcessId, 
    _In_ int    Signal)
{
    // Variables
    MCoreProcess_t *Process = NULL;

    // Lookup process
    Process = PhoenixGetProcess(ProcessId);
    if (Process == NULL) {
        return OsError;
    }

    // Create the signal
    return SignalCreate(Process->Base.MainThread, Signal);
}

/* ScProcessGetStartupInformation
 * Retrieves information passed about process startup. */
OsStatus_t
ScProcessGetStartupInformation(
    _InOut_ ProcessStartupInformation_t *StartupInformation)
{
    // Variables
    MCoreProcess_t *Process = NULL;

    // Sanitize parameters
    if (StartupInformation == NULL) {
        return OsError;
    }

    // Find process
    Process = PhoenixGetCurrentProcess();
    if (Process == NULL) {
        return OsError;
    }

    // Update outs
    if (Process->StartupInformation.ArgumentPointer != NULL) {
        if (StartupInformation->ArgumentPointer != NULL) {
            memcpy((void*)StartupInformation->ArgumentPointer, 
                Process->StartupInformation.ArgumentPointer,
                MIN(StartupInformation->ArgumentLength, 
                    Process->StartupInformation.ArgumentLength));
        }
        else {
            StartupInformation->ArgumentLength = 
                Process->StartupInformation.ArgumentLength;
        }
    }
    if (Process->StartupInformation.InheritanceBlockPointer != NULL) {
        if (StartupInformation->InheritanceBlockPointer != NULL) {
            size_t BytesToCopy = MIN(StartupInformation->InheritanceBlockLength, 
                    Process->StartupInformation.InheritanceBlockLength);
            memcpy((void*)StartupInformation->InheritanceBlockPointer, 
                Process->StartupInformation.InheritanceBlockPointer, BytesToCopy);
            StartupInformation->InheritanceBlockLength = BytesToCopy;
        }
        else {
            StartupInformation->InheritanceBlockLength = 0;
        }
    }

    // Done
    return OsSuccess;
}

/* ScProcessGetModuleHandles
 * Retrieves a list of loaded module handles. Handles can be queried
 * for various application-image data. */
OsStatus_t
ScProcessGetModuleHandles(
    _In_ Handle_t ModuleList[PROCESS_MAXMODULES])
{
    // Variables
    MCoreAsh_t *Process = NULL;

    // Get current process
    Process = PhoenixGetCurrentAsh();
    if (Process == NULL) {
        return OsError;
    }

    // Redirect call to executable interface
    return PeGetModuleHandles(Process->Executable, ModuleList);
}

/* ScProcessGetModuleEntryPoints
 * Retrieves a list of loaded module entry points. */
OsStatus_t
ScProcessGetModuleEntryPoints(
    _In_ Handle_t ModuleList[PROCESS_MAXMODULES])
{
    // Variables
    MCoreAsh_t *Process = NULL;

    // Get current process
    Process = PhoenixGetCurrentAsh();
    if (Process == NULL) {
        return OsError;
    }

    // Redirect call to executable interface
    return PeGetModuleEntryPoints(Process->Executable, ModuleList);
}

/*******************************************************************************
 * Shared Object Functions
 *******************************************************************************/

/* ScSharedObjectLoad
 * Load a shared object given a path 
 * path must exists otherwise NULL is returned */
Handle_t
ScSharedObjectLoad(
    _In_  const char*   SharedObject)
{
    // Variables
    MCoreAsh_t *Process     = PhoenixGetCurrentAsh();
    MString_t *Path         = NULL;
    uintptr_t BaseAddress   = 0;
    Handle_t Handle         = HANDLE_INVALID;

    // Sanitize the process
    if (Process == NULL) {
        return HANDLE_INVALID;
    }

    // Sanitize the given shared-object path
    // If null, get handle to current assembly
    if (SharedObject == NULL) {
        return HANDLE_GLOBAL;
    }

    // Create a mstring object from the string
    Path = MStringCreate((void*)SharedObject, StrUTF8);

    // Try to resolve the library
    BaseAddress = Process->NextLoadingAddress;
    Handle = (Handle_t)PeResolveLibrary(Process->Executable, NULL, Path, &BaseAddress);
    Process->NextLoadingAddress = BaseAddress;

    // Cleanup the mstring object
    MStringDestroy(Path);
    return Handle;
}

/* ScSharedObjectGetFunction
 * Load a function-address given an shared object
 * handle and a function name, function must exist
 * otherwise null is returned */
uintptr_t
ScSharedObjectGetFunction(
    _In_ Handle_t       Handle, 
    _In_ const char*    Function)
{
    // Validate parameters
    if (Handle == HANDLE_INVALID || Function == NULL) {
        return 0;
    }

    // If the handle is the handle_global, search all loaded
    // libraries for the symbol
    if (Handle == HANDLE_GLOBAL) {
        MCoreAsh_t *Process = PhoenixGetCurrentAsh();
        uintptr_t Address   = 0;
        if (Process != NULL) {
            Address = PeResolveFunction(Process->Executable, Function);
            if (!Address) {
                foreach(Node, Process->Executable->LoadedLibraries) {
                    Address = PeResolveFunction((MCorePeFile_t*)Node->Data, Function);
                    if (Address != 0) {
                        break;
                    }
                }
            }
        }
        return Address;
    }
    else {
        return PeResolveFunction((MCorePeFile_t*)Handle, Function);
    }
}

/* Unloads a valid shared object handle
 * returns 0 on success */
OsStatus_t
ScSharedObjectUnload(
    _In_  Handle_t  Handle)
{
    // Variables
    MCoreAsh_t *Process = PhoenixGetCurrentAsh();
    if (Process == NULL || Handle == HANDLE_INVALID) {
        return OsError;
    }
    if (Handle == HANDLE_GLOBAL) { // Never close running handle
        return OsSuccess;
    }
    return PeUnloadLibrary(Process->Executable, (MCorePeFile_t*)Handle);
}

/*******************************************************************************
 * Threading Functions
 *******************************************************************************/

/* ScThreadCreate
 * Creates a new thread bound to 
 * the calling process, with the given entry point and arguments */
UUId_t
ScThreadCreate(
    _In_ ThreadEntry_t      Entry, 
    _In_ void*              Data, 
    _In_ Flags_t            Flags) {
    // Sanitize parameters
    if (Entry == NULL) {
        return UUID_INVALID;
    }
    return ThreadingCreateThread(NULL, Entry, Data, ThreadingGetCurrentMode() | THREADING_INHERIT | Flags);
}

/* ScThreadExit
 * Exits the current thread and instantly yields control to scheduler */
OsStatus_t
ScThreadExit(
    _In_ int ExitCode) {
    ThreadingExitThread(ExitCode);
    return OsSuccess;
}

/* ScThreadJoin
 * Thread join, waits for a given
 * thread to finish executing, and returns it's exit code */
OsStatus_t
ScThreadJoin(
    _In_  UUId_t    ThreadId,
    _Out_ int*      ExitCode)
{
    // Variables
    UUId_t PId      = ThreadingGetCurrentThread(CpuGetCurrentId())->AshId;
    int ResultCode  = 0;

    // Perform security checks
    if (ThreadingGetThread(ThreadId) == NULL
        || ThreadingGetThread(ThreadId)->AshId != PId) {
        return OsError;
    }
    ResultCode = ThreadingJoinThread(ThreadId);
    if (ExitCode != NULL) {
        *ExitCode = ResultCode;
    }
    return OsSuccess;
}

/* ScThreadDetach
 * Unattaches a running thread from a process, making sure it lives
 * past the lifetime of a process unless killed. */
OsStatus_t
ScThreadDetach(
    _In_  UUId_t    ThreadId)
{
    // Variables
    MCoreThread_t *Thread   = ThreadingGetCurrentThread(CpuGetCurrentId());
    MCoreThread_t *Target   = ThreadingGetThread(ThreadId);
    UUId_t PId              = Thread->AshId;

    // Perform security checks
    if (Target == NULL || Target->AshId != PId) {
        return OsError;
    }
    
    // Perform the detach
    Target->Flags |= THREADING_DETACHED;
    return OsSuccess;
}

/* ScThreadSignal
 * Kills the thread with the given id, owner must be same process */
OsStatus_t
ScThreadSignal(
    _In_ UUId_t     ThreadId,
    _In_ int        SignalCode)
{
    // Variables
    UUId_t PId = ThreadingGetCurrentThread(CpuGetCurrentId())->AshId;

    // Perform security checks
    if (ThreadingGetThread(ThreadId) == NULL
        || ThreadingGetThread(ThreadId)->AshId != PId) {
        ERROR("Thread does not belong to same process");
        return OsError;
    }
    return SignalCreate(ThreadId, SignalCode);
}

/* ScThreadSleep
 * Sleeps the current thread for the given milliseconds. */
OsStatus_t
ScThreadSleep(
    _In_  time_t    Milliseconds,
    _Out_ time_t*   MillisecondsSlept)
{
    // Variables
    clock_t Start   = 0;
    clock_t End     = 0;
    
    // Initiate start
    TimersGetSystemTick(&Start);
    if (SchedulerThreadSleep(NULL, Milliseconds) == SCHEDULER_SLEEP_INTERRUPTED) {
        End = ThreadingGetCurrentThread(CpuGetCurrentId())->Sleep.InterruptedAt;
    }
    else {
        TimersGetSystemTick(&End);
    }

    // Update outs
    if (MillisecondsSlept != NULL) {
        *MillisecondsSlept = (time_t)(End - Start);
    }
    return OsSuccess;
}

/* ScThreadGetCurrentId
 * Retrieves the thread id of the calling thread */
UUId_t
ScThreadGetCurrentId(void) {
    return ThreadingGetCurrentThreadId();
}

/* ScThreadYield
 * This yields the current thread and gives cpu time to another thread */
OsStatus_t
ScThreadYield(void) {
    ThreadingYield();
    return OsSuccess;
}

/* ScThreadSetCurrentName
 * Changes the name of the current thread to the one specified by ThreadName. */
OsStatus_t
ScThreadSetCurrentName(const char *ThreadName) 
{
    // Variables
    MCoreThread_t *Thread       = ThreadingGetCurrentThread(CpuGetCurrentId());
    const char *PreviousName    = NULL;

    if (Thread == NULL || ThreadName == NULL) {
        return OsError;
    }
    PreviousName = Thread->Name;
    Thread->Name = strdup(ThreadName);
    kfree((void*)PreviousName);
    return OsSuccess;
}

/* ScThreadGetCurrentName
 * Retrieves the name of the current thread, up to max specified bytes. */
OsStatus_t
ScThreadGetCurrentName(char *ThreadNameBuffer, size_t MaxLength)
{
    // Variables
    MCoreThread_t *Thread       = ThreadingGetCurrentThread(CpuGetCurrentId());

    if (Thread == NULL || ThreadNameBuffer == NULL) {
        return OsError;
    }
    strncpy(ThreadNameBuffer, Thread->Name, MaxLength);
    return OsSuccess;
}

/***********************
* Synch Functions      *
***********************/

/* ScConditionCreate
 * Create a new shared handle 
 * that is unique for a condition variable */
OsStatus_t
ScConditionCreate(
    _Out_ Handle_t *Handle)
{
    // Sanitize input
    if (Handle == NULL) {
        return OsError;
    }
 
    *Handle = (Handle_t)kmalloc(sizeof(Handle_t));
    return OsSuccess;
}

/* ScConditionDestroy
 * Destroys a shared handle
 * for a condition variable */
OsStatus_t
ScConditionDestroy(
    _In_ Handle_t Handle)
{
    kfree(Handle);
    return OsSuccess;
}

/* ScSignalHandle
 * Signals a handle for wakeup 
 * This is primarily used for condition
 * variables and semaphores */
OsStatus_t
ScSignalHandle(
    _In_ uintptr_t *Handle)
{
    return SchedulerHandleSignal(Handle);
}

/* Signals a handle for wakeup all
 * This is primarily used for condition
 * variables and semaphores */
OsStatus_t
ScSignalHandleAll(
    _In_ uintptr_t *Handle)
{
    SchedulerHandleSignalAll(Handle);
    return OsSuccess;
}

/* ScWaitForObject
 * Waits for a signal relating to the above function, this
 * function uses a timeout. Returns OsError on timed-out */
OsStatus_t
ScWaitForObject(
    _In_ uintptr_t *Handle,
    _In_ size_t Timeout)
{
    // Store reason for waking up
    int WakeReason = SchedulerThreadSleep(Handle, Timeout);
    if (WakeReason == SCHEDULER_SLEEP_OK) {
        return OsSuccess;
    }
    else {
        return OsError;
    }
}

/***********************
* Memory Functions     *
***********************/
#include <os/mollenos.h>

/* ScMemoryAllocate
 * Allows a process to allocate memory
 * from the userheap, it takes a size and allocation flags */
OsStatus_t
ScMemoryAllocate(
    _In_  size_t        Size, 
    _In_  Flags_t       Flags, 
    _Out_ uintptr_t*    VirtualAddress,
    _Out_ uintptr_t*    PhysicalAddress)
{
    // Variables
    uintptr_t AllocatedAddress  = 0;
    MCoreAsh_t *Ash;

    // Locate the current running process
    Ash = PhoenixGetCurrentAsh();
    if (Ash == NULL || Size == 0) {
        return OsError;
    }
    
    // Now do the allocation in the user-bitmap 
    // since memory is managed in userspace for speed
    AllocatedAddress = AllocateBlocksInBlockmap(Ash->Heap, __MASK, Size);
    if (AllocatedAddress == 0) {
        return OsError;
    }

    // Force a commit of memory if any flags
    // is given, because we can't apply flags later
    if (Flags != 0) {
        Flags |= MEMORY_COMMIT;
    }

    // Handle flags
    // If the commit flag is not given the flags won't be applied
    if (Flags & MEMORY_COMMIT) {
        int ExtendedFlags = MAPPING_USERSPACE | MAPPING_FIXED;

        // Build extensions
        if (Flags & MEMORY_CONTIGIOUS) {
            ExtendedFlags |= MAPPING_CONTIGIOUS;
        }
        if (Flags & MEMORY_UNCHACHEABLE) {
            ExtendedFlags |= MAPPING_NOCACHE;
        }
        if (Flags & MEMORY_LOWFIRST) {
            // Handle mask
        }

        // Do the actual mapping
        if (CreateSystemMemorySpaceMapping(GetCurrentSystemMemorySpace(), 
            PhysicalAddress, &AllocatedAddress, Size, ExtendedFlags, __MASK) != OsSuccess) {
            ReleaseBlockmapRegion(Ash->Heap, AllocatedAddress, Size);
            *VirtualAddress = 0;
            return OsError;
        }

        // Handle post allocation flags
        if (Flags & MEMORY_CLEAN) {
            memset((void*)AllocatedAddress, 0, Size);
        }
    }
    else {
        *PhysicalAddress = 0;
    }

    // Update out and return
    *VirtualAddress = (uintptr_t)AllocatedAddress;
    return OsSuccess;
}

/* Free's previous allocated memory, given an address
 * and a size (though not needed for now!) */
OsStatus_t 
ScMemoryFree(
    _In_ uintptr_t  Address, 
    _In_ size_t     Size)
{
    // Variables
    MCoreAsh_t *Ash = NULL;

    // Locate the current running process
    Ash = PhoenixGetCurrentAsh();
    if (Ash == NULL || Address == 0 || Size == 0) {
        return OsError;
    }

    // Now do the deallocation in the user-bitmap 
    // since memory is managed in userspace for speed
    if (ReleaseBlockmapRegion(Ash->Heap, Address, Size) != OsSuccess) {
        ERROR("ScMemoryFree(Address 0x%x, Size 0x%x) was invalid", Address, Size);
        return OsError;
    }
    return RemoveSystemMemoryMapping(GetCurrentSystemMemorySpace(), Address, Size);
}

/* Queries information about a chunk of memory 
 * and returns allocation information or stats 
 * depending on query function */
OsStatus_t
ScMemoryQuery(
    _Out_ MemoryDescriptor_t *Descriptor)
{
    // Copy relevant data over
    Descriptor->AllocationGranularityBytes = GetMachine()->MemoryGranularity;
    Descriptor->PageSizeBytes   = GetSystemMemoryPageSize();
    Descriptor->PagesTotal      = GetMachine()->PhysicalMemory.BlockCount;
    Descriptor->PagesUsed       = GetMachine()->PhysicalMemory.BlocksAllocated;

    // Return no error, should never fail
    return OsSuccess;
}

/* ScMemoryAcquire
 * Acquires a given physical memory region described by a starting address
 * and region size. This is then mapped into the caller's address space and
 * is then accessible. The virtual address pointer is returned. */
OsStatus_t
ScMemoryAcquire(
    _In_  uintptr_t     PhysicalAddress,
    _In_  size_t        Size,
    _Out_ uintptr_t*    VirtualAddress)
{
    // Variables
    MCoreAsh_t *Ash = NULL;
    uintptr_t Shm;

    // Locate the current running process
    Ash = PhoenixGetCurrentAsh();
    if (Ash == NULL || PhysicalAddress == 0 || Size == 0) {
        return OsError;
    }

    // Start out by allocating memory 
    // in target process's shared memory space
    Shm = AllocateBlocksInBlockmap(Ash->Heap, __MASK, Size);
    if (Shm == 0) {
        return OsError;
    }
    *VirtualAddress = Shm + (PhysicalAddress & (GetSystemMemoryPageSize() - 1));
    return CreateSystemMemorySpaceMapping(GetCurrentSystemMemorySpace(), &PhysicalAddress, &Shm, 
        Size, MAPPING_USERSPACE | MAPPING_FIXED | MAPPING_VIRTUAL, __MASK);
}

/* ScMemoryRelease
 * Releases a previously acquired memory region and unmaps it from the caller's
 * address space. The virtual address pointer will no longer be accessible. */
OsStatus_t
ScMemoryRelease(
    _In_ uintptr_t VirtualAddress, 
    _In_ size_t Size)
{
    // Variables
    MCoreAsh_t *Ash = PhoenixGetCurrentAsh();
    uintptr_t PageMask = ~(GetSystemMemoryPageSize() - 1);
    size_t NumBlocks, i;

    // Assumptions:
    // VirtualAddress is page aligned
    // Size is page-aligned

    // Sanitize the running process
    if (Ash == NULL || VirtualAddress == 0 || Size == 0) {
        return OsError;
    }

    // Calculate the number of blocks
    NumBlocks = DIVUP(Size, GetSystemMemoryPageSize());

    // Sanity -> If we cross a page boundary
    if (((VirtualAddress + Size) & PageMask) != (VirtualAddress & PageMask)) {
        NumBlocks++;
    }

    // Iterate through allocated pages and free them
    for (i = 0; i < NumBlocks; i++) {
        RemoveSystemMemoryMapping(GetCurrentSystemMemorySpace(), 
            VirtualAddress + (i * GetSystemMemoryPageSize()), GetSystemMemoryPageSize());
    }

    // Free it in bitmap
    ReleaseBlockmapRegion(Ash->Heap, VirtualAddress, Size);
    return OsSuccess;
}

/* MemoryProtect
 * Changes the protection flags of a previous memory allocation
 * made by MemoryAllocate */
OsStatus_t
ScMemoryProtect(
    _In_  void*     MemoryPointer,
    _In_  size_t    Length,
    _In_  Flags_t   Flags,
    _Out_ Flags_t*  PreviousFlags)
{
    // Variables
    uintptr_t AddressStart = (uintptr_t)MemoryPointer;
    if (MemoryPointer == NULL || Length == 0) {
        return OsSuccess;
    }

    // We must force the application flag as it will remove
    // the user-accessibility if we allow it to change
    return ChangeSystemMemorySpaceProtection(GetCurrentSystemMemorySpace(), 
        AddressStart, Length, Flags | MAPPING_USERSPACE, PreviousFlags);
}

/*******************************************************************************
 * Path Functions
 *******************************************************************************/

/* ScGetWorkingDirectory
 * Queries the current working directory path for the current process (See _MAXPATH) */
OsStatus_t
ScGetWorkingDirectory(
    _In_ UUId_t     ProcessId,
    _In_ char*      PathBuffer,
    _In_ size_t     MaxLength)
{
    // Variables
    MCoreProcess_t *Process     = NULL;
    size_t BytesToCopy          = MaxLength;

    // Determine the process
    if (ProcessId == UUID_INVALID) {
        Process = PhoenixGetCurrentProcess();
    }
    else {
        Process = PhoenixGetProcess(ProcessId);
    }

    // Sanitize parameters
    if (Process == NULL || PathBuffer == NULL) {
        return OsError;
    }
    
    BytesToCopy = MIN(strlen(MStringRaw(Process->WorkingDirectory)), MaxLength);
    memcpy(PathBuffer, MStringRaw(Process->WorkingDirectory), BytesToCopy);
    return OsSuccess;
}

/* ScSetWorkingDirectory
 * Performs changes to the current working directory by canonicalizing the given 
 * path modifier or absolute path */
OsStatus_t
ScSetWorkingDirectory(
    _In_ const char *Path)
{
    // Variables
    MCoreProcess_t *Process = PhoenixGetCurrentProcess();
    MString_t *Translated = NULL;

    // Sanitize parameters
    if (Process == NULL || Path == NULL) {
        return OsError;
    }

    // Create a new string instead of modification
    Translated = MStringCreate((void*)Path, StrUTF8);
    MStringDestroy(Process->WorkingDirectory);
    Process->WorkingDirectory = Translated;
    return OsSuccess;
}

/* ScGetAssemblyDirectory
 * Queries the application path for the current process (See _MAXPATH) */
OsStatus_t
ScGetAssemblyDirectory(
    _In_ char*      PathBuffer,
    _In_ size_t     MaxLength)
{
    // Variables
    MCoreProcess_t *Process = PhoenixGetCurrentProcess();
    size_t BytesToCopy = MaxLength;

    // Sanitize parameters
    if (Process == NULL || PathBuffer == NULL) {
        return OsError;
    }
    if (strlen(MStringRaw(Process->BaseDirectory)) < MaxLength) {
        BytesToCopy = strlen(MStringRaw(Process->BaseDirectory));
    }
    memcpy(PathBuffer, MStringRaw(Process->BaseDirectory), BytesToCopy);
    return OsSuccess;
}

/* Parameter structure for creating file-mappings. 
 * Private structure, only used for parameter passing. */
struct FileMappingParameters {
    UUId_t    FileHandle;
    int       Flags;
    uint64_t  Offset;
    size_t    Size;
};

/* ScCreateFileMapping
 * Creates a new file-mapping that are bound to a specific file-descriptor. 
 * Accessing this mapping will be proxied to the specific file-access */
OsStatus_t
ScCreateFileMapping(
    _In_  struct FileMappingParameters* Parameters,
    _Out_ void**                        MemoryPointer)
{
    // Variables
    MCoreAshFileMapping_t *Mapping  = NULL;
    MCoreAsh_t *Ash                 = PhoenixGetCurrentAsh();
    uintptr_t BaseAddress           = 0;
    DataKey_t Key;

    // Sanity
    if (Ash == NULL || Parameters == NULL || Parameters->Size == 0) {
        return OsError;
    }

    // Start out by allocating memory
    // in target process's shared memory space
    BaseAddress = AllocateBlocksInBlockmap(Ash->Heap, __MASK, Parameters->Size + (Parameters->Offset % GetSystemMemoryPageSize()));
    if (BaseAddress == 0) {
        return OsError;
    }

    // Create a new mapping
    // We only read in block-sizes of page-size, this means the
    Mapping                 = (MCoreAshFileMapping_t*)kmalloc(sizeof(MCoreAshFileMapping_t));
    Mapping->FileHandle     = Parameters->FileHandle;
    Mapping->VirtualBase    = BaseAddress;
    Mapping->Flags          = (Flags_t)Parameters->Flags;
    Mapping->FileBlock      = (Parameters->Offset / GetSystemMemoryPageSize()) * GetSystemMemoryPageSize();
    Mapping->BlockOffset    = (Parameters->Offset % GetSystemMemoryPageSize());
    Mapping->Length         = Parameters->Size + Mapping->BlockOffset;
    Key.Value               = 0;
    CollectionAppend(Ash->FileMappings, CollectionCreateNode(Key, Mapping));

    // Update out
    *MemoryPointer          = (void*)(BaseAddress + Mapping->BlockOffset);
    return OsSuccess;
}

/* ScDestroyFileMapping
 * Destroys a previously created file-mapping using it's counterpart. */
OsStatus_t
ScDestroyFileMapping(
    _In_ void *MemoryPointer)
{
    // Variables
    MCoreAshFileMapping_t *Mapping  = NULL;
    CollectionItem_t *Node          = NULL;
    MCoreAsh_t *Ash                 = PhoenixGetCurrentAsh();

    // Buffers
    BufferObject_t TransferObject;
    LargeInteger_t Value;
    if (Ash == NULL) {
        return OsError;
    }

    // Iterate and find the node first
    _foreach(Node, Ash->FileMappings) {
        Mapping = (MCoreAshFileMapping_t*)Node->Data;
        if (ISINRANGE((uintptr_t)MemoryPointer, Mapping->VirtualBase, (Mapping->VirtualBase + Mapping->Length) - 1)) {
            break; // Continue to unmap process
        }
    }
    if (Node == NULL) {
        return OsError;
    }

    // Start the unmap process
    CollectionRemoveByNode(Ash->FileMappings, Node);
    CollectionDestroyNode(Ash->FileMappings, Node);
    
    // Unmap all mappings done
    for (uintptr_t ItrAddress = Mapping->VirtualBase; 
        ItrAddress < (Mapping->VirtualBase + Mapping->Length); 
        ItrAddress += GetSystemMemoryPageSize()) {
        
        // Check if page should be flushed to disk
        if (IsSystemMemoryPageDirty(GetCurrentSystemMemorySpace(), ItrAddress) == OsSuccess) {
            Value.QuadPart          = Mapping->FileBlock + (ItrAddress - Mapping->VirtualBase);
            
            // Fishy fishy @todo
            TransferObject.Virtual  = (const char*)ItrAddress;
            TransferObject.Physical = GetSystemMemoryMapping(GetCurrentSystemMemorySpace(), ItrAddress);
            TransferObject.Capacity = TransferObject.Length = GetSystemMemoryPageSize();
            TransferObject.Position = 0;
            if (SeekFile(Mapping->FileHandle, Value.u.LowPart, Value.u.HighPart) != FsOk ||
                WriteFile(Mapping->FileHandle, &TransferObject, NULL) != FsOk) {
                ERROR("Failed to flush file mapping, file most likely is closed or doesn't exist");
            }
        }

        // Unmap and cleanup
        if (GetSystemMemoryMapping(GetCurrentSystemMemorySpace(), ItrAddress) != 0) {
            RemoveSystemMemoryMapping(GetCurrentSystemMemorySpace(), ItrAddress, GetSystemMemoryPageSize());
        }
    }
    ReleaseBlockmapRegion(Ash->Heap, Mapping->VirtualBase, Mapping->Length);

    // Cleanup
    kfree(Mapping);
    return OsSuccess;
}

/***********************
* IPC Functions        *
***********************/

/* ScPipeOpen
 * Opens a new pipe for the calling Ash process and allows 
 * communication to this port from other processes */
OsStatus_t
ScPipeOpen(
    _In_ int            Port, 
    _In_ int            Type) {
    return PhoenixOpenAshPipe(PhoenixGetCurrentAsh(), Port, Type);
}

/* ScPipeClose
 * Closes an existing pipe on a given port and
 * shutdowns any communication on that port */
OsStatus_t
ScPipeClose(
    _In_ int            Port) {
    return PhoenixCloseAshPipe(PhoenixGetCurrentAsh(), Port);
}

/* ScPipeRead
 * Reads the requested number of bytes from the system-pipe. */
OsStatus_t
ScPipeRead(
    _In_ int            Port,
    _In_ uint8_t*       Container,
    _In_ size_t         Length)
{
    // Variables
    SystemPipe_t *Pipe  = NULL;

    // Sanitize parameters
    if (Length == 0) {
        return OsSuccess;
    }

    Pipe = PhoenixGetAshPipe(PhoenixGetCurrentAsh(), Port);
    if (Pipe == NULL) {
        ERROR("Trying to read from non-existing pipe %i", Port);
        return OsError;
    }

    ReadSystemPipe(Pipe, Container, Length);
    return OsSuccess;
}

/* ScPipeWrite
 * Writes the requested number of bytes to the system-pipe. */
OsStatus_t
ScPipeWrite(
    _In_ UUId_t         ProcessId,
    _In_ int            Port,
    _In_ uint8_t*       Message,
    _In_ size_t         Length)
{
    // Variables
    SystemPipe_t *Pipe      = NULL;

    // Sanitize parameters
    if (Message == NULL || Length == 0) {
        return OsError;
    }

    // Are we looking for a system out pipe? (std) then the
    // process id (target) will be set as invalid
    if (ProcessId == UUID_INVALID) {
        if (Port == PIPE_STDOUT || Port == PIPE_STDERR) {
            if (Port == PIPE_STDOUT) {
                Pipe = LogPipeStdout();
            }
            else if (Port == PIPE_STDERR) {
                Pipe = LogPipeStderr();
            }
        }
        else {
            ERROR("Invalid system pipe %i", Port);
            return OsError;
        }
    }
    else { Pipe = PhoenixGetAshPipe(PhoenixGetAsh(ProcessId), Port); }
    if (Pipe == NULL) {
        ERROR("Invalid pipe %i", Port);
        return OsError;
    }

    WriteSystemPipe(Pipe, Message, Length);
    return OsSuccess;
}

/* ScPipeReceive
 * Receives the requested number of bytes to the system-pipe. */
OsStatus_t
ScPipeReceive(
    _In_ UUId_t         ProcessId,
    _In_ int            Port,
    _In_ uint8_t*       Message,
    _In_ size_t         Length)
{
    // Variables
    SystemPipe_t *Pipe      = NULL;

    // Sanitize parameters
    if (Message == NULL || Length == 0 || ProcessId == UUID_INVALID) {
        ERROR("Invalid paramters for pipe-receive");
        return OsError;
    }

    Pipe = PhoenixGetAshPipe(PhoenixGetAsh(ProcessId), Port);
    if (Pipe == NULL) {
        ERROR("Invalid pipe %i", Port);
        return OsError;
    }

    ReadSystemPipe(Pipe, Message, Length);
    return OsSuccess;
}

/* ScRpcResponse
 * Waits for IPC RPC request to finish 
 * by polling the default pipe for a rpc-response */
OsStatus_t
ScRpcResponse(
    _In_ MRemoteCall_t* RemoteCall)
{
    // Variables
    SystemPipe_t *Pipe  = ThreadingGetCurrentThread(CpuGetCurrentId())->Pipe;
    assert(Pipe != NULL);
    assert(RemoteCall != NULL);
    assert(RemoteCall->Result.Length > 0);

    // Read up to <Length> bytes, this results in the next 1 .. Length
    // being read from the raw-pipe.
    ReadSystemPipe(Pipe, (uint8_t*)RemoteCall->Result.Data.Buffer, 
        RemoteCall->Result.Length);
    return OsSuccess;
}

/* ScRpcExecute
 * Executes an IPC RPC request to the given process and optionally waits for
 * a reply/response */
OsStatus_t
ScRpcExecute(
    _In_ MRemoteCall_t* RemoteCall,
    _In_ int            Async)
{
    // Variables
    SystemPipeUserState_t State;
    MCoreThread_t *Thread;
    SystemPipe_t *Pipe;
    MCoreAsh_t *Ash;
    size_t TotalLength  = sizeof(MRemoteCall_t);
    int i               = 0;

    // Trace
    TRACE("%s: ScRpcExecute(Target 0x%x, Message %i, Async %i)", 
        MStringRaw(PhoenixGetCurrentAsh()->Name), 
        RemoteCall->To.Process, RemoteCall->Function, Async);
    
    // Start out by resolving both the process and pipe
    Ash     = PhoenixGetAsh(RemoteCall->To.Process);
    Pipe    = PhoenixGetAshPipe(Ash, RemoteCall->To.Port);

    // Sanitize the lookups
    if (Ash == NULL || Pipe == NULL) {
        if (Ash == NULL) {
            ERROR("Target 0x%x did not exist", RemoteCall->To.Process);
        }
        else {
            ERROR("Port %u did not exist in target 0x%x",
                RemoteCall->To.Port, RemoteCall->To.Process);
        }
        return OsError;
    }

    // Install Sender
    Thread = ThreadingGetCurrentThread(CpuGetCurrentId());
    RemoteCall->From.Process    = Thread->AshId;
    RemoteCall->From.Thread     = Thread->Id;
    RemoteCall->From.Port       = -1;

    // Calculate how much data to be comitted
    for (i = 0; i < IPC_MAX_ARGUMENTS; i++) {
        if (RemoteCall->Arguments[i].Type == ARGUMENT_BUFFER) {
            TotalLength += RemoteCall->Arguments[i].Length;
        }
    }

    // Setup producer access
    AcquireSystemPipeProduction(Pipe, TotalLength, &State);
    WriteSystemPipeProduction(&State, (const uint8_t*)RemoteCall, sizeof(MRemoteCall_t));
    for (i = 0; i < IPC_MAX_ARGUMENTS; i++) {
        if (RemoteCall->Arguments[i].Type == ARGUMENT_BUFFER) {
            WriteSystemPipeProduction(&State, 
                (const uint8_t*)RemoteCall->Arguments[i].Data.Buffer,
                RemoteCall->Arguments[i].Length);
        }
    }

    // Async request? Because if yes, don't
    // wait for response
    if (Async) {
        return OsSuccess;
    }
    return ScRpcResponse(RemoteCall);
}

/* ScRpcListen
 * Listens for a new rpc-message on the default rpc-pipe. */
OsStatus_t
ScRpcListen(
    _In_ int            Port,
    _In_ MRemoteCall_t* RemoteCall,
    _In_ uint8_t*       ArgumentBuffer)
{
    // Variables
    SystemPipeUserState_t State;
    uint8_t *BufferPointer = ArgumentBuffer;
    SystemPipe_t *Pipe;
    MCoreAsh_t *Ash;
    size_t Length;
    int i;

    // Trace
    TRACE("%s: ScRpcListen(Port %i)", MStringRaw(PhoenixGetCurrentAsh()->Name), Port);
    
    // Start out by resolving both the
    // process and pipe
    Ash     = PhoenixGetCurrentAsh();
    Pipe    = PhoenixGetAshPipe(Ash, Port);

    // Start consuming
    AcquireSystemPipeConsumption(Pipe, &Length, &State);
    ReadSystemPipeConsumption(&State, (uint8_t*)RemoteCall, sizeof(MRemoteCall_t));
    for (i = 0; i < IPC_MAX_ARGUMENTS; i++) {
        if (RemoteCall->Arguments[i].Type == ARGUMENT_BUFFER) {
            RemoteCall->Arguments[i].Data.Buffer = (const void*)BufferPointer;
            ReadSystemPipeConsumption(&State, BufferPointer, RemoteCall->Arguments[i].Length);
            BufferPointer += RemoteCall->Arguments[i].Length;
        }
    }
    FinalizeSystemPipeConsumption(Pipe, &State);
    return OsSuccess;
}

/* ScRpcRespond
 * A wrapper for sending an RPC response to the calling thread. Each thread has each it's response
 * channel to avoid any concurrency issues. */
OsStatus_t
ScRpcRespond(
    _In_ MRemoteCallAddress_t*  RemoteAddress,
    _In_ const uint8_t*         Buffer, 
    _In_ size_t                 Length)
{
    // Variables
    MCoreThread_t *Thread   = ThreadingGetThread(RemoteAddress->Thread);
    SystemPipe_t *Pipe      = NULL;

    // Sanitize thread still exists
    if (Thread != NULL) {
        Pipe = Thread->Pipe;
    }
    if (Pipe == NULL) {
        ERROR("Thread %u did not exist", RemoteAddress->Thread);
        return OsError;
    }

    WriteSystemPipe(Pipe, Buffer, Length);
    return OsSuccess;
}

/***********************
 * Driver Functions    *
 ***********************/
#include <acpiinterface.h>
#include <os/io.h>
#include <os/device.h>
#include <os/buffer.h>
#include <modules/modules.h>
#include <process/server.h>

/* ScAcpiQueryStatus
 * Queries basic acpi information and returns either OsSuccess
 * or OsError if Acpi is not supported on the running platform */
OsStatus_t
ScAcpiQueryStatus(
    AcpiDescriptor_t *AcpiDescriptor)
{
    /* Sanitize the parameters */
    if (AcpiDescriptor == NULL) {
        return OsError;
    }

    /* Sanitize the acpi-status in this system */
    if (AcpiAvailable() == ACPI_NOT_AVAILABLE) {
        return OsError;
    }
    else {
        /* Copy information over to descriptor */
        AcpiDescriptor->Century = AcpiGbl_FADT.Century;
        AcpiDescriptor->BootFlags = AcpiGbl_FADT.BootFlags;
        AcpiDescriptor->ArmBootFlags = AcpiGbl_FADT.ArmBootFlags;
        AcpiDescriptor->Version = ACPI_VERSION_6_0;

        /* Wuhu! */
        return OsSuccess;
    }
}

/* ScAcpiQueryTableHeader
 * Queries the table header of the table that matches
 * the given signature, if none is found OsError is returned */
OsStatus_t
ScAcpiQueryTableHeader(
    const char *Signature,
    ACPI_TABLE_HEADER *Header)
{
    /* Use a temporary buffer as we don't
     * really know the length */
    ACPI_TABLE_HEADER *PointerToHeader = NULL;

    /* Sanitize that ACPI is enabled on this
     * system before we query */
    if (AcpiAvailable() == ACPI_NOT_AVAILABLE) {
        return OsError;
    }

    /* Now query for the header */
    if (ACPI_FAILURE(AcpiGetTable((ACPI_STRING)Signature, 0, &PointerToHeader))) {
        return OsError;
    }

    /* Wuhuu, the requested table exists, copy the
     * header information over */
    memcpy(Header, PointerToHeader, sizeof(ACPI_TABLE_HEADER));
    return OsSuccess;
}

/* ScAcpiQueryTable
 * Queries the full table information of the table that matches
 * the given signature, if none is found OsError is returned */
OsStatus_t
ScAcpiQueryTable(
    const char *Signature,
    ACPI_TABLE_HEADER *Table)
{
    /* Use a temporary buffer as we don't
     * really know the length */
    ACPI_TABLE_HEADER *Header = NULL;

    /* Sanitize that ACPI is enabled on this
     * system before we query */
    if (AcpiAvailable() == ACPI_NOT_AVAILABLE) {
        return OsError;
    }

    /* Now query for the full table */
    if (ACPI_FAILURE(AcpiGetTable((ACPI_STRING)Signature, 0, &Header))) {
        return OsError;
    }

    /* Wuhuu, the requested table exists, copy the
     * table information over */
    memcpy(Header, Table, Header->Length);
    return OsSuccess;
}

/* ScAcpiQueryInterrupt 
 * Queries the interrupt-line for the given bus, device and
 * pin combination. The pin must be zero indexed. Conform flags
 * are returned in the <AcpiConform> */
OsStatus_t
ScAcpiQueryInterrupt(
    DevInfo_t   Bus,
    DevInfo_t   Device,
    int         Pin, 
    int*        Interrupt,
    Flags_t*    AcpiConform)
{
    // Redirect the call to the interrupt system
    *Interrupt = AcpiDeriveInterrupt(Bus, Device, Pin, AcpiConform);
    return (*Interrupt == INTERRUPT_NONE) ? OsError : OsSuccess;
}
 
/* ScIoSpaceRegister
 * Creates and registers a new IoSpace with our
 * architecture sub-layer, it must support io-spaces 
 * or atleast dummy-implementation */
OsStatus_t ScIoSpaceRegister(DeviceIoSpace_t *IoSpace)
{
    /* Sanitize params */
    if (IoSpace == NULL) {
        return OsError;
    }

    /* Validate process permissions */

    /* Now we can try to actually register the
     * io-space, if it fails it exists already */
    return IoSpaceRegister(IoSpace);
}

/* ScIoSpaceAcquire
 * Tries to claim a given io-space, only one driver
 * can claim a single io-space at a time, to avoid
 * two drivers using the same device */
OsStatus_t ScIoSpaceAcquire(DeviceIoSpace_t *IoSpace)
{
    /* Sanitize params */
    if (IoSpace == NULL) {
        return OsError;
    }

    /* Validate process permissions */

    /* Now lets try to acquire the IoSpace */
    return IoSpaceAcquire(IoSpace);
}

/* ScIoSpaceRelease
 * Tries to release a given io-space, only one driver
 * can claim a single io-space at a time, to avoid
 * two drivers using the same device */
OsStatus_t ScIoSpaceRelease(DeviceIoSpace_t *IoSpace)
{
    /* Now lets try to release the IoSpace 
     * Don't bother with validation */
    return IoSpaceRelease(IoSpace);
}

/* ScIoSpaceDestroy
 * Destroys the io-space with the given id and removes
 * it from the io-manage in the operation system, it
 * can only be removed if its not already acquired */
OsStatus_t ScIoSpaceDestroy(UUId_t IoSpace)
{
    /* Sanitize params */

    /* Validate process permissions */

    /* Destroy the io-space, it might
     * not be possible, if we don't own it */
    return IoSpaceDestroy(IoSpace);
}

/* Allows a server to register an alias for its 
 * process id, as applications can't possibly know
 * its id if it changes */
OsStatus_t
ScRegisterAliasId(
    _In_ UUId_t Alias)
{
    // Debug
    TRACE("ScRegisterAliasId(Server %s, Alias 0x%X)",
        MStringRaw(PhoenixGetCurrentAsh()->Name), Alias);

    // Redirect call to phoenix
    return PhoenixRegisterAlias(
        PhoenixGetCurrentAsh(), Alias);
}

/* ScLoadDriver
 * Attempts to resolve the best possible drive for
 * the given device information */
OsStatus_t
ScLoadDriver(
    _In_ MCoreDevice_t *Device,
    _In_ size_t         Length)
{
    // Variables
    MCorePhoenixRequest_t *Request  = NULL;
    MCoreServer_t *Server           = NULL;
    MCoreModule_t *Module           = NULL;
    MString_t *Path                 = NULL;
    MRemoteCall_t RemoteCall        = { { 0 }, { 0 }, 0 };

    // Trace
    TRACE("ScLoadDriver(Vid 0x%x, Pid 0x%x, Class 0x%x, Subclass 0x%x)",
        Device->VendorId, Device->DeviceId,
        Device->Class, Device->Subclass);

    // Sanitize parameters, length must not be less than base
    if (Device == NULL || Length < sizeof(MCoreDevice_t)) {
        return OsError;
    }

    // First of all, if a server has already been spawned
    // for the specific driver, then call it's RegisterInstance
    Server = PhoenixGetServerByDriver(
        Device->VendorId, Device->DeviceId,
        Device->Class, Device->Subclass);

    // Sanitize the lookup 
    // If it's not found, spawn server
    if (Server == NULL) {
        // Look for matching driver first, then generic
        Module = ModulesFindSpecific(Device->VendorId, Device->DeviceId);
        if (Module == NULL) {
            Module = ModulesFindGeneric(Device->Class, Device->Subclass);
        }
        if (Module == NULL) {
            return OsError;
        }

        // Build ramdisk path for module/server
        Path = MStringCreate("rd:/", StrUTF8);
        MStringAppendString(Path, Module->Name);

        // Create the request 
        Request = (MCorePhoenixRequest_t*)kmalloc(sizeof(MCorePhoenixRequest_t));
        memset(Request, 0, sizeof(MCorePhoenixRequest_t));
        Request->Base.Type = AshSpawnServer;
        Request->Path = Path;

        // Initiate request
        PhoenixCreateRequest(Request);
        PhoenixWaitRequest(Request, 0);

        // Sanitize startup
        Server = PhoenixGetServer(Request->AshId);
        assert(Server != NULL);

        // Cleanup
        MStringDestroy(Request->Path);
        kfree(Request);

        // Update the server params for next load
        Server->VendorId = Device->VendorId;
        Server->DeviceId = Device->DeviceId;
        Server->DeviceClass = Device->Class;
        Server->DeviceSubClass = Device->Subclass;
    }

    // Initialize the base of a new message, always protocol version 1
    RPCInitialize(&RemoteCall, Server->Base.Id, 1, PIPE_REMOTECALL, __DRIVER_REGISTERINSTANCE);
    RPCSetArgument(&RemoteCall, 0, Device, Length);

    // Make sure the server has opened it's comm-pipe
    PhoenixWaitAshPipe(&Server->Base, PIPE_REMOTECALL);
    return ScRpcExecute(&RemoteCall, 1);
}

/* ScRegisterInterrupt 
 * Allocates the given interrupt source for use by
 * the requesting driver, an id for the interrupt source
 * is returned. After a succesful register, OnInterrupt
 * can be called by the event-system */
UUId_t
ScRegisterInterrupt(
    _In_ MCoreInterrupt_t *Interrupt,
    _In_ Flags_t Flags)
{
    /* Sanitize parameters */
    if (Interrupt == NULL
        || (Flags & (INTERRUPT_KERNEL | INTERRUPT_SOFT))) {
        return UUID_INVALID;
    }

    /* Just redirect the call */
    return InterruptRegister(Interrupt, Flags);
}

/* ScUnregisterInterrupt 
 * Unallocates the given interrupt source and disables
 * all events of OnInterrupt */
OsStatus_t
ScUnregisterInterrupt(
    _In_ UUId_t Source)
{
    return InterruptUnregister(Source);
}

/* ScTimersStart
 * Creates a new standard timer for the requesting process. 
 * When interval elapses a __TIMEOUT event is generated for
 * the owner of the timer. */
UUId_t
ScTimersStart(
    _In_ size_t Interval,
    _In_ int Periodic,
    _In_ __CONST void *Data)
{
    return TimersStart(Interval, Periodic, Data);
}

/* ScTimersStop
 * Destroys a existing standard timer, owner must be the requesting
 * process. Otherwise access fault. */
OsStatus_t
ScTimersStop(
    _In_ UUId_t TimerId) {
    return TimersStop(TimerId);
}

/*******************************************************************************
 * System Functions
 *******************************************************************************/
OsStatus_t ScEndBootSequence(void) {
    TRACE("Ending console session");
    LogSetRenderMode(0);
    return OsSuccess;
}

/* ScFlushHardwareCache
 * Flushes the specified hardware cache. Should be used with caution as it might
 * result in performance drops. */
OsStatus_t ScFlushHardwareCache(
    _In_     int    Cache,
    _In_Opt_ void*  Start, 
    _In_Opt_ size_t Length) {
    if (Cache == CACHE_INSTRUCTION) {
        CpuFlushInstructionCache(Start, Length);
        return OsSuccess;
    }
    return OsError;
}

/* System (Environment) Query 
 * This function allows the user to query 
 * information about cpu, memory, stats etc */
int ScEnvironmentQuery(void) {
    return 0;
}

/* ScSystemTime
 * Retrieves the system time. This is only ticking
 * if a system clock has been initialized. */
OsStatus_t
ScSystemTime(
    _Out_ struct tm *SystemTime)
{
    // Sanitize input
    if (SystemTime == NULL) {
        return OsError;
    }
    return TimersGetSystemTime(SystemTime);
}

/* ScSystemTick
 * Retrieves the system tick counter. This is only ticking
 * if a system timer has been initialized. */
OsStatus_t
ScSystemTick(
    _Out_ clock_t *SystemTick)
{
    // Sanitize input
    if (SystemTick == NULL) {
        return OsError;
    }
    return TimersGetSystemTick(SystemTick);
}

/* ScPerformanceFrequency
 * Returns how often the performance timer fires every
 * second, the value will never be 0 */
OsStatus_t
ScPerformanceFrequency(
    _Out_ LargeInteger_t *Frequency)
{
    // Sanitize input
    if (Frequency == NULL) {
        return OsError;
    }
    return TimersQueryPerformanceFrequency(Frequency);
}

/* ScPerformanceTick
 * Retrieves the system performance tick counter. This is only ticking
 * if a system performance timer has been initialized. */
OsStatus_t
ScPerformanceTick(
    _Out_ LargeInteger_t *Value)
{
    // Sanitize input
    if (Value == NULL) {
        return OsError;
    }
    return TimersQueryPerformanceTick(Value);
}

/* ScQueryDisplayInformation
 * Queries information about the active display */
OsStatus_t
ScQueryDisplayInformation(
    _In_ VideoDescriptor_t *Descriptor) {
    if (Descriptor == NULL) {
        return OsError;
    }
    return VideoQuery(Descriptor);
}

/* ScCreateDisplayFramebuffer
 * Right now it simply identity maps the screen display framebuffer
 * into the current process's memory mappings and returns a pointer to it. */
void*
ScCreateDisplayFramebuffer(void) {
    uintptr_t FbPhysical    = VideoGetTerminal()->FrameBufferAddressPhysical;
    uintptr_t FbVirtual     = 0;
    size_t FbSize           = VideoGetTerminal()->Info.BytesPerScanline * VideoGetTerminal()->Info.Height;

    // Sanitize
    if (PhoenixGetCurrentAsh() == NULL) {
        return NULL;
    }

    // Allocate the neccessary size
    FbVirtual = AllocateBlocksInBlockmap(PhoenixGetCurrentAsh()->Heap, __MASK, FbSize);
    if (FbVirtual == 0) {
        return NULL;
    }

    if (CreateSystemMemorySpaceMapping(GetCurrentSystemMemorySpace(), &FbPhysical, &FbVirtual,
        FbSize, MAPPING_USERSPACE | MAPPING_NOCACHE | MAPPING_FIXED | MAPPING_VIRTUAL, __MASK) != OsSuccess) {
        // What? @todo
        ERROR("Failed to map the display buffer");
    }
    return (void*)FbVirtual;
}

/* NoOperation
 * Empty operation, mostly because the operation is reserved */
OsStatus_t
NoOperation(void) {
    return OsSuccess;
}

/* Syscall Table */
uintptr_t GlbSyscallTable[111] = {
    DefineSyscall(ScSystemDebug),

    /* Process & Threading
     * - Starting index is 1 */
    DefineSyscall(ScProcessExit),
    DefineSyscall(ScProcessGetCurrentId),
    DefineSyscall(ScProcessSpawn),
    DefineSyscall(ScProcessJoin),
    DefineSyscall(ScProcessKill),
    DefineSyscall(ScProcessSignal),
    DefineSyscall(ScProcessRaise),
    DefineSyscall(ScProcessGetCurrentName),
    DefineSyscall(ScProcessGetModuleHandles),
    DefineSyscall(ScProcessGetModuleEntryPoints),
    DefineSyscall(ScProcessGetStartupInformation),
    DefineSyscall(ScSharedObjectLoad),
    DefineSyscall(ScSharedObjectGetFunction),
    DefineSyscall(ScSharedObjectUnload),
    DefineSyscall(ScThreadCreate),
    DefineSyscall(ScThreadExit),
    DefineSyscall(ScThreadSignal),
    DefineSyscall(ScThreadJoin),
    DefineSyscall(ScThreadDetach),
    DefineSyscall(ScThreadSleep),
    DefineSyscall(ScThreadYield),
    DefineSyscall(ScThreadGetCurrentId),
    DefineSyscall(ScThreadSetCurrentName),
    DefineSyscall(ScThreadGetCurrentName),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),

    /* Synchronization Functions - 31 */
    DefineSyscall(ScConditionCreate),
    DefineSyscall(ScConditionDestroy),
    DefineSyscall(ScWaitForObject),
    DefineSyscall(ScSignalHandle),
    DefineSyscall(ScSignalHandleAll),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),

    /* Memory Functions - 41 */
    DefineSyscall(ScMemoryAllocate),
    DefineSyscall(ScMemoryFree),
    DefineSyscall(ScMemoryQuery),
    DefineSyscall(ScMemoryAcquire),
    DefineSyscall(ScMemoryRelease),
    DefineSyscall(ScMemoryProtect),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),

    /* Operating System Support Functions - 51 */
    DefineSyscall(ScGetWorkingDirectory),
    DefineSyscall(ScSetWorkingDirectory),
    DefineSyscall(ScGetAssemblyDirectory),
    DefineSyscall(ScCreateFileMapping),
    DefineSyscall(ScDestroyFileMapping),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),

    /* IPC Functions - 61 */
    DefineSyscall(ScPipeOpen),
    DefineSyscall(ScPipeClose),
    DefineSyscall(ScPipeRead),
    DefineSyscall(ScPipeWrite),
    DefineSyscall(ScPipeReceive),
    DefineSyscall(NoOperation),
    DefineSyscall(ScRpcExecute),
    DefineSyscall(ScRpcResponse),
    DefineSyscall(ScRpcListen),
    DefineSyscall(ScRpcRespond),

    /* System Functions - 71 */
    DefineSyscall(ScEndBootSequence),
    DefineSyscall(ScFlushHardwareCache),
    DefineSyscall(ScEnvironmentQuery),
    DefineSyscall(ScSystemTick),
    DefineSyscall(ScPerformanceFrequency),
    DefineSyscall(ScPerformanceTick),
    DefineSyscall(ScSystemTime),
    DefineSyscall(ScQueryDisplayInformation),
    DefineSyscall(ScCreateDisplayFramebuffer),
    DefineSyscall(NoOperation),

    /* Driver Functions - 81 
     * - ACPI Support */
    DefineSyscall(ScAcpiQueryStatus),
    DefineSyscall(ScAcpiQueryTableHeader),
    DefineSyscall(ScAcpiQueryTable),
    DefineSyscall(ScAcpiQueryInterrupt),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),

    /* Driver Functions - 91 
     * - I/O Support */
    DefineSyscall(ScIoSpaceRegister),
    DefineSyscall(ScIoSpaceAcquire),
    DefineSyscall(ScIoSpaceRelease),
    DefineSyscall(ScIoSpaceDestroy),

    /* Driver Functions - 95
     * - Support */
    DefineSyscall(ScRegisterAliasId),
    DefineSyscall(ScLoadDriver),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),

    /* Driver Functions - 101
     * - Interrupt Support */
    DefineSyscall(ScRegisterInterrupt),
    DefineSyscall(ScUnregisterInterrupt),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(ScTimersStart),
    DefineSyscall(ScTimersStop),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation),
    DefineSyscall(NoOperation)
};
