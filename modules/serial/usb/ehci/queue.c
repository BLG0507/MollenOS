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
 * MollenOS MCore - Enhanced Host Controller Interface Driver
 * TODO:
 * - Power Management
 * - Isochronous Transport
 * - Transaction Translator Support
 */

/* Includes
 * - System */
#include <os/mollenos.h>
#include "ehci.h"

/* Includes
 * - Library */
#include <assert.h>
#include <string.h>

/* Globals 
 * Error messages for codes that might appear in transfers */
const char *EhciErrorMessages[] = {
	"No Error",
	"Ping State/PERR",
	"Split Transaction State",
	"Missed Micro-Frame",
	"Transaction Error (CRC, Timeout)",
	"Babble Detected",
	"Data Buffer Error",
	"Halted, Stall",
	"Active"
};

/* Disable the warning about conditional
 * expressions being constant, they are intentional */
#ifdef _MSC_VER
#pragma warning(disable:4127)
#endif

/* EhciQueueInitialize
 * Initialize the controller's queue resources and resets counters */
OsStatus_t
EhciQueueInitialize(
	_In_ EhciController_t *Controller)
{
	// Variables
	EhciControl_t *Queue = NULL;
	uintptr_t RequiredSpace = 0, PoolPhysical = 0;
	void *Pool = NULL;
	int i;

	// Trace
	TRACE("EhciQueueInitialize()");

	// Shorthand the queue controller
	Queue = &Controller->QueueControl;

	// Null out queue-control
	memset(Queue, 0, sizeof(EhciControl_t));

	// The first thing we want to do is 
	// to determine the size of the frame list, if we can control it ourself
	// we set it to the shortest available (not 32 tho)
	if (Controller->CParameters & EHCI_CPARAM_VARIABLEFRAMELIST) {
		Queue->FrameLength = 256;
	}
 	else {
		Queue->FrameLength = 1024;
	}

	// Allocate a virtual list for keeping track of added
	// queue-heads in virtual space first.
	Queue->VirtualList = (reg32_t*)malloc(Queue->FrameLength * sizeof(reg32_t));

	// Add up all the size we are going to need for pools and
	// the actual frame list
	RequiredSpace += Queue->FrameLength * sizeof(reg32_t);        // Framelist
	RequiredSpace += sizeof(EhciQueueHead_t) * EHCI_POOL_NUM_QH;  // Qh-pool
	RequiredSpace += sizeof(EhciTransferDescriptor_t) * EHCI_POOL_NUM_TD; // Td-pool

	// Perform the allocation
	if (MemoryAllocate(RequiredSpace, MEMORY_CLEAN | MEMORY_COMMIT
		| MEMORY_LOWFIRST | MEMORY_CONTIGIOUS, &Pool, &PoolPhysical) != OsSuccess) {
		ERROR("Failed to allocate memory for resource-pool");
		return OsError;
	}

	// Store the physical address for the frame
	Queue->FrameList = (reg32_t*)Pool;
	Queue->FrameListPhysical = PoolPhysical;

	// Initialize addresses for pools
	Queue->QHPool = (EhciQueueHead_t*)
		((uint8_t*)Pool + (Queue->FrameLength * sizeof(reg32_t)));
	Queue->QHPoolPhysical = PoolPhysical + (Queue->FrameLength * sizeof(reg32_t));
	Queue->TDPool = (EhciTransferDescriptor_t*)
		((uint8_t*)Queue->QHPool + (sizeof(EhciQueueHead_t) * EHCI_POOL_NUM_QH));
	Queue->TDPoolPhysical = Queue->QHPoolPhysical + (sizeof(EhciQueueHead_t) * EHCI_POOL_NUM_QH);

	// Initialize frame lists
	for (i = 0; i < Queue->FrameLength; i++) {
		Queue->VirtualList[i] = Queue->FrameList[i] = EHCI_LINK_END;
	}

	// Initialize the QH pool
	for (i = 0; i < EHCI_POOL_NUM_QH; i++) {
		Queue->QHPool[i].Index = i;
		Queue->QHPool[i].LinkIndex = EHCI_NO_INDEX;
	}

	// Initialize the TD pool
	for (i = 0; i < EHCI_POOL_NUM_TD; i++) {
		Queue->TDPool[i].Index = i;
		Queue->TDPool[i].LinkIndex = EHCI_NO_INDEX;
		Queue->TDPool[i].AlternativeLinkIndex = EHCI_NO_INDEX;
	}

	// Initialize the dummy (null) queue-head that we use for end-link
	Queue->QHPool[EHCI_POOL_QH_NULL].Overlay.NextTD = EHCI_LINK_END;
	Queue->QHPool[EHCI_POOL_QH_NULL].Overlay.NextAlternativeTD = EHCI_LINK_END;
	Queue->QHPool[EHCI_POOL_QH_NULL].LinkPointer = EHCI_LINK_END;
	Queue->QHPool[EHCI_POOL_QH_NULL].HcdFlags = EHCI_QH_ALLOCATED;

	// Initialize the dummy (async) transfer-descriptor that we use for queuing
	Queue->TDPool[EHCI_POOL_TD_ASYNC].Status = EHCI_TD_HALTED;
	Queue->TDPool[EHCI_POOL_TD_ASYNC].Link = EHCI_LINK_END;
    Queue->TDPool[EHCI_POOL_TD_ASYNC].AlternativeLink = EHCI_LINK_END;
    Queue->TDPool[EHCI_POOL_TD_ASYNC].AlternativeLinkIndex = EHCI_NO_INDEX;

	// Initialize the dummy (async) queue-head that we use for end-link
	Queue->QHPool[EHCI_POOL_QH_ASYNC].LinkPointer = 
		(EHCI_POOL_QHINDEX(Controller, EHCI_POOL_QH_ASYNC)) | EHCI_LINK_QH;
	Queue->QHPool[EHCI_POOL_QH_ASYNC].LinkIndex = EHCI_POOL_QH_ASYNC;

	Queue->QHPool[EHCI_POOL_QH_ASYNC].Flags = EHCI_QH_RECLAMATIONHEAD;
	Queue->QHPool[EHCI_POOL_QH_ASYNC].Overlay.Status = EHCI_TD_HALTED;
	Queue->QHPool[EHCI_POOL_QH_ASYNC].Overlay.NextTD = EHCI_LINK_END;
	Queue->QHPool[EHCI_POOL_QH_ASYNC].Overlay.NextAlternativeTD =
		EHCI_POOL_TDINDEX(Controller, EHCI_POOL_TD_ASYNC);
	Queue->QHPool[EHCI_POOL_QH_ASYNC].HcdFlags = EHCI_QH_ALLOCATED;

	// Allocate the transaction list
	Queue->TransactionList = ListCreate(KeyInteger, LIST_SAFE);

	// Initialize a bandwidth scheduler
	Controller->Scheduler = UsbSchedulerInitialize(
		Queue->FrameLength, EHCI_MAX_BANDWIDTH, 8);

	// Update the hardware registers to point to the newly allocated
	// addresses
	Controller->OpRegisters->PeriodicListAddress = 
		(reg32_t)Queue->FrameListPhysical;
	Controller->OpRegisters->AsyncListAddress = 
		(reg32_t)EHCI_POOL_QHINDEX(Controller, EHCI_POOL_QH_ASYNC) | EHCI_LINK_QH;
}

