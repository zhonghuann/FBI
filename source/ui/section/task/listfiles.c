#include <sys/syslimits.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "../../list.h"
#include "../../error.h"
#include "../../../util.h"
#include "../../../screen.h"
#include "task.h"

typedef struct {
    list_item* items;
    u32* count;
    u32 max;

    Handle cancelEvent;

    file_info* dir;
} populate_files_data;

static void task_populate_files_thread(void* arg) {
    populate_files_data* data = (populate_files_data*) arg;

    data->dir->containsCias = false;

    Result res = 0;

    if(data->max > *data->count) {
        Handle dirHandle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenDirectory(&dirHandle, *data->dir->archive, fsMakePath(PATH_ASCII, data->dir->path)))) {
            u32 entryCount = 0;
            FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(data->max, sizeof(FS_DirectoryEntry));
            if(entries != NULL) {
                if(R_SUCCEEDED(res = FSDIR_Read(dirHandle, &entryCount, data->max, entries)) && entryCount > 0) {
                    qsort(entries, entryCount, sizeof(FS_DirectoryEntry), util_compare_directory_entries);

                    SMDH smdh;
                    for(u32 i = 0; i < entryCount && i < data->max; i++) {
                        if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
                            break;
                        }

                        if(entries[i].attributes & FS_ATTRIBUTE_HIDDEN) {
                            continue;
                        }

                        file_info* fileInfo = (file_info*) calloc(1, sizeof(file_info));
                        if(fileInfo != NULL) {
                            char entryName[0x213] = {'\0'};
                            utf16_to_utf8((uint8_t*) entryName, entries[i].name, sizeof(entryName) - 1);

                            fileInfo->archive = data->dir->archive;
                            strncpy(fileInfo->name, entryName, NAME_MAX);

                            list_item* item = &data->items[*data->count];

                            if(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) {
                                item->rgba = COLOR_DIRECTORY;

                                snprintf(fileInfo->path, PATH_MAX, "%s%s/", data->dir->path, entryName);
                                fileInfo->isDirectory = true;
                                fileInfo->containsCias = false;
                                fileInfo->size = 0;
                                fileInfo->isCia = false;
                            } else {
                                item->rgba = COLOR_TEXT;

                                snprintf(fileInfo->path, PATH_MAX, "%s%s", data->dir->path, entryName);
                                fileInfo->isDirectory = false;
                                fileInfo->containsCias = false;
                                fileInfo->size = 0;
                                fileInfo->isCia = false;

                                Handle fileHandle;
                                if(R_SUCCEEDED(FSUSER_OpenFile(&fileHandle, *data->dir->archive, fsMakePath(PATH_ASCII, fileInfo->path), FS_OPEN_READ, 0))) {
                                    FSFILE_GetSize(fileHandle, &fileInfo->size);

                                    AM_TitleEntry titleEntry;
                                    if(R_SUCCEEDED(AM_GetCiaFileInfo(MEDIATYPE_SD, &titleEntry, fileHandle))) {
                                        data->dir->containsCias = true;

                                        fileInfo->isCia = true;
                                        fileInfo->ciaInfo.titleId = titleEntry.titleID;
                                        fileInfo->ciaInfo.version = titleEntry.version;
                                        fileInfo->ciaInfo.installedSizeSD = titleEntry.size;
                                        fileInfo->ciaInfo.hasSmdh = false;

                                        if(R_SUCCEEDED(AM_GetCiaFileInfo(MEDIATYPE_NAND, &titleEntry, fileHandle))) {
                                            fileInfo->ciaInfo.installedSizeNAND = titleEntry.size;
                                        } else {
                                            fileInfo->ciaInfo.installedSizeNAND = 0;
                                        }

                                        u32 bytesRead;
                                        if(R_SUCCEEDED(FSFILE_Read(fileHandle, &bytesRead, fileInfo->size - sizeof(SMDH), &smdh, sizeof(SMDH))) && bytesRead == sizeof(SMDH)) {
                                            if(smdh.magic[0] == 'S' && smdh.magic[1] == 'M' && smdh.magic[2] == 'D' &&
                                                smdh.magic[3] == 'H') {
                                                u8 systemLanguage = CFG_LANGUAGE_EN;
                                                CFGU_GetSystemLanguage(&systemLanguage);

                                                fileInfo->ciaInfo.hasSmdh = true;
                                                utf16_to_utf8((uint8_t *) fileInfo->ciaInfo.smdhInfo.shortDescription, smdh.titles[systemLanguage].shortDescription, sizeof(fileInfo->ciaInfo.smdhInfo.shortDescription));
                                                utf16_to_utf8((uint8_t *) fileInfo->ciaInfo.smdhInfo.longDescription, smdh.titles[systemLanguage].longDescription, sizeof(fileInfo->ciaInfo.smdhInfo.longDescription));
                                                utf16_to_utf8((uint8_t *) fileInfo->ciaInfo.smdhInfo.publisher, smdh.titles[systemLanguage].publisher, sizeof(fileInfo->ciaInfo.smdhInfo.publisher));
                                                fileInfo->ciaInfo.smdhInfo.texture = screen_load_texture_tiled_auto(smdh.largeIcon, sizeof(smdh.largeIcon), 48, 48, GPU_RGB565, false);
                                            }
                                        }
                                    }

                                    FSFILE_Close(fileHandle);
                                }
                            }

                            strncpy(item->name, entryName, NAME_MAX);
                            item->data = fileInfo;

                            (*data->count)++;
                        }
                    }
                }

                free(entries);
            } else {
                res = MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, 254, RD_OUT_OF_MEMORY);
            }

            FSDIR_Close(dirHandle);
        }
    }

    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to load file listing.");
    }

    svcCloseHandle(data->cancelEvent);
    free(data);
}

static void task_clear_files(list_item* items, u32* count) {
    if(items == NULL || count == NULL) {
        return;
    }

    u32 prevCount = *count;
    *count = 0;

    for(u32 i = 0; i < prevCount; i++) {
        if(items[i].data != NULL) {
            file_info* fileInfo = (file_info*) items[i].data;
            if(fileInfo->isCia && fileInfo->ciaInfo.hasSmdh) {
                screen_unload_texture(fileInfo->ciaInfo.smdhInfo.texture);
            }

            free(items[i].data);
            items[i].data = NULL;
        }

        memset(items[i].name, '\0', NAME_MAX);
        items[i].rgba = 0;
    }
}

Handle task_populate_files(list_item* items, u32* count, u32 max, file_info* dir) {
    if(items == NULL || count == NULL || max == 0 || dir == NULL) {
        return 0;
    }

    task_clear_files(items, count);

    populate_files_data* data = (populate_files_data*) calloc(1, sizeof(populate_files_data));
    data->items = items;
    data->count = count;
    data->max = max;
    data->dir = dir;

    Result eventRes = svcCreateEvent(&data->cancelEvent, 1);
    if(R_FAILED(eventRes)) {
        error_display_res(NULL, NULL, eventRes, "Failed to create file list cancel event.");

        free(data);
        return 0;
    }

    if(threadCreate(task_populate_files_thread, data, 0x4000, 0x18, 1, true) == NULL) {
        error_display(NULL, NULL, "Failed to create file list thread.");

        svcCloseHandle(data->cancelEvent);
        free(data);
        return 0;
    }

    return data->cancelEvent;
}