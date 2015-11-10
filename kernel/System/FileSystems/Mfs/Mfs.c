/* MollenOS
*
* Copyright 2011 - 2014, Philip Meulengracht
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
* MollenOS MCore - MollenOS File System
*/

/* Includes */
#include <FileSystems/Mfs.h>
#include <MString.h>
#include <Heap.h>
#include <stdio.h>
#include <string.h>

/* Read Sectors Wrapper */
int MfsReadSectors(MCoreFileSystem_t *Fs, uint64_t Sector, void *Buffer, uint32_t Count)
{
	/* Keep */
	int Result = 0;

	/* Lock Disk */
	MutexLock(Fs->Disk->Lock);

	/* Do the read */
	Result = Fs->Disk->Read(Fs->Disk->DiskData,
		Fs->SectorStart + Sector, Buffer, (Count * Fs->Disk->SectorSize));

	/* Unlock disk */
	MutexUnlock(Fs->Disk->Lock);

	/* Done! */
	return Result;
}

/* Write Sectors Wrapper */
int MfsWriteSectors(MCoreFileSystem_t *Fs, uint64_t Sector, void *Buffer, uint32_t Count)
{
	/* Keep */
	int Result = 0;

	/* Lock Disk */
	MutexLock(Fs->Disk->Lock);

	/* Do the read */
	Result = Fs->Disk->Write(Fs->Disk->DiskData,
		Fs->SectorStart + Sector, Buffer, (Count * Fs->Disk->SectorSize));

	/* Unlock disk */
	MutexUnlock(Fs->Disk->Lock);

	/* Done! */
	return Result;
}

/* Get next bucket in chain 
 * Todo: Have this in memory */
/* Locate next bucket */
uint32_t MfsGetNextBucket(MCoreFileSystem_t *Fs, uint32_t Bucket)
{
	/* Vars */
	MfsData_t *mData = (MfsData_t*)Fs->FsData;

	/* Calculate Index */
	uint32_t SectorOffset = Bucket / (uint32_t)mData->BucketsPerSector;
	uint32_t SectorIndex = Bucket % (uint32_t)mData->BucketsPerSector;

	/* Read sector */
	if (mData->BucketBufferOffset != SectorOffset)
	{
		/* Read */
		if (MfsReadSectors(Fs, mData->BucketMapSector + SectorOffset, mData->BucketBuffer, 1) < 0)
		{
			/* Error */
			printf("MFS_GETNEXTBUCKET: Error reading from disk\n");
			return 0xFFFFFFFF;
		}

		/* Update */
		mData->BucketBufferOffset = SectorOffset;
	}
	
	/* Pointer to array */
	uint8_t *BufPtr = (uint8_t*)mData->BucketBuffer;

	/* Done */
	return BufPtr[SectorIndex] | (BufPtr[SectorIndex + 1] << 8) 
		| (BufPtr[SectorIndex + 2] << 16) | (BufPtr[SectorIndex + 3] << 24);
}

