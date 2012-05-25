/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utils/Log.h>
#include "LogHelper.h"

int32_t gLogLevel = 0;

using namespace android;

const char CameraParamsLogger::ParamsDelimiter[] = ";";
const char CameraParamsLogger::ValueDelimiter[]  = "=";

CameraParamsLogger::CameraParamsLogger(const char * params):mString(params) {

    fillMap(mPropMap, mString);
}

CameraParamsLogger::~CameraParamsLogger() {
    mPropMap.clear();
    mString.clear();
}

void
CameraParamsLogger::dump() {
    LOG2("Dumping Camera Params");
    for (unsigned int i = 0; i < mPropMap.size(); i++) {
        LOG2("%s=%s",mPropMap.keyAt(i).string(), mPropMap.valueAt(i).string());
    }
}
void CameraParamsLogger::dumpDifference(CameraParamsLogger &oldParams) {

    int longParamsSize = mPropMap.size();
    KeyedVector<String8, String8> &longMap = mPropMap;
    KeyedVector<String8, String8> &shortMap = oldParams.mPropMap;
    bool longNew = true;

    if( mPropMap.size() < oldParams.mPropMap.size()) {

        longParamsSize = oldParams.mPropMap.size();
        longMap = oldParams.mPropMap;
        shortMap = mPropMap;
        longNew = false;

    }

    for (int i = 0; i < longParamsSize; i++) {
        if (shortMap.indexOfKey(longMap.keyAt(i)) != NAME_NOT_FOUND) {
            if(shortMap.valueFor(longMap.keyAt(i)) != longMap.valueAt(i)) {
                LOG1("Param [%s] changed from %s - to - %s",longMap.keyAt(i).string(),
                                                            oldParams.mPropMap.valueFor(longMap.keyAt(i)).string(),
                                                            mPropMap.valueFor(longMap.keyAt(i)).string());
            }

        } else {
            if(!longNew) {
                LOG1("Param [%s] not specified in new params", longMap.keyAt(i).string());
            }else {
                LOG1("New Param [%s] = %s",longMap.keyAt(i).string(), longMap.valueAt(i).string());
            }

        }
    }

}

int
CameraParamsLogger::splitParam(String8 &inParam , String8  &aKey, String8 &aValue) {

    ssize_t  start = 0, end = 0;

    end = inParam.find( ValueDelimiter, start);
    if (end == -1)
        return -1;

    aKey.setTo( &(inParam.string()[start]), end - start);

    start = end + 1;

    aValue.setTo(&(inParam.string()[start]), inParam.size() - start);

    return 0;
}

void
CameraParamsLogger::fillMap(KeyedVector<String8,String8> &aMap, String8 &aString) {

    ssize_t  start = 0, end = 0;
    String8 theParam;
    String8 aKey, aValue;

    while ( end != -1)
    {
       end = aString.find( ParamsDelimiter, start);

       theParam.setTo( &(aString.string()[start]),(end == -1) ? aString.size() - start : end - start);

       if(splitParam(theParam, aKey, aValue)) {
            LOGE("Invalid Param: %s", theParam.string());
            start = end + 1;
            continue;
       }
       aMap.add(aKey, aValue);
       start = end + 1;
    }

}