/* EhciQueueDestroy
 * Unschedules any scheduled ed's and frees all resources allocated
 * by the initialize function */
OsStatus_t
EhciQueueDestroy(
	_In_ EhciController_t *Controller)
{

}

/* EhciConditionCodeToIndex
 * Converts a given condition bit-index to number */
int
EhciConditionCodeToIndex(
	_In_ unsigned ConditionCode)
{
    // Variables
	unsigned Cc = ConditionCode;
	int bCount = 0;

	// Shift untill we reach 0, count number of shifts
	for (; Cc != 0;) {
		bCount++;
		Cc >>= 1;
	}
	return bCount;
}

/* EhciEnableAsyncScheduler
 * Enables the async scheduler if it is not enabled already */
void
EhciEnableAsyncScheduler(
    _In_ EhciController_t *Controller)
{
	// Variables
	reg32_t Temp = 0;

	// Sanitize the current status
	if (Controller->OpRegisters->UsbStatus & EHCI_STATUS_ASYNC_ACTIVE) {
		return;
    }

	// Fire the enable command
	Temp = Controller->OpRegisters->UsbCommand;
	Temp |= EHCI_COMMAND_ASYNC_ENABLE;
	Controller->OpRegisters->UsbCommand = Temp;
}

/* EhciDisableAsyncScheduler
 * Disables the async sheduler if it is not disabled already */