/* Locate Node */
MfsFile_t *MfsLocateEntry(MCoreFileSystem_t *Fs, uint32_t DirBucket, MString_t *Path)
{
	/* Vars */
	MfsData_t *mData = (MfsData_t*)Fs->FsData;
	uint32_t CurrentBucket = DirBucket;
	int IsEnd = 0;
	uint32_t i;

	/* Get token */
	int IsEndOfPath = 0;
	MString_t *Token = NULL;
	int StrIndex = MStringFind(Path, (uint32_t)'/');
	if (StrIndex == -1
		|| StrIndex == (int)(MStringLength(Path) - 1))
	{
		/* Set end, and token as rest of path */
		IsEndOfPath = 1;
		Token = Path;
	}
	else
		Token = MStringSubString(Path, 0, StrIndex);

	/* Allocate buffer for data */
	void *EntryBuffer = kmalloc(mData->BucketSize * Fs->Disk->SectorSize);
	 
	/* Let's iterate */
	while (!IsEnd)
	{
		/* Load bucket */
		if (MfsReadSectors(Fs, mData->BucketSize * CurrentBucket, EntryBuffer, mData->BucketSize) < 0)
		{
			/* Error */
			printf("MFS_LOCATEENTRY: Error reading from disk\n");
			break;
		}

		/* Iterate buffer */
		MfsTableEntry_t *Entry = (MfsTableEntry_t*)EntryBuffer;
		for (i = 0; i < (mData->BucketSize / 2); i++)
		{
			/* Sanity, end of table */
			if (Entry->Status == 0x0)
			{
				IsEnd = 1;
				break;
			}

			/* Load UTF8 */
			MString_t *NodeName = MStringCreate(Entry->Name, StrUTF8);

			/* If we find a match, and we are at end 
			 * we are done. Otherwise, go deeper */
			if (MStringCompare(Token, NodeName, 1))
			{
				/* Match */
				if (!IsEndOfPath)
				{
					/* This should be a directory */
					if (!(Entry->Flags & MFS_DIRECTORY))
					{
						/* Cleanup */
						kfree(EntryBuffer);
						MStringDestroy(NodeName);
						MStringDestroy(Token);

						/* Path not found ofc */
						MfsFile_t *RetData = (MfsFile_t*)kmalloc(sizeof(MfsFile_t));
						RetData->Status = VfsPathIsNotDirectory;
						return RetData;
					}

					/* Sanity the bucket beforehand */
					if (Entry->StartBucket == MFS_END_OF_CHAIN)
					{
						/* Cleanup */
						kfree(EntryBuffer);
						MStringDestroy(NodeName);
						MStringDestroy(Token);

						/* Path not found ofc */
						MfsFile_t *RetData = (MfsFile_t*)kmalloc(sizeof(MfsFile_t));
						RetData->Status = VfsPathNotFound;
						return RetData;
					}

					/* Create a new sub-string with rest */
					MString_t *RestOfPath = 
						MStringSubString(Path, StrIndex + 1, 
						(MStringLength(Path) - (StrIndex + 1)));

					/* Go deeper */
					MfsFile_t *Ret = MfsLocateEntry(Fs, Entry->StartBucket, RestOfPath);

					/* Cleanup */
					kfree(EntryBuffer);
					MStringDestroy(RestOfPath);
					MStringDestroy(NodeName);
					MStringDestroy(Token);

					/* Done */
					return Ret;
				}
				else
				{
					/* Yay, proxy data, cleanup, done! */
					MfsFile_t *Ret = (MfsFile_t*)kmalloc(sizeof(MfsFile_t));

					/* Proxy */
					Ret->Name = NodeName;
					Ret->Flags = Entry->Flags;
					Ret->Size = Entry->Size;
					Ret->AllocatedSize = Entry->AllocatedSize;
					Ret->DataBucket = Entry->StartBucket;
					Ret->Status = VfsOk;

					/* Save position */
					Ret->DirBucket = CurrentBucket;
					Ret->DirOffset = i;

					/* Cleanup */
					kfree(EntryBuffer);
					MStringDestroy(Token);

					/* Done */
					return Ret;
				}
			}

			/* Cleanup */
			MStringDestroy(NodeName);

			/* Go on to next */
			Entry++;
		}

		/* Get next bucket */
		if (!IsEnd)
		{
			CurrentBucket = MfsGetNextBucket(Fs, CurrentBucket);

			if (CurrentBucket == MFS_END_OF_CHAIN)
				IsEnd = 1;
		}
	}

	/* Cleanup */
	kfree(EntryBuffer);
	MStringDestroy(Token);
	
	/* If IsEnd is set, we couldn't find it 
	 * If IsEnd is not set, we should not be here... */
	MfsFile_t *RetData = (MfsFile_t*)kmalloc(sizeof(MfsFile_t));
	RetData->Status = VfsPathNotFound;
	return RetData;
}

/* Open File */
VfsErrorCode_t MfsOpenFile(void *FsData, 
	MCoreFile_t *Handle, MString_t *Path, VfsFileFlags_t Flags)
{
	/* Cast */
	MCoreFileSystem_t *Fs = (MCoreFileSystem_t*)FsData;
	MfsData_t *mData = (MfsData_t*)Fs->FsData;
	VfsErrorCode_t RetCode = VfsOk;

	/* This will be a recursive parse of path */
	MfsFile_t *FileInfo = MfsLocateEntry(Fs, mData->RootIndex, Path);

	/* Validate */
	if (FileInfo->Status != VfsOk)
	{
		/* Cleanup */
		RetCode = FileInfo->Status;
		kfree(FileInfo);
		return RetCode;
	}

	/* Fill out Handle */
	Handle->Data = FileInfo;
	Handle->Flags = Flags;
	Handle->Position = 0;
	Handle->Size = FileInfo->Size;
	Handle->Name = FileInfo->Name;

	/* Done */
	return RetCode;
}

/* Close File 
 * frees resources allocated */
VfsErrorCode_t MfsCloseFile(void *FsData, MCoreFile_t *Handle)
{
	/* Not used */
	_CRT_UNUSED(FsData);

	/* Cast */
	MfsFile_t *FileInfo = (MfsFile_t*)Handle->Data;
	VfsErrorCode_t RetCode = VfsOk;

	/* Sanity */
	if (Handle->Data == NULL)
		return RetCode;

	/* Cleanup */
	kfree(FileInfo);

	/* Done */
	return RetCode;
}

/* Read File */
VfsErrorCode_t MfsReadFile(void *FsData, MCoreFile_t *Handle, void *Buffer, uint32_t Size)
{
	
}

