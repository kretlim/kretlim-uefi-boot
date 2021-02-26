#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include "DevicePathUtils.h"
#include "Except.h"

BOOLEAN InsideDevicePath(EFI_DEVICE_PATH* All, EFI_DEVICE_PATH* One) {
    // iterate this one
    EFI_DEVICE_PATH* Path;
    for (
            Path = One;
            DevicePathNodeLength(Path) == DevicePathNodeLength(All) &&
            !IsDevicePathEndType(Path) &&
            CompareMem(Path, All, DevicePathNodeLength(All)) == 0;
            Path = NextDevicePathNode(Path), All = NextDevicePathNode(All)
            ) {
    }

    // return true if we reached the end of the one device path
    // that we were looking for
    return IsDevicePathEndType(Path);
}

EFI_DEVICE_PATH* LastDevicePathNode(EFI_DEVICE_PATH* Dp) {
    if (Dp == NULL) {
        return NULL;
    }

    EFI_DEVICE_PATH* LastDp = NULL;
    for ( ; !IsDevicePathEndType(Dp); Dp = NextDevicePathNode(Dp)) {
        LastDp = Dp;
    }

    return LastDp;
}

EFI_DEVICE_PATH* RemoveLastDevicePathNode(EFI_DEVICE_PATH* Dp) {
    if (Dp == NULL) {
        return NULL;
    }

    // get the last node and calculate the new node size
    EFI_DEVICE_PATH* LastNode = LastDevicePathNode(Dp);
    UINTN Len = (UINTN)LastNode - (UINTN)Dp;

    // allocate the new node with another one for the path end
    EFI_DEVICE_PATH* NewNode = AllocatePool(Len + sizeof(EFI_DEVICE_PATH));

    // copy it
    CopyMem(NewNode, Dp, Len);

    // set the last one to be empty
    LastNode = (EFI_DEVICE_PATH*)((UINTN)NewNode + Len);
    SetDevicePathEndNode(LastNode);

    return NewNode;
}

EFI_STATUS EFIAPI OpenFileByDevicePath(EFI_DEVICE_PATH_PROTOCOL** FilePath, EFI_FILE_PROTOCOL** File, UINT64 OpenMode, UINT64 Attributes) {
    EFI_STATUS                      Status;
    EFI_HANDLE                      FileSystemHandle;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    EFI_FILE_PROTOCOL               *LastFile;
    FILEPATH_DEVICE_PATH            *FilePathNode;
    CHAR16                          *AlignedPathName;
    CHAR16                          *PathName;
    EFI_FILE_PROTOCOL               *NextFile;

    if (File == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    *File = NULL;

    if (FilePath == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    //
    // Look up the filesystem.
    //
    Status = gBS->LocateDevicePath (
            &gEfiSimpleFileSystemProtocolGuid,
            FilePath,
            &FileSystemHandle
    );
    if (EFI_ERROR (Status)) {
        return Status;
    }
    Status = gBS->OpenProtocol (
            FileSystemHandle,
            &gEfiSimpleFileSystemProtocolGuid,
            (VOID **)&FileSystem,
            gImageHandle,
            NULL,
            EFI_OPEN_PROTOCOL_GET_PROTOCOL
    );
    if (EFI_ERROR (Status)) {
        return Status;
    }

    //
    // Open the root directory of the filesystem. After this operation succeeds,
    // we have to release LastFile on error.
    //
    Status = FileSystem->OpenVolume (FileSystem, &LastFile);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    //
    // Traverse the device path nodes relative to the filesystem.
    //
    while (!IsDevicePathEnd (*FilePath)) {
        if (DevicePathType (*FilePath) != MEDIA_DEVICE_PATH ||
            DevicePathSubType (*FilePath) != MEDIA_FILEPATH_DP) {
            Status = EFI_INVALID_PARAMETER;
            goto CloseLastFile;
        }
        FilePathNode = (FILEPATH_DEVICE_PATH *)*FilePath;

        //
        // FilePathNode->PathName may be unaligned, and the UEFI specification
        // requires pointers that are passed to protocol member functions to be
        // aligned. Create an aligned copy of the pathname if necessary.
        //
        if ((UINTN)FilePathNode->PathName % sizeof *FilePathNode->PathName == 0) {
            AlignedPathName = NULL;
            PathName = FilePathNode->PathName;
        } else {
            AlignedPathName = AllocateCopyPool (
                    (DevicePathNodeLength (FilePathNode) -
                     SIZE_OF_FILEPATH_DEVICE_PATH),
                    FilePathNode->PathName
            );
            if (AlignedPathName == NULL) {
                Status = EFI_OUT_OF_RESOURCES;
                goto CloseLastFile;
            }
            PathName = AlignedPathName;
        }

        //
        // Open or create the file corresponding to the next pathname fragment.
        //
        Status = LastFile->Open (
                LastFile,
                &NextFile,
                PathName,
                OpenMode,
                Attributes
        );

        //
        // Release any AlignedPathName on both error and success paths; PathName is
        // no longer needed.
        //
        if (AlignedPathName != NULL) {
            FreePool (AlignedPathName);
        }
        if (EFI_ERROR (Status)) {
            goto CloseLastFile;
        }

        //
        // Advance to the next device path node.
        //
        LastFile->Close (LastFile);
        LastFile = NextFile;
        *FilePath = NextDevicePathNode (FilePathNode);
    }

    *File = LastFile;
    return EFI_SUCCESS;

CloseLastFile:
    LastFile->Close (LastFile);

    //
    // We are on the error path; we must have set an error Status for returning
    // to the caller.
    //
    ASSERT (EFI_ERROR (Status));
    return Status;
}


EFI_STATUS GetBootDevicePath(EFI_DEVICE_PATH** BootDrive) {
    EFI_STATUS Status = EFI_SUCCESS;
    EFI_DEVICE_PATH* BootImage = NULL;

    CHECK(BootDrive != NULL);
    *BootDrive = NULL;

    // get the boot image device path
    EFI_CHECK(gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageDevicePathProtocolGuid, (void**)&BootImage));

    EFI_DEVICE_PATH* LastNode = LastDevicePathNode(BootImage);
    CHECK(LastNode->Type == MEDIA_DEVICE_PATH && LastNode->SubType == MEDIA_FILEPATH_DP);

    // now remove the last element (Which would be the device path)
    BootImage = RemoveLastDevicePathNode(BootImage);
    CHECK_STATUS(BootImage, EFI_OUT_OF_RESOURCES);

    // for hard drives we need to remove the
    // partition as well, so we can get all the
    // filesystems on that drive
    EFI_DEVICE_PATH* Node = LastDevicePathNode(BootImage);
    if (Node->Type == MEDIA_DEVICE_PATH && Node->SubType == MEDIA_HARDDRIVE_DP) {
        Node = RemoveLastDevicePathNode(BootImage);
        CHECK_STATUS(Node != NULL, EFI_OUT_OF_RESOURCES);
        FreePool(BootImage);
        BootImage = Node;
    }

    // set it
    *BootDrive = BootImage;

cleanup:
    return Status;
}