void
EhciDisableAsyncScheduler(
    _In_ EhciController_t *Controller)
{
    // Variables
    reg32_t Temp = 0;

	// Sanitize its current status
	if (!(Controller->OpRegisters->UsbStatus & EHCI_STATUS_ASYNC_ACTIVE)) {
		return;
    }

	// Fire off disable command
	Temp = Controller->OpRegisters->UsbCommand;
	Temp &= ~(EHCI_COMMAND_ASYNC_ENABLE);
	Controller->OpRegisters->UsbCommand = Temp;
}

/* EhciRingDoorbell
 * This functions rings the bell */
void
EhciRingDoorbell(
    _In_ EhciController_t *Controller)
{
	// If the bell is already ringing, force a re-bell
	if (!Controller->QueueControl.BellIsRinging) {
		Controller->QueueControl.BellIsRinging = 1;
		Controller->OpRegisters->UsbCommand |= EHCI_COMMAND_IOC_ASYNC_DOORBELL;
	}
	else {
		Controller->QueueControl.BellReScan = 1;
    }
}

/* EhciNextGenericLink
 * Get's a pointer to the next virtual link, only Qh's have this implemented 
 * right now and will need modifications */
EhciGenericLink_t*
EhciNextGenericLink(
    EhciGenericLink_t *Link, 
    uintptr_t Type)
{
    // @todo
	switch (Type) {
        case EHCI_LINK_QH:
            return (EhciGenericLink_t*)&Link->Qh->LinkPointerVirtual;
        case EHCI_LINK_FSTN:
            return (EhciGenericLink_t*)&Link->FSTN->PathPointer;
        case EHCI_LINK_iTD:
            return (EhciGenericLink_t*)&Link->iTd->Link;
        default:
            return (EhciGenericLink_t*)&Link->siTd->Link;
	}
}

/* EhciLinkPeriodicQh
 * This function links an interrupt Qh into the schedule at Qh->sFrame 
 * and every other Qh->Interval */
void
EhciLinkPeriodicQh(
    _In_ EhciController_t *Controller, 
    _In_ EhciQueueHead_t *Qh)
{
    // Variables
	size_t Period = Qh->Interval;
	size_t i;

	// Sanity the period, it must be _atleast_ 1
    if (Period == 0) {
		Period = 1;
    }

	// Iterate the entire framelist and install the periodic qh
	for (i = Qh->sFrame; i < Controller->QueueControl.FrameList; i += Period) {
		// Retrieve a virtual pointer and a physical
        EhciGenericLink_t *VirtualLink =
            (EhciGenericLink_t*)&Controller->QueueControl.VirtualList[i];
		uintptr_t *PhysicalLink = &Controller->QueueControl.FrameList[i];
		EhciGenericLink_t This = *VirtualLink;
		uintptr_t Type = 0;

		// Iterate past isochronous tds
		while (This.Address) {
			Type = EHCI_LINK_TYPE(*PhysicalLink);
			if (Type == EHCI_LINK_QH) {
				break;
            }

            // Update iterators
			VirtualLink = EhciNextGenericLink(VirtualLink, Type);
			PhysicalLink = &This.Address;
			This = *VirtualLink;
		}

		// sorting each branch by period (slow-->fast)
		// enables sharing interior tree nodes
		while (This.Address && Qh != This.Qh) {
			if (Qh->Interval > This.Qh->Interval) {
				break;
            }

			// Update iterators
			VirtualLink = (EhciGenericLink_t*)&Controller->QueueControl.QHPool[This.Qh->LinkIndex];
			PhysicalLink = &This.Qh->LinkPointer;
			This = *VirtualLink;
		}

		// link in this qh, unless some earlier pass did that
		if (Qh != This.Qh) {
			Qh->LinkIndex = This.Qh->Index;
			if (This.Qh) {
				Qh->LinkPointer = *PhysicalLink;
            }

            // Flush memory writes
			MemoryBarrier();

			// Perform linking
			VirtualLink->Qh = Qh;
			*PhysicalLink = (EHCI_POOL_QHINDEX(Controller, Qh->Index) | EHCI_LINK_QH);
		}
	}
}

