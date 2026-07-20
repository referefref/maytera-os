// hello.c - Minimal UEFI application for MayteraOS
#include <efi.h>
#include <efilib.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);

    Print(L"========================================\r\n");
    Print(L"  MayteraOS UEFI Boot Test\r\n");
    Print(L"========================================\r\n");
    Print(L"\r\n");
    Print(L"UEFI Firmware initialized successfully!\r\n");
    Print(L"Toolchain working correctly.\r\n");
    Print(L"\r\n");
    Print(L"Press any key to exit...\r\n");

    // Wait for keypress
    EFI_INPUT_KEY Key;
    SystemTable->ConIn->Reset(SystemTable->ConIn, FALSE);
    while (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_NOT_READY);

    return EFI_SUCCESS;
}
