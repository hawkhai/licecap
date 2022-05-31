#include "base_encoder.h"

ShareMemoryDll::ShareMemoryWrite sharememory(L"licecap.exe.log", 1024 * 1024 * 1);

ShareMemoryDll::ShareMemoryWrite* getShareMemory() {
    return &sharememory;
}