/* EhciUnlinkPeriodic
 * Generic unlink from periodic list needs a bit more information as it
 * is used for all formats */
void
EhciUnlinkPeriodic(
    _In_ EhciController_t *Controller, 
    _In_ uintptr_t Address, 
    _In_ size_t Period, 
    _In_ size_t sFrame)
{
	// Variables
	size_t i;

	// Sanity the period, it must be _atleast_ 1
    if (Period == 0) {
		Period = 1;
    }

	// We should mark Qh->Flags |= EHCI_QH_INVALIDATE_NEXT 
	// and wait for next frame
	for (i = sFrame; i < Controller->QueueControl.FrameList; i += Period) {
		// Retrieve a virtual pointer and a physical
        EhciGenericLink_t *VirtualLink =
            (EhciGenericLink_t*)&Controller->QueueControl.VirtualList[i];
        uintptr_t *PhysicalLink = &Controller->QueueControl.FrameList[i];
        EhciGenericLink_t This = *VirtualLink;
        uintptr_t Type = 0;

		// Find previous handle that points to our qh
		while (This.Address && This.Address != Address) {
			Type = EHCI_LINK_TYPE(*PhysicalLink);
			VirtualLink = EhciNextGenericLink(VirtualLink, Type);
			PhysicalLink = &This.Address;
			This = *VirtualLink;
		}

		// Sanitize end of list, it didn't exist
		if (!This.Address) {
			return;
        }

		// Perform the unlinking
		Type = EHCI_LINK_TYPE(*PhysicalLink);
		*VirtualLink = *EhciNextGenericLink(&This, Type);

		if (*(&This.Address) != EHCI_LINK_END) {
			*PhysicalLink = *(&This.Address);
        }
	}
}

/* EhciQhAllocate
 * This allocates a QH for a Control, Bulk and Interrupt 
 * transaction and should not be used for isoc */
EhciQueueHead_t*
EhciQhAllocate(
    _In_ EhciController_t *Controller, 
    _In_ UsbTransferType_t Type)
{
	// Variables
    EhciQueueHead_t *Qh = NULL;
    int i;

	// Acquire controller lock
    SpinlockAcquire(&Controller->Base.Lock);
    
    // Iterate the pool and find a free entry
    for (i = EHCI_POOL_QH_START; i < EHCI_POOL_NUM_QH; i++) {
        if (Controller->QueueControl.QHPool[i].HcdFlags & EHCI_QH_ALLOCATED) {
            continue;
        }

        // Set initial state
        Controller->QueueControl.QHPool[i].Overlay.Status = EHCI_TD_HALTED;
        Controller->QueueControl.QHPool[i].HcdFlags = EHCI_QH_ALLOCATED;
        Qh = &Controller->QueueControl.QHPool[i];
        break;
    }

    // Did we find anything? 
    if (Qh == NULL) {
        ERROR("EhciQhAllocate::(RAN OUT OF QH's)");
    }

	// Release controller lock
	SpinlockRelease(&Controller->Base.Lock);
	return Qh;
}

/* EhciQhInitialize
 * This initiates any periodic scheduling information 
 * that might be needed */
void
EhciQhInitialize(
    _In_ EhciController_t *Controller, 
    _In_ EhciQueueHead_t *Qh,
    _In_ UsbSpeed_t Speed,
    _In_ int Direction,
    _In_ UsbTransferType_t Type,
    _In_ size_t EndpointInterval,
    _In_ size_t EndpointMaxPacketSize,
    _In_ size_t TransferLength)
{
    // Variables
    int TransactionsPerFrame = DIVUP(TransferLength, EndpointMaxPacketSize);

	// Calculate the neccessary bandwidth
	Qh->Bandwidth = (reg32_t)
        NS_TO_US(UsbCalculateBandwidth(Speed, 
            Direction, Type, TransferLength));

    // Calculate the frame period
    // If highspeed/fullspeed or Isoc calculate period as 2^(Interval-1)
	if (Speed == HighSpeed
		|| (Speed == FullSpeed && Type == IsochronousTransfer)) {
		Qh->Interval = (1 << EndpointInterval);
	}
	else {
		Qh->Interval = EndpointInterval;
    }

	/* Validate Bandwidth */
    if (UsbSchedulerValidate(Controller->Scheduler, 
        Qh->Interval, Qh->Bandwidth, TransactionsPerFrame)) {
		TRACE("EHCI::Couldn't allocate space in scheduler for params %u:%u", 
			Qh->Interval, Qh->Bandwidth);
    }
}

