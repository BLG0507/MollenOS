/*
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
* MollenOS CLib - Exit Function (Exit normally with CRT Cleanup)
*/

/* Includes */
#include <os/Syscall.h>
#include <stddef.h>
#include <stdlib.h>

#ifndef LIBC_KERNEL

/* Terminate */
void exit(int status)
{
	/* Cleanup Crt */

	/* Clean us up */
	Syscall1(MOLLENOS_SYSCALL_TERMINATE, MOLLENOS_SYSCALL_PARAM(status));

	/* Yield */
	Syscall0(MOLLENOS_SYSCALL_YIELD);

	/* Forever */
	for (;;);
}

#endif