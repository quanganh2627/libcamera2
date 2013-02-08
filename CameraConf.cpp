/*
 * Copyright 2012 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "Camera_Conf"

#include "CameraConf.h"
#include "LogHelper.h"
#include "PlatformData.h"
#include <utils/Errors.h>
#include <dirent.h>  // DIR, dirent
#include <fnmatch.h>  // fnmatch()
#include <fcntl.h>  // open(), close()...
#include <linux/media.h>  // media controller
#include <linux/kdev_t.h>  // MAJOR(), MINOR()

namespace android {

const char *cpfConfigPath = "/etc/atomisp/";  // Where CPF files are located
// FIXME: The spec for following is "dr%02d[0-9][0-9]??????????????.cpf"
const char *cpfConfigPattern = "*.cpf";  // How CPF file name should look
const char *subdevPathName = "/dev/v4l-subdev%d";  // Subdevs
const char *sysfsPath = "/sys/class/video4linux";  // Drivers
const char *mcPathName = "/dev/media0";  // Media Controller

// Defining and initializing static members
Vector<struct CpfStore::SensorDriver> CpfStore::registeredDrivers;
Vector<struct stat> CpfStore::validatedCpfFiles;

CameraBlob::CameraBlob(const int size)
{
    mSize = 0;
    if (size == 0) {
        mPtr = 0;
        LOGE("ERROR zero memory allocation!");
        return;
    }
    mPtr = malloc(size);
    if (!mPtr) {
        LOGE("ERROR memory allocation failure!");
        return;
    }
    mSize = size;
}

CameraBlob::CameraBlob(const sp<CameraBlob>& ref, const int offset, const int size)
{
    mSize = 0;
    mPtr = 0;
    if (ref == 0) {
        LOGE("ERROR referring to null object!");
        return;
    }
    // Must refer only to memory allocated by reference object
    if (ref->size() < offset + size) {
        LOGE("ERROR illegal allocation!");
        return;
    }
    mRef = ref;
    mSize = size;
    mPtr = (char *)(ref->ptr()) + offset;
}

CameraBlob::CameraBlob(const sp<CameraBlob>& ref, void * const ptr, const int size)
{
    mSize = 0;
    mPtr = 0;
    if (ref == 0) {
        LOGE("ERROR referring to null object!");
        return;
    }
    // Must refer only to memory allocated by reference object
    int offset = (char *)(ptr) - (char *)(ref->ptr());
    if ((offset < 0) || (offset + size > ref->size())) {
        LOGE("ERROR illegal allocation!");
        return;
    }
    mRef = ref;
    mSize = size;
    mPtr = ptr;
}

CameraBlob::~CameraBlob()
{
    if ((mRef == 0) && (mPtr)) {
        free(mPtr);
    }
}

CpfStore::CpfStore(const int cameraId)
    : mCameraId(cameraId)
    , mIsOldConfig(false)
{
    // If anything goes wrong here, we simply return silently.
    // CPF should merely be seen as a way to do multiple configurations
    // at once; failing in that is not a reason to return with errors
    // and terminate the camera (some cameras may not have any CPF at all)

    if (mCameraId >= PlatformData::numberOfCameras()) {
        LOGE("ERROR bad camera index");
        mCameraId = -1;
        return;
    }

    // Find out the name of the CPF config file
    if (initNames(mCpfPathName, mSysfsPathName)) {
        LOGE("ERROR could not get CPF file name");
        return;
    }

    // Get separate CPF configurations from CPF config file
    if (initConf(mAiqConf, mDrvConf, mHalConf)) {
        LOGE("ERROR could not get CPF configuration");
        return;
    }

    // Provide configuration data to driver and clean pointer
    // to that data
    processDrvConf();

    // Process configuration data to HAL and clean pointer to that data
    processHalConf();
}

CpfStore::~CpfStore()
{
}

const sp<CameraConf> CpfStore::createCameraConf()
{
    if (mCameraId < 0) {
        return 0;
    }

    sp<CameraConf> cfg = new CameraConf();
    if (cfg == 0) {
        LOGE("ERROR no memory in %s", __func__);
        return 0;
    }

    cfg->mCameraId = mCameraId;
    cfg->mCameraFacing = PlatformData::cameraFacing(mCameraId);
    cfg->mCameraOrientation = PlatformData::cameraOrientation(mCameraId);

    cfg->aiqConf = mAiqConf;

    return cfg;
}

status_t CpfStore::initNames(String8& cpfName, String8& sysfsName)
{
    String8 name;
    status_t ret = 0;
    int drvIndex = -1;  // Index of registered driver for which CPF file exists
    bool anyMatch = false;

    if ((ret = initDriverList())) {
        LOGE("ERROR could not obtain list of sensor drivers");
        return ret;
    }

    // We go the directory containing CPF files thru one by one file,
    // and see if a particular file is something to react upon. If yes,
    // we then see if there is a corresponding driver registered. It
    // is allowed to have more than one CPF file for particular driver
    // (logic therein decides which one to use, then), but having
    // more than one suitable driver registered is a strict no no...

    // Sensor drivers have been registered to media controller
    DIR *dir = opendir(cpfConfigPath);
    if (!dir) {
        LOGE("ERROR in opening CPF folder \"%s\": %s", cpfConfigPath, strerror(errno));
        return ENOTDIR;
    }

    do {
        dirent *entry;
        if (errno = 0, !(entry = readdir(dir))) {
            if (errno) {
                LOGE("ERROR in browsing CPF folder \"%s\": %s", cpfConfigPath, strerror(errno));
                ret = FAILED_TRANSACTION;
            }
            // If errno was not set, return 0 means end of directory.
            // So, let's see if we found any (suitable) CPF files
            if (drvIndex < 0) {
                if (anyMatch) {
                    LOGE("ERROR no suitable CPF files found in CPF folder \"%s\"", cpfConfigPath);
                } else {
                    LOGE("ERROR not a single CPF file found in CPF folder \"%s\"", cpfConfigPath);
                }
                ret = NO_INIT;
            }
            break;
        }
        if ((strcmp(entry->d_name, ".") == 0) ||
            (strcmp(entry->d_name, "..") == 0)) {
            continue;  // Skip self and parent
        }
        String8 pattern = String8::format(cpfConfigPattern, mCameraId);
        int r = fnmatch(pattern, entry->d_name, 0);
        switch (r) {
        case 0:
            // The file name looks like a valid CPF file name
            anyMatch = true;
            // See if we have corresponding driver registered
            // (if there is an error, the looping ends at 'while')
            ret = initNamesHelper(String8(entry->d_name), name, drvIndex);
            continue;
        case FNM_NOMATCH:
            // The file name did not look like a CPF file name
            continue;
        default:
            // Unknown error (the looping ends at 'while')
            LOGE("ERROR in pattern matching file name \"%s\"", entry->d_name);
            ret = UNKNOWN_ERROR;
            continue;
        }
    } while (!ret);

    if (closedir(dir)) {
        LOGE("ERROR in closing CPF folder \"%s\": %s", cpfConfigPath, strerror(errno));
        if (!ret) ret = EPERM;
    }

    if (!ret) {
        // Here is the correct CPF file, finally found out
        cpfName = String8(cpfConfigPath).appendPath(name);
        sysfsName = String8(sysfsPath).appendPath(registeredDrivers[drvIndex].mSysfsName);
    }

    return ret;
}

status_t CpfStore::initNamesHelper(const String8& filename, String8& refName, int& index)
{
    status_t ret = 0;

    for (int i = registeredDrivers.size(); i-- > 0; ) {
        if (filename.find(registeredDrivers[i].mSensorName) < 0) {
            // Name of this registered driver was not found
            // from within CPF looking file name -> skip it
            continue;
        } else {
            // Since we are here, we do have a registered
            // driver whose name maps to this CPF file name
            if (index < 0) {
                // No previous CPF<>driver pairs
                index = i;
                refName = filename;
            } else {
                if (index == i) {
                    // Multiple CPF files match the driver
                    // Let's use the most recent one
                    if (strcmp(filename, refName) > 0) {
                        refName = filename;
                    }
                } else {
                    // We just got lost:
                    // Which is the correct sensor driver?
                    LOGE("ERROR multiple driver candidates for CPF file \"%s\"", filename.string());
                    ret = ENOTUNIQ;
                }
            }
        }
    }

    return ret;
}

status_t CpfStore::initDriverList()
{
    status_t ret = 0;

    if (registeredDrivers.size() > 0) {
        // We only need to go through the drivers once
        return ret;
    }

    // Sensor drivers have been registered to media controller
    int fd = open(mcPathName, O_RDONLY);
    if (fd == -1) {
        LOGE("ERROR in opening media controller: %s", strerror(errno));
        return ENXIO;
    }

    struct media_entity_desc entity;
    memset(&entity, 0, sizeof(entity));
    do {
        // Go through the list of media controller entities
        entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) < 0) {
            if (errno == EINVAL) {
                // Ending up here when no more entities left.
                // Will simply 'break' if everything was ok
                if (registeredDrivers.size() == 0) {
                    // No registered drivers found
                    LOGE("ERROR no sensor driver registered in media controller");
                    ret = NO_INIT;
                }
            } else {
                LOGE("ERROR in browsing media controller entities: %s", strerror(errno));
                ret = FAILED_TRANSACTION;
            }
            break;
        } else {
            if (entity.type == MEDIA_ENT_T_V4L2_SUBDEV_SENSOR) {
                // A driver has been found!
                // The driver is using sensor name when registering
                // to media controller (we will truncate that to
                // first space, if any); but we also have to find the
                // proper driver name for sysfs usage
                SensorDriver drvInfo;
                drvInfo.mSensorName = entity.name;
                // Cut the name to first space
                for(int i = 0; (i = drvInfo.mSensorName.find(" ")) > 0; drvInfo.mSensorName.setTo(drvInfo.mSensorName, i));

                int major = entity.v4l.major;
                int minor = entity.v4l.minor;

                // Go thru the subdevs one by one, see which one
                // corresponds to this driver (if there is an error,
                // the looping ends at 'while')
                ret = initDriverListHelper(major, minor, drvInfo);
            }
        }
    } while (!ret);

    if (close(fd)) {
        LOGE("ERROR in closing media controller: %s", strerror(errno));
        if (!ret) ret = EPERM;
    }

    return ret;
}

status_t CpfStore::initDriverListHelper(int major, int minor, SensorDriver& drvInfo)
{
    String8 subdevPathNameN;

    for (int n = 0; true; n++) {
        subdevPathNameN = String8::format(subdevPathName, n);
        struct stat fileInfo;
        if (stat(subdevPathNameN, &fileInfo) < 0) {
            // We end up here when there are no more subdevs
            if (errno == ENOENT) {
                LOGE("ERROR sensor subdev missing: \"%s\"", subdevPathNameN.string());
                return NO_INIT;
            } else {
                LOGE("ERROR querying sensor subdev filestat for \"%s\": %s", subdevPathNameN.string(), strerror(errno));
                return FAILED_TRANSACTION;
            }
        }
        if ((major == MAJOR(fileInfo.st_rdev)) && (minor == MINOR(fileInfo.st_rdev))) {
            drvInfo.mSysfsName = subdevPathNameN.getPathLeaf();
            registeredDrivers.push(drvInfo);
            LOGD("Registered sensor driver \"%s\" found for sensor \"%s\"", drvInfo.mSysfsName.string(), drvInfo.mSensorName.string());
            // All ok
            break;
        }
    }

    return 0;
}

status_t CpfStore::initConf(sp<CameraBlob>& aiqConf, sp<CameraBlob>& drvConf, sp<CameraBlob>& halConf)
{
    sp<CameraBlob> allConf;
    status_t ret = 0;

    // First, we load the correct configuration file.
    // It will be behind reference counted MemoryHeapBase
    // object "allConf", meaning that the memory will be
    // automatically freed when it is no longer being pointed at
    if ((ret = loadConf(allConf)))
        return ret;

    // Then, we will dig out component specific configuration
    // data from within "allConf". That will be placed behind
    // reference counting MemoryBase memory descriptors.
    // We only need to verify checksum once
    if ((ret = fetchConf(allConf, aiqConf, tbd_class_aiq, "AIQ")))
        return ret;
    if ((ret = fetchConf(allConf, drvConf, tbd_class_dvr, "DRV")))
        return ret;
    if ((ret = fetchConf(allConf, halConf, tbd_class_hal, "HAL")))
        return ret;

    return ret;
}

status_t CpfStore::loadConf(sp<CameraBlob>& allConf)
{
    FILE *file;
    struct stat statCurrent;
    status_t ret = 0;

    LOGD("Opening CPF file \"%s\"", mCpfPathName.string());
    file = fopen(mCpfPathName, "rb");
    if (!file) {
        LOGE("ERROR in opening CPF file \"%s\": %s", mCpfPathName.string(), strerror(errno));
        return NAME_NOT_FOUND;
    }

    do {
        int fileSize;
        if ((fseek(file, 0, SEEK_END) < 0) ||
            ((fileSize = ftell(file)) < 0) ||
            (fseek(file, 0, SEEK_SET) < 0)) {
            LOGE("ERROR querying properties of CPF file \"%s\": %s", mCpfPathName.string(), strerror(errno));
            ret = ESPIPE;
            break;
        }

        allConf = new CameraBlob(fileSize);
        if ((allConf == 0) || (allConf->ptr() == 0)) {
            LOGE("ERROR no memory in %s", __func__);
            ret = NO_MEMORY;
            break;
        }

        if (fread(allConf->ptr(), fileSize, 1, file) < 1) {
            LOGE("ERROR reading CPF file \"%s\"", mCpfPathName.string());
            ret = EIO;
            break;
        }

        // We use file statistics for file identification purposes.
        // The access time was just changed (because of us!),
        // so let's nullify the access time info
        if (stat(mCpfPathName, &statCurrent) < 0) {
            LOGE("ERROR querying filestat of CPF file \"%s\": %s", mCpfPathName.string(), strerror(errno));
            ret = FAILED_TRANSACTION;
            break;
        }
        statCurrent.st_atime = 0;
        statCurrent.st_atime_nsec = 0;

    } while (0);

    if (fclose(file)) {
        LOGE("ERROR in closing CPF file \"%s\": %s", mCpfPathName.string(), strerror(errno));
        if (!ret) ret = EPERM;
    }

    if (!ret) {
        ret = validateConf(allConf, statCurrent);
    }

    return ret;
}

status_t CpfStore::validateConf(const sp<CameraBlob>& allConf, const struct stat& statCurrent)
{
    status_t ret = 0;

    // In case the very same CPF configuration file has been verified
    // already earlier, checksum calculation will be skipped this time.
    // Files are identified by their stat structure. If we set the
    // cache size equal to number of cameras in the system, checksum
    // calculations are avoided when user switches between cameras.
    // Note: the capacity could be set to zero as well if one wants
    // to validate the file in every case
    validatedCpfFiles.setCapacity(PlatformData::numberOfCameras());
    bool& canSkipChecksum = mIsOldConfig = false;

    // See if we know the file already
    for (int i = validatedCpfFiles.size() - 1; i >= 0; i--) {
        if (!memcmp(&validatedCpfFiles[i], &statCurrent, sizeof(struct stat))) {
            canSkipChecksum = true;
            break;
        }
    }

    if (canSkipChecksum) {
        LOGD("CPF file already validated");
    } else {
        LOGD("CPF file not validated yet, validating...");
        if (tbd_err_none != tbd_validate(allConf->ptr(), allConf->size(), tbd_tag_cpff)) {
            // Error, looks like we had unknown file
            LOGE("ERROR corrupted CPF file");
            ret = DEAD_OBJECT;
            return ret;
        }
    }

    // If we are here, the file was ok. If it wasn't cached already,
    // then do so now (adding to end of cache, removing from beginning)
    if (!canSkipChecksum) {
        if (validatedCpfFiles.size() < validatedCpfFiles.capacity()) {
            validatedCpfFiles.push_back(statCurrent);
        } else {
            if (validatedCpfFiles.size() > 0) {
                validatedCpfFiles.removeAt(0);
                validatedCpfFiles.push_back(statCurrent);
            }
        }
    }

    return ret;
}

status_t CpfStore::fetchConf(const sp<CameraBlob>& allConf, sp<CameraBlob>& recConf, tbd_class_t recordClass, const char *blockDebugName)
{
    status_t ret = 0;

    if (allConf == 0) {
        // This should never happen; CPF file has not been loaded properly
        LOGE("ERROR null pointer provided");
        return NO_MEMORY;
    }

    // The contents have been validated already, let's look for specific record
    void *data;
    size_t size;
    if (!(ret = tbd_get_record(allConf->ptr(), recordClass, tbd_format_any, &data, &size))) {
        if (data && size) {
            recConf = new CameraBlob(allConf, data, size);
            if (recConf == 0) {
                LOGE("ERROR no memory in %s", __func__);
                ret = NO_MEMORY;
            } else {
                LOGD("CPF %s record found", blockDebugName);
            }
        } else {
            // Looks like we didn't have AIQ record in CPF file
            LOGD("CPF %s record missing!", blockDebugName);
        }
    }

    return ret;
}

status_t CpfStore::processDrvConf()
{
    // FIXME: To be only cleared after use
    mDrvConf.clear();

    // Only act if CPF file has been updated and there is some data
    // to be sent
    if (mIsOldConfig || mDrvConf == 0) {
        return 0;
    }

    status_t ret = 0;

    // There is a limitation in sysfs; maximum data size to be sent
    // is one page
    if (mDrvConf->size() > getpagesize()) {
        LOGE("ERROR too big driver configuration record");
        return EOVERFLOW;
    }

    // Now, let's write the driver configuration data via sysfs
    LOGD("Writing to sysfs file \"%s\"", mSysfsPathName.string());
    int fd = open(mSysfsPathName, O_WRONLY);
    if (fd == -1) {
        LOGE("ERROR in opening sysfs write file \"%s\": %s", mSysfsPathName.string(), strerror(errno));
        return NO_INIT;
    }

    // Writing the driver data record...
    int bytes = write(fd, mDrvConf->ptr(), mDrvConf->size());
    if (bytes < 0) {
        LOGE("ERROR in writing sysfs data: %s", strerror(errno));
        ret = EIO;
    }
    if ((bytes == 0) || (bytes != mDrvConf->size())) {
        LOGE("ERROR in writing sysfs data");
        ret = EIO;
    }

    if (close(fd)) {
        LOGE("ERROR in closing sysfs write file \"%s\": %s", mSysfsPathName.string(), strerror(errno));
        if (!ret) ret = EPERM;
    }

    return ret;
}

status_t CpfStore::processHalConf()
{
    // FIXME: To be only cleared after use
    mHalConf.clear();

    return 0;
}

} // namespace android
