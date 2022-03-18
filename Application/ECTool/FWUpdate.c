#include <Library/CrosECLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiShellLib/UefiShellLib.h>

#include "EC.h"
#include "FWUpdate.h"
#include "Flash.h"

EFI_STATUS CheckReadyForECFlash() {
	UINT8 ecid[2];
	ECReadMemoryLPC(EC_MEMMAP_ID, ecid, 2);
	if(ecid[0] != 'E' || ecid[1] != 'C') {
		Print(L"This machine doesn't look like it has an EC\n");
		return EFI_INVALID_PARAMETER;
	}

	UINT32 batteryLfcc, batteryCap;
	UINT8 batteryFlag;
	ECReadMemoryLPC(EC_MEMMAP_BATT_FLAG, &batteryFlag, sizeof(batteryFlag));
	ECReadMemoryLPC(EC_MEMMAP_BATT_LFCC, &batteryLfcc, sizeof(batteryLfcc));
	ECReadMemoryLPC(EC_MEMMAP_BATT_CAP, &batteryCap, sizeof(batteryCap));

	if(0 == (batteryFlag & EC_BATT_FLAG_AC_PRESENT) || ((100ULL * batteryCap) / batteryLfcc) < 20) {
		Print(L"Make sure AC is connected and that the battery is at least 20%% charged.\n");
		return EFI_NOT_READY;
	}

	return EFI_SUCCESS;
}

