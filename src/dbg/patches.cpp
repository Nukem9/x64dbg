/**
 @file patches.cpp

 @brief Implements the patches class.
 */

#include "patches.h"
#include "memory.h"
#include "debugger.h"
#include "threading.h"
#include "module.h"

std::unordered_map<duint, PATCHINFO> patches;

bool PatchSet(duint Address, unsigned char OldByte, unsigned char NewByte)
{
    ASSERT_DEBUGGING("Export call");

    // Address must be valid
    if(!MemIsValidReadPtr(Address))
        return false;

    // Don't patch anything if the new and old values are the same
    if(OldByte == NewByte)
        return true;

    PATCHINFO newPatch;
    newPatch.addr = Address - ModBaseFromAddr(Address);
    newPatch.oldbyte = OldByte;
    newPatch.newbyte = NewByte;
    ModNameFromAddr(Address, newPatch.mod, true);

    // Generate a key for this address
    const duint key = ModHashFromAddr(Address);

    EXCLUSIVE_ACQUIRE(LockPatches);

    // Find any patch with this specific address
    auto found = patches.find(key);

    if(found != patches.end())
    {
        if(found->second.oldbyte == NewByte)
        {
            // The patch was undone here
            patches.erase(found);
            return true;
        }

        // Keep the original byte from the previous patch
        newPatch.oldbyte = found->second.oldbyte;
        found->second = newPatch;
    }
    else
    {
        // The entry was never found, insert it
        patches.insert(std::make_pair(key, newPatch));
    }

    return true;
}

bool PatchGet(duint Address, PATCHINFO* Patch)
{
    ASSERT_DEBUGGING("Export call");
    SHARED_ACQUIRE(LockPatches);

    // Find this specific address in the list
    auto found = patches.find(ModHashFromAddr(Address));

    if(found == patches.end())
        return false;

    // Did the user request an output buffer?
    if(Patch)
    {
        *Patch = found->second;
        Patch->addr += ModBaseFromAddr(Address);
    }

    // Return true because the patch was found
    return true;
}

bool PatchDelete(duint Address, bool Restore)
{
    ASSERT_DEBUGGING("Export call");
    EXCLUSIVE_ACQUIRE(LockPatches);

    // Do a list lookup with hash
    auto found = patches.find(ModHashFromAddr(Address));

    if(found == patches.end())
        return false;

    // Restore the original byte at this address
    if(Restore)
        MemWrite((found->second.addr + ModBaseFromAddr(Address)), &found->second.oldbyte, sizeof(char));

    // Finally remove it from the list
    patches.erase(found);
    return true;
}

void PatchDelRange(duint Start, duint End, bool Restore)
{
    ASSERT_DEBUGGING("Export call");

    // Are all bookmarks going to be deleted?
    // 0x00000000 - 0xFFFFFFFF
    if(Start == 0 && End == ~0)
    {
        EXCLUSIVE_ACQUIRE(LockPatches);
        patches.clear();
    }
    else
    {
        // Make sure 'Start' and 'End' reference the same module
        duint moduleBase = ModBaseFromAddr(Start);

        if(moduleBase != ModBaseFromAddr(End))
            return;

        // VA to RVA in module
        Start -= moduleBase;
        End -= moduleBase;

        EXCLUSIVE_ACQUIRE(LockPatches);
        for(auto itr = patches.begin(); itr != patches.end();)
        {
            const auto & currentPatch = itr->second;
            // [Start, End)
            if(currentPatch.addr >= Start && currentPatch.addr < End)
            {
                // Restore the original byte if necessary
                if(Restore)
                    MemWrite((currentPatch.addr + moduleBase), &currentPatch.oldbyte, sizeof(char));

                itr = patches.erase(itr);
            }
            else
                ++itr;
        }
    }
}

bool PatchEnum(PATCHINFO* List, size_t* Size)
{
    ASSERT_DEBUGGING("Export call");
    ASSERT_FALSE(!List && !Size);
    SHARED_ACQUIRE(LockPatches);

    // Did the user request the size?
    if(Size)
    {
        *Size = patches.size() * sizeof(PATCHINFO);

        if(!List)
            return true;
    }

    // Copy each vector entry to a C-style array
    for(auto & itr : patches)
    {
        *List = itr.second;
        List->addr += ModBaseFromName(itr.second.mod);;
        List++;
    }

    return true;
}

