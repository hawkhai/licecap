#include "base_encoder.h"

ShareMemoryDll::ShareMemoryWrite sharememory(L"licecap.exe.log", 1024 * 1024 * 1);
ShareMemoryDll::ShareMemoryWrite sharememoryImage(L"licecap.exe.img", 1024 * 1024 * 50);

ShareMemoryDll::ShareMemoryWrite* getShareMemory() {
    return &sharememory;
}

ShareMemoryDll::ShareMemoryWrite* getShareMemoryImage() {
    return &sharememoryImage;
}