/* EhciTdAllocate
 * This allocates a QTD (TD) for Control, Bulk and Interrupt */
EhciTransferDescriptor_t*
EhciTdAllocate(
    _In_ EhciController_t *Controller)
{
	// Variables
	EhciTransferDescriptor_t *Td = NULL;
	int i;

	// Acquire controller lock
    SpinlockAcquire(&Controller->Base.Lock);
    
    // Iterate the pool and find a free entry
    for (i = 0; i < EHCI_POOL_TD_ASYNC; i++) {
        if (Controller->QueueControl.TDPool[i].HcdFlags & EHCI_TD_ALLOCATED) {
            continue;
        }

        // Perform allocation
        Controller->QueueControl.TDPool[i].HcdFlags = EHCI_TD_ALLOCATED;
        Td = &Controller->QueueControl.TDPool[i];
        break;
	}

    // Sanitize end of list, no allocations?
    if (Td == NULL) {
        ERROR("EhciTdAllocate::Ran out of TD's");
    }

	// Release controller lock
	SpinlockRelease(&Controller->Base.Lock);
	return Td;
}

/* EhciTdFill
 * This sets up a QTD (TD) buffer structure and makes 
 * sure it's split correctly out on all the pages */
size_t
EhciTdFill(
    _In_ EhciTransferDescriptor_t *Td, 
    _In_ uintptr_t BufferAddress, 
    _In_ size_t Length)
{
	// Variables
	size_t LengthRemaining = Length;
	size_t Count = 0;
	int i;

	// Sanitize parameters
	if (Length == 0 || BufferAddress == 0) {
		return 0;
    }

	// Iterate buffers
	for (i = 0; LengthRemaining > 0 && i < 5; i++) {
		uintptr_t Physical = BufferAddress + (i * 0x1000);
        
        // Update buffer
        Td->Buffers[i] = EHCI_TD_BUFFER(Physical);
		if (sizeof(uintptr_t) > 4) {
			Td->ExtBuffers[i] = EHCI_TD_EXTBUFFER(Physical);
        }
		else {
			Td->ExtBuffers[i] = 0;
        }

		// Update iterators
		Count += MIN(0x1000, LengthRemaining);
		LengthRemaining -= MIN(0x1000, LengthRemaining);
    }

    // Return how many bytes were "buffered"
	return Count;
}

/* EhciTdSetup
 * This allocates & initializes a TD for a setup transaction 
 * this is only used for control transactions */
EhciTransferDescriptor_t*
EhciTdSetup(
    _In_ EhciController_t *Controller, 
	_In_ UsbTransaction_t *Transaction)
{
	// Variables
	EhciTransferDescriptor_t *Td;

	// Allocate the transfer-descriptor
	Td = EhciTdAllocate(Controller);

	// Initialize the transfer-descriptor
	Td->Link = EHCI_LINK_END;
    Td->AlternativeLink = EHCI_LINK_END;
    Td->AlternativeLinkIndex = EHCI_NO_INDEX;
	Td->Status = EHCI_TD_ACTIVE;
	Td->Token = EHCI_TD_SETUP;
	Td->Token |= EHCI_TD_ERRCOUNT;

	// Calculate the length of the setup transfer
    Td->Length = (uint16_t)(EHCI_TD_LENGTH(EhciTdFill(Td, 
        Transaction->BufferAddress, sizeof(UsbPacket_t))));

	// Return the allocated descriptor
	return Td;
}

/* EhciTdIo
 * This allocates & initializes a TD for an i/o transaction 
 * and is used for control, bulk and interrupt */