/* Write File */
VfsErrorCode_t MfsWriteFile(void *FsData, MCoreFile_t *Handle, void *Buffer, uint32_t Size)
{

}

/* Delete File */
VfsErrorCode_t MfsDeleteFile(void *FsData, MCoreFile_t *Handle)
{
	
}

/* Query information */
VfsErrorCode_t MfsQuery(void *FsData, MCoreFile_t *Handle)
{

}

/* Unload MFS Driver 
 * If it's forced, we can't save
 * stuff back to the disk :/ */
OsResult_t MfsDestroy(void *FsData, uint32_t Forced)
{
	/* Cast */
	MCoreFileSystem_t *Fs = (MCoreFileSystem_t*)FsData;
	MfsData_t *mData = (MfsData_t*)Fs->FsData;

	/* Sanity */
	if (!Forced)
	{

	}

	/* Free resources */
	kfree(mData->VolumeLabel);
	kfree(mData);

	/* Done */
	return OsOk;
}

/* Load Mfs Driver */
OsResult_t MfsInit(MCoreFileSystem_t *Fs)
{
	/* Allocate structures */
	void *TmpBuffer = (void*)kmalloc(Fs->Disk->SectorSize);
	MfsBootRecord_t *BootRecord = NULL;

	/* Read bootsector */
	if (MfsReadSectors(Fs, 0, TmpBuffer, 1) < 0)
	{
		/* Error */
		printf("MFS_INIT: Error reading from disk\n");
		kfree(TmpBuffer);
		return OsFail;
	}

	/* Cast */
	BootRecord = (MfsBootRecord_t*)TmpBuffer;
	
	/* Validate Magic */
	if (BootRecord->Magic != MFS_MAGIC)
	{
		printf("MFS_INIT: Invalid Magic 0x%x\n", BootRecord->Magic);
		kfree(TmpBuffer);
		return OsFail;
	}

	/* Validate Version */
	if (BootRecord->Version != 0x1)
	{
		printf("MFS_INIT: Invalid Version\n");
		kfree(TmpBuffer);
		return OsFail;
	}

	/* Allocate */
	MfsData_t *mData = (MfsData_t*)kmalloc(sizeof(MfsData_t));

	/* Save some of the data */
	mData->MbSector = BootRecord->MasterBucketSector;
	mData->MbMirrorSector = BootRecord->MasterBucketMirror;
	mData->Version = (uint32_t)BootRecord->Version;
	mData->BucketSize = (uint32_t)BootRecord->SectorsPerBucket;
	mData->Flags = (uint32_t)BootRecord->Flags;

	/* Calculate the bucket-map sector */
	mData->BucketCount = Fs->SectorCount / mData->BucketSize;
	mData->BucketMapSize = mData->BucketCount * 4; /* One bucket descriptor is 4 bytes */
	mData->BucketMapSector = (Fs->SectorCount - ((mData->BucketMapSize / Fs->Disk->SectorSize) + 1));
	mData->BucketsPerSector = Fs->Disk->SectorSize / 4;

	/* Copy the volume label over */
	mData->VolumeLabel = (char*)kmalloc(8 + 1);
	memset(mData->VolumeLabel, 0, 9);
	memcpy(mData->VolumeLabel, BootRecord->BootLabel, 8);

	/* Read the MB */
	if (MfsReadSectors(Fs, mData->MbSector, TmpBuffer, 1) < 0)
	{
		/* Error */
		printf("MFS_INIT: Error reading MB from disk\n");
		kfree(TmpBuffer);
		kfree(mData->VolumeLabel);
		kfree(mData);
		return OsFail;
	}

	/* Validate MB */
	MfsMasterBucket_t *Mb = (MfsMasterBucket_t*)TmpBuffer;

	/* Sanity */
	if (Mb->Magic != MFS_MAGIC)
	{
		printf("MFS_INIT: Invalid MB-Magic 0x%x\n", Mb->Magic);
		kfree(TmpBuffer);
		kfree(mData->VolumeLabel);
		kfree(mData);
		return OsFail;
	}

	/* Parse */
	mData->RootIndex = Mb->RootIndex;
	mData->FreeIndex = Mb->FreeBucket;

	/* Setup buffer */
	mData->BucketBuffer = kmalloc(Fs->Disk->SectorSize);
	mData->BucketBufferOffset = 0xFFFFFFFF;

	/* Setup Fs */
	Fs->FsData = mData;

	/* Setup functions */
	Fs->Destory = MfsDestroy;
	Fs->OpenFile = MfsOpenFile;
	Fs->CloseFile = MfsCloseFile;
	Fs->ReadFile = MfsReadFile;
	Fs->WriteFile = MfsWriteFile;
	Fs->DeleteFile = MfsDeleteFile;
	Fs->Query = MfsQuery;

	/* Done, cleanup */
	kfree(TmpBuffer);
	return OsOk;
}