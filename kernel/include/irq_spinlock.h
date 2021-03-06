/**
 * MollenOS
 *
 * Copyright 2019, Philip Meulengracht
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
 * Synchronization (Irq Spinlocks)
 * - Spinlock implementation that also disables interrupts.
 */

#ifndef __VALI_IRQ_SPINLOCK_H__
#define __VALI_IRQ_SPINLOCK_H__

#include <os/spinlock.h>

typedef struct IrqSpinlock {
    spinlock_t  SyncObject;
    IntStatus_t Flags;
} IrqSpinlock_t;

#define OS_IRQ_SPINLOCK_INIT { _SPN_INITIALIZER_NP(spinlock_plain), 0 }

KERNELAPI void KERNELABI
IrqSpinlockConstruct(
    _In_ IrqSpinlock_t* Spinlock);

KERNELAPI void KERNELABI
IrqSpinlockAcquire(
    _In_ IrqSpinlock_t* Spinlock);

KERNELAPI void KERNELABI
IrqSpinlockRelease(
    _In_ IrqSpinlock_t* Spinlock);

#endif //!__VALI_IRQ_SPINLOCK_H__