EhciTransferDescriptor_t*
EhciTdIo(
    _In_ EhciController_t *Controller,
    _In_ UsbTransfer_t *Transfer,
    _In_ UsbTransaction_t *Transaction,
	_In_ uint32_t PId,
	_In_ int Toggle)
{
	// Variables
	EhciTransferDescriptor_t *Td = NULL;

	// Allocate a new td
	Td = EhciTdAllocate(Controller);

	// Initialize the new Td
	Td->Link = EHCI_LINK_END;
    Td->AlternativeLink = EHCI_LINK_END;
    Td->AlternativeLinkIndex = EHCI_NO_INDEX;
	Td->Status = EHCI_TD_ACTIVE;
	Td->Token = (uint8_t)(PId & 0x3);
	Td->Token |= EHCI_TD_ERRCOUNT;
    
    // Short packet not ok? 
	if (Transfer->Flags & USB_TRANSFER_SHORT_NOT_OK && PId == EHCI_TD_IN) {
		Td->AlternativeLink = EHCI_POOL_TDINDEX(Controller, EHCI_POOL_TD_ASYNC);
        Td->AlternativeLinkIndex = EHCI_POOL_TD_ASYNC;
    }

	// Calculate the length of the transfer
    Td->Length = (uint16_t)(EHCI_TD_LENGTH(EhciTdFill(Td, 
        Transaction->BufferAddress, Transaction->Length)));

	// Set toggle?
	if (Toggle) {
		Td->Length |= EHCI_TD_TOGGLE;
    }

	// Calculate next toggle 
    // if transaction spans multiple transfers
    // @todo
	if (Transaction->Length > 0
		&& !(DIVUP(Transaction->Length, Transfer->Endpoint.MaxPacketSize) % 2)) {
        Toggle ^= 0x1;
    }

	// Setup done, return the new descriptor
	return Td;
}

/* Restarts an interrupt QH 
 * by resetting it to it's start state */
void
EhciRestartQh(
    EhciController_t *Controller, 
    UsbManagerTransfer_t *Transfer)
{
	/* Get transaction list */
	UsbHcTransaction_t *tList = (UsbHcTransaction_t*)Request->Transactions;
	EhciQueueHead_t *Qh = (EhciQueueHead_t*)Request->Data;
	EhciTransferDescriptor_t *Td = NULL, *TdCopy = NULL;

	/* For now */
	_CRT_UNUSED(Controller);

	/* Iterate and reset 
	 * Switch toggles if necessary ? */
	while (tList)
	{
		/* Cast TD(s) */
		TdCopy = (EhciTransferDescriptor_t*)tList->TransferDescriptorCopy;
		Td = (EhciTransferDescriptor_t*)tList->TransferDescriptor;

		/* Let's see */
		if (tList->Length != 0
			&& Td->Token & EHCI_TD_IN)
			memcpy(tList->Buffer, tList->TransferBuffer, tList->Length);

		/* Update Toggles */
		TdCopy->Length &= ~(EHCI_TD_TOGGLE);
		if (Td->Length & EHCI_TD_TOGGLE)
			TdCopy->Length |= EHCI_TD_TOGGLE;

		/* Reset */
		memcpy(Td, TdCopy, sizeof(EhciTransferDescriptor_t));

		/* Get next link */
		tList = tList->Link;
	}

	/* Set Qh to point to first */
	Td = (EhciTransferDescriptor_t*)Request->Transactions->TransferDescriptor;

	/* Zero out overlay (BUT KEEP TOGGLE???) */
	memset(&Qh->Overlay, 0, sizeof(EhciQueueHeadOverlay_t));

	/* Set pointers accordingly */
	Qh->Overlay.NextTD = Td->PhysicalAddress;
	Qh->Overlay.NextAlternativeTD = EHCI_LINK_END;
}

/* Scans a QH for completion or error 
 * returns non-zero if it has been touched */