#define FLASH_BASE    0x0  // 0x80000
#define FLASH_RO_BASE 0x0
#define FLASH_RO_SIZE 0x3C000
#define FLASH_RW_BASE 0x40000
#define FLASH_RW_SIZE 0x39000
EFI_STATUS cmd_reflash(int argc, CHAR16** argv) {
	EFI_STATUS Status = EFI_SUCCESS;
	SHELL_FILE_HANDLE FirmwareFile = NULL;
	EFI_FILE_INFO* FileInfo = NULL;
	EFI_INPUT_KEY Key = {0};
	int rv = 0;
	char* FirmwareBuffer = NULL;
	char* VerifyBuffer = NULL;
	UINTN ReadSize = 0;
	struct ec_params_flash_notified FlashNotifyParams = {0};

	if(argc < 2) {
		Print(L"ectool reflash FILE\n\nAttempts to safely reflash the Framework Laptop's EC\nPreserves flash "
		      L"region 3C000-3FFFF and 79000-7FFFF.\n");
		return 1;
	}

	Status = CheckReadyForECFlash();
	if(EFI_ERROR(Status)) {
		Print(L"System not ready\n");
		goto Out;
	}

	Status = ShellOpenFileByName(argv[1], &FirmwareFile, EFI_FILE_MODE_READ, 0);
	if(EFI_ERROR(Status)) {
		Print(L"Failed to open `%s': %r\n", argv[1], Status);
		goto Out;
	}

	FileInfo = ShellGetFileInfo(FirmwareFile);

	if(FileInfo->FileSize != (512 * 1024)) {
		Print(L"Firmware image is %d bytes (expected %d).\n", FileInfo->FileSize, (512 * 1024));
		Status = EFI_UNSUPPORTED;
		goto Out;
	}

	FirmwareBuffer = (char*)AllocatePool(FileInfo->FileSize);
	VerifyBuffer = (char*)AllocatePool(FileInfo->FileSize);
	if(!FirmwareBuffer || !VerifyBuffer) {
		Print(L"Failed to allocate an arena for the firmware image or verification buffer\n");
		Status = EFI_NOT_READY;
		goto Out;
	}

	ReadSize = FileInfo->FileSize;
	Status = ShellReadFile(FirmwareFile, &ReadSize, FirmwareBuffer);
	if(EFI_ERROR(Status)) {
		Print(L"Failed to read firmware: %r\n", Status);
		goto Out;
	}

	if(ReadSize != FileInfo->FileSize) {
		Print(L"Failed to read entire firmware image into memory.\n");
		Status = EFI_END_OF_FILE;
		goto Out;
	}

	Print(L"*** STARTING FLASH (PRESS ANY KEY TO CANCEL)\n");
	Key.ScanCode = SCAN_NULL;
	for(int i = 7; i > 0; i--) {
		Print(L"%d...", i);
		gBS->Stall(1000000);
		EFI_STATUS KeyStatus = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
		if(!EFI_ERROR(KeyStatus)) {
			Print(L"\nABORTED!\n");
			return EFI_ABORTED;
		}
	}
	Print(L"\n");

	Status = CheckReadyForECFlash();
	if(EFI_ERROR(Status)) {
		Print(L"System not ready\n");
		goto Out;
	}

	Print(L"Unlocking flash... ");
	FlashNotifyParams.flags = FLASH_ACCESS_SPI;
	rv = ECSendCommandLPCv3(EC_CMD_FLASH_NOTIFIED, 0, &FlashNotifyParams, sizeof(FlashNotifyParams), NULL, 0);
	if(rv < 0)
		goto EcOut;
	FlashNotifyParams.flags = FLASH_FIRMWARE_START;
	rv = ECSendCommandLPCv3(EC_CMD_FLASH_NOTIFIED, 0, &FlashNotifyParams, sizeof(FlashNotifyParams), NULL, 0);
	if(rv < 0)
		goto EcOut;
	Print(L"OK\n");

	Print(L"Erasing RO region... ");
	rv = flash_erase(FLASH_BASE + FLASH_RO_BASE, FLASH_RO_SIZE);
	if(rv < 0)
		goto EcOut;
	Print(L"OK\n");

	Print(L"Erasing RW region... ");
	rv = flash_erase(FLASH_BASE + FLASH_RW_BASE, FLASH_RW_SIZE);
	if(rv < 0)
		goto EcOut;
	Print(L"OK\n");

	Print(L"Writing RO region... ");
	rv = flash_write(FLASH_BASE + FLASH_RO_BASE, FLASH_RO_SIZE, FirmwareBuffer + FLASH_RO_BASE);
	if(rv < 0)
		goto EcOut;
	Print(L"OK\n");

	Print(L"Writing RW region... ");
	rv = flash_write(FLASH_BASE + FLASH_RW_BASE, FLASH_RW_SIZE, FirmwareBuffer + FLASH_RW_BASE);
	if(rv < 0)
		goto EcOut;
	Print(L"OK\n");

	Print(L"Verifying: Read... ");

	rv = flash_read(FLASH_BASE + FLASH_RO_BASE, FLASH_RO_SIZE, VerifyBuffer + FLASH_RO_BASE);
	if(rv < 0)
		goto EcOut;
	rv = flash_read(FLASH_BASE + FLASH_RW_BASE, FLASH_RW_SIZE, VerifyBuffer + FLASH_RW_BASE);
	if(rv < 0)
		goto EcOut;
	Print(L"OK. Check... ");

	if(CompareMem(VerifyBuffer + FLASH_RO_BASE, FirmwareBuffer + FLASH_RO_BASE, FLASH_RO_SIZE) == 0) {
		Print(L"RO OK... ");
	} else {
		Print(L"RO FAIL! ");
	}
	if(CompareMem(VerifyBuffer + FLASH_RW_BASE, FirmwareBuffer + FLASH_RW_BASE, FLASH_RW_SIZE) == 0) {
		Print(L"RW OK... ");
	} else {
		Print(L"RW FAIL! ");
	}
	Print(L"OK\n");

	Print(L"Locking flash... ");
	FlashNotifyParams.flags = FLASH_ACCESS_SPI_DONE;
	rv = ECSendCommandLPCv3(EC_CMD_FLASH_NOTIFIED, 0, &FlashNotifyParams, sizeof(FlashNotifyParams), NULL, 0);
	if(rv < 0)
		goto EcOut;
	FlashNotifyParams.flags = FLASH_FIRMWARE_DONE;
	rv = ECSendCommandLPCv3(EC_CMD_FLASH_NOTIFIED, 0, &FlashNotifyParams, sizeof(FlashNotifyParams), NULL, 0);
	if(rv < 0)
		goto EcOut;
	Print(L"OK\n");

	Print(L"\nLooks like it worked?\nConsider running `ectool reboot` to reset the EC/AP.\n");

EcOut:
	if(rv < 0) {
		PrintECResponse(rv);
		Print(L"\n");
		Print(L"*** YOUR COMPUTER MAY NO LONGER BOOT ***\n");
		Status = EFI_DEVICE_ERROR;
	}
Out:
	if(FirmwareFile) {
		ShellCloseFile(&FirmwareFile);
	}
	SHELL_FREE_NON_NULL(FileInfo);
	SHELL_FREE_NON_NULL(FirmwareBuffer);
	SHELL_FREE_NON_NULL(VerifyBuffer);
	return Status;
}