int PatchFile(const PATCHINFO* List, int Count, const char* FileName, char* Error)
{
    //
    // This function returns an int based on the number
    // of patches applied. -1 indicates a failure.
    //
    if(Count <= 0)
    {
        // Notify the user of the error
        if(Error)
            strcpy_s(Error, MAX_ERROR_SIZE, "No patches to apply");

        return -1;
    }

    // Get a copy of the first module name in the array
    char moduleName[MAX_MODULE_SIZE];
    strcpy_s(moduleName, List[0].mod);

    // Check if all patches are in the same module
    for(int i = 0; i < Count; i++)
    {
        if(_stricmp(List[i].mod, moduleName))
        {
            if(Error)
                sprintf_s(Error, MAX_ERROR_SIZE, "Not all patches are in module %s", moduleName);

            return -1;
        }
    }

    // See if the module was loaded
    duint moduleBase = ModBaseFromName(moduleName);

    if(!moduleBase)
    {
        if(Error)
            sprintf_s(Error, MAX_ERROR_SIZE, "Failed to get base of module %s", moduleName);

        return -1;
    }

    // Get the unicode version of the module's path
    wchar_t originalName[MAX_PATH];

    if(!GetModuleFileNameExW(fdProcessInfo->hProcess, (HMODULE)moduleBase, originalName, ARRAYSIZE(originalName)))
    {
        if(Error)
            sprintf_s(Error, MAX_ERROR_SIZE, "Failed to get module path of module %s", moduleName);

        return -1;
    }

    // Create a temporary backup file
    if(!CopyFileW(originalName, StringUtils::Utf8ToUtf16(FileName).c_str(), false))
    {
        if(Error)
            strcpy_s(Error, MAX_ERROR_SIZE, "Failed to make a copy of the original file (patch target is in use?)");

        return -1;
    }

    HANDLE fileHandle;
    DWORD loadedSize;
    HANDLE fileMap;
    ULONG_PTR fileMapVa;
    if(!StaticFileLoadW(StringUtils::Utf8ToUtf16(FileName).c_str(), UE_ACCESS_ALL, false, &fileHandle, &loadedSize, &fileMap, &fileMapVa))
    {
        strcpy_s(Error, MAX_ERROR_SIZE, "StaticFileLoad failed");
        return -1;
    }

    // Begin iterating all patches, applying them to a file
    int patchCount = 0;

    for(int i = 0; i < Count; i++)
    {
        // Convert the virtual address to an offset within disk file data
        unsigned char* ptr = (unsigned char*)ConvertVAtoFileOffsetEx(fileMapVa, loadedSize, moduleBase, List[i].addr, false, true);

        // Skip patches that do not have a raw address
        if(!ptr)
            continue;

        *ptr = List[i].newbyte;
        patchCount++;
    }

    // Unload the file from memory and commit changes to disk
    if(!StaticFileUnloadW(StringUtils::Utf8ToUtf16(FileName).c_str(), true, fileHandle, loadedSize, fileMap, fileMapVa))
    {
        if(Error)
            strcpy_s(Error, MAX_ERROR_SIZE, "StaticFileUnload failed");

        return -1;
    }

    // Zero the error message and return count
    if(Error)
        memset(Error, 0, MAX_ERROR_SIZE * sizeof(char));

    return patchCount;
}

void PatchClear(const char* Module)
{
    EXCLUSIVE_ACQUIRE(LockPatches);

    // Was a module specified?
    if(!Module || Module[0] == '\0')
    {
        // No specific entries to delete, so remove all of them
        patches.clear();
    }
    else
    {
        // Otherwise iterate over each patch and check the owner
        // module for the address
        for(auto itr = patches.begin(); itr != patches.end();)
        {
            if(!_stricmp(itr->second.mod, Module))
                itr = patches.erase(itr);
            else
                ++itr;
        }
    }
}