int
EhciScanQh(
    EhciController_t *Controller, 
    UsbManagerTransfer_t *Transfer)
{
	/* Get transaction list */
	UsbHcTransaction_t *tList = (UsbHcTransaction_t*)Request->Transactions;

	/* State variables */
	int ShortTransfer = 0;
	int ErrorTransfer = 0;
	int Counter = 0;
	int ProcessQh = 0;

	/* For now... */
	_CRT_UNUSED(Controller);

	/* Loop through transactions */
	while (tList)
	{
		/* Increament */
		Counter++;

		/* Cast Td */
		EhciTransferDescriptor_t *Td =
			(EhciTransferDescriptor_t*)tList->TransferDescriptor;

		/* Get code */
		int CondCode = EhciConditionCodeToIndex(Request->Speed == HighSpeed ? Td->Status & 0xFC : Td->Status);
		int BytesLeft = Td->Length & 0x7FFF;

		/* Sanity first */
		if (Td->Status & EHCI_TD_ACTIVE) {

			/* If it's not the first TD, skip */
			if (Counter > 1
				&& (ShortTransfer || ErrorTransfer))
				ProcessQh = (ErrorTransfer == 1) ? 2 : 1;
			else
				ProcessQh = 0; /* No, only partially done without any errs */

			/* Break */
			break;
		}

		/* Set for processing per default */
		ProcessQh = 1;

		/* TD is not active
		* this means it's been processed */
		if (BytesLeft > 0) {
			ShortTransfer = 1;
		}

		/* Error Transfer ? */
		if (CondCode != 0) {
			ErrorTransfer = 1;
		}

		/* Get next transaction */
		tList = tList->Link;
	}

	/* Sanity */
	if (ErrorTransfer)
		ProcessQh = 2;

	/* Done */
	return ProcessQh;
}

/* EhciProcessTransfers
 * For transaction progress this involves done/error transfers */
void
EhciProcessTransfers(
	_In_ EhciController_t *Controller)
{
	/* Transaction is completed / Failed */
	List_t *Transactions = (List_t*)Controller->TransactionList;

	/* Get transactions in progress and find the offender */
	foreach(Node, Transactions)
	{
		/* Cast UsbRequest */
		UsbHcRequest_t *HcRequest = (UsbHcRequest_t*)Node->Data;
		int Processed = 0;

		/* Scan */
		if (HcRequest->Type != IsochronousTransfer)
			Processed = EhciScanQh(Controller, HcRequest);
		else
			;

		/* If it is to be processed, wake or process */
		if (Processed) {
			if (HcRequest->Type == InterruptTransfer) 
			{
				/* Restart the Qh */
				EhciRestartQh(Controller, HcRequest);

				/* Access Callback */
				if (HcRequest->Callback != NULL)
					HcRequest->Callback->Callback(HcRequest->Callback->Args,
					Processed == 2 ? TransferStalled : TransferFinished);

				/* Renew data in out transfers */
				UsbHcTransaction_t *tList = (UsbHcTransaction_t*)HcRequest->Transactions;

				/* Iterate and reset */
				while (tList)
				{
					/* Cast TD(s) */
					EhciTransferDescriptor_t *Td = 
						(EhciTransferDescriptor_t*)tList->TransferDescriptor;

					/* Let's see */
					if (tList->Length != 0
						&& Td->Token & EHCI_TD_OUT)
						memcpy(tList->TransferBuffer, tList->Buffer, tList->Length);

					/* Get next link */
					tList = tList->Link;
				}
			}
			else
				SchedulerWakeupOneThread((Addr_t*)HcRequest->Data);
		}
	}
}

/* EhciProcessDoorBell
 * This makes sure to schedule and/or unschedule transfers */
void
EhciProcessDoorBell(
	_In_ EhciController_t *Controller)
{
    // Variables
	ListNode_t *Node = NULL;

Scan:
    // As soon as we enter the scan area we reset the re-scan
    // to allow other threads to set it again
	Controller->QueueControl.BellReScan = 0;

	/* Iterate transactions */
	_foreach(Node, Controller->QueueControl.TransactionList) {
		// Instantiate a transaction pointer
        UsbManagerTransfer_t *Transfer = 
            (UsbManagerTransfer_t*)Node->Data;

		/* Get transaction type */
		if (Request->Type == ControlTransfer
			|| Request->Type == BulkTransfer) {
			EhciQueueHead_t *Qh = (EhciQueueHead_t*)Request->Data;

			/* Has it asked to be unscheduled? */
			if (Qh->HcdFlags & EHCI_QH_UNSCHEDULE) {
				Qh->HcdFlags &= ~(EHCI_QH_UNSCHEDULE);
				SchedulerWakeupOneThread((Addr_t*)Qh);
			}
		}
	}

	// If someone has rung the bell while 
	// the door was opened, we should not close the door yet
	if (Controller->QueueControl.BellReScan != 0) {
		goto Scan;
    }

	// Bell is no longer ringing
	Controller->QueueControl.BellIsRinging = 0;
}

/* Re-enable warnings */
#ifdef _MSC_VER
#pragma warning(default:4127)
#endif
