//
// Created by huangyuyang on 4/11/23.
//

#include <fcntl.h>
#include <sys/mman.h>
#include <cstring>
#include <algorithm>

#include "utils.h"
#include "devices/tfacc/fastllm-tfacc.h"
#include "json11.hpp"

namespace fastllm {
    enum ComputeTaskType {
        None = 0,
        LinearInt4NoZero = 1,
        LinearInt8 = 2,

        GetComputeServerInfo = 10000,
        StartLongData = 10001,
        FinishLongData = 10002
    };

    const int PAGE = 16 * 1024;
    static int transLimit = 28 * 1024 * 1024;

    struct U8Buffer {
        std::vector <uint8_t> buffer;

        void Clear() {
            buffer.clear();
        }

        void WriteInt(int v) {
            int oldLen = buffer.size();
            buffer.resize(oldLen + 4);
            ((int*)(buffer.data() + oldLen))[0] = v;
        }

        void WriteFloat(float v) {
            int oldLen = buffer.size();
            buffer.resize(oldLen + 4);
            ((float*)(buffer.data() + oldLen))[0] = v;
        }

        void WriteBytes(uint8_t *v, int len) {
            int oldLen = buffer.size();
            buffer.resize(oldLen + len);
            memcpy(buffer.data() + oldLen, v, len);
        }
    };

    TfaccClient::TfaccClient() {
        fd = open("/dev/thinkforce0", O_RDWR);
        buf = (volatile uint8_t *)mmap(NULL, 64 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 9997 * 0x1000);
        result = buf + 32 * 1024 * 1024;
        flag = (volatile int32_t*)(buf + 63 * 1024 * 1024);

        serverNumaCnt = 4;
        this->Launch(ComputeTaskType::GetComputeServerInfo);
        while (true) {
            int a = *flag;
            if (a == 0) {
                break;
            }
        }

        int len = ((int32_t*)this->result)[0];
        std::string infoString;
        for (int i = 0; i < len; i++) {
            infoString += this->result[4 + i];
        }
        std::string error;
        json11::Json info = json11::Json::parse(infoString, error);
        serverNumaCnt = info["numacnt"].int_value();
        this->Wait();
    }

    TfaccClient::~TfaccClient() {
        std::set <std::string> names = this->registerDataNames;
        for (auto &dataName : names) {
            this->UnregisterFastllmData(dataName);
        }
    }

    void TfaccClient::Launch(int opType) {
        asm volatile("dmb ish");
        volatile int *curFlag = flag;
        for (int i = 0; i < serverNumaCnt; i++) {
            *(curFlag) = opType;
            curFlag += PAGE;
            asm volatile("dmb ish");
        }
    }

    void TfaccClient::Wait() {
        while (true) {
            volatile int *curFlag = flag;
            int notFinish = 0;
            for (int i = 0; i < serverNumaCnt; i++) {
                notFinish |= (*curFlag);
                curFlag += PAGE;
            }
            if (!notFinish) {
                return;
            }
        }
    }

    void TfaccClient::SendLongMessage(uint8_t *buffer, int len) {
        for (int i = 0; i < len; i += transLimit) {
            int cur = std::min(transLimit, len - i);
            ((int32_t*)this->buf)[0] = cur;
            memcpy((uint8_t*)this->buf + 4, buffer + i, cur);
            this->Launch(ComputeTaskType::StartLongData);
            this->Wait();
        }
        this->Launch(ComputeTaskType::FinishLongData);
        this->Wait();
    }

    void TfaccClient::RegisterFastllmData(fastllm::Data *data, const std::string &weightType) {
        if (data->name == "" || this->registerDataNames.find(data->name) != this->registerDataNames.end()) {
            return;
        }

        this->registerDataNames.insert(data->name);
        json11::Json config = json11::Json::object {
            {"op", "registerData"},
            {"dataName", data->name},
            {"weightType", weightType}
        };
        std::string configString = config.dump();

        U8Buffer buffer;
        buffer.WriteInt(configString.size());
        buffer.WriteBytes((uint8_t*)configString.data(), configString.size());

        buffer.WriteInt((int)data->dims.size());
        for (int i : data->dims) {
            buffer.WriteInt(i);
        }
        DataType dataType = data->dataType;
        if (dataType == DataType::FLOAT32 || dataType == DataType::BFLOAT16 || dataType == DataType::FLOAT16) {
            buffer.WriteInt((int) dataType);
            buffer.WriteBytes(data->cpuData, data->GetBytes());
        } else if (dataType == DataType::INT8 || dataType == DataType::INT4 || dataType == DataType::INT4_NOZERO) {
            buffer.WriteInt((int) dataType);
            buffer.WriteInt(data->perChannelAxis);
            int k = data->perChannelAxis == -1 ? 1 : data->dims[data->perChannelAxis];
            for (int i = 0; i < k; i++) {
                buffer.WriteFloat(data->perChannelsConfigs[i].min);
                buffer.WriteFloat(data->perChannelsConfigs[i].max);
            }
            buffer.WriteBytes(data->cpuData, data->GetBytes());
        } else if (dataType == DataType::INT4_GROUP) {
            buffer.WriteInt((int) dataType);
            buffer.WriteInt(data->perChannelAxis);
            buffer.WriteInt(data->group);
            buffer.WriteInt(data->groupCnt);
            int k = data->perChannelAxis == -1 ? 1 : data->dims[data->perChannelAxis];
            for (int i = 0; i < k * data->group; i++) {
                buffer.WriteFloat(data->perChannelsConfigs[i].min);
                buffer.WriteFloat(data->perChannelsConfigs[i].max);
            }
            buffer.WriteBytes(data->cpuData, data->GetBytes());
        }

        SendLongMessage(buffer.buffer.data(), buffer.buffer.size());
    }

    void TfaccClient::UnregisterFastllmData(const std::string &dataName) {
        if (this->registerDataNames.find(dataName) == this->registerDataNames.end()) {
            return;
        }

        this->registerDataNames.erase(dataName);
        json11::Json config = json11::Json::object {
            {"op", "unregisterData"},
            {"dataName", dataName}
        };
        std::string configString = config.dump();

        U8Buffer buffer;
        buffer.WriteInt(configString.size());
        buffer.WriteBytes((uint8_t*)configString.data(), configString.size());
        SendLongMessage(buffer.buffer.data(), buffer.buffer.size());
    }

    void TfaccClient::RunTfaccLinearU(int n, int m, int k, 
                                     fastllm::Data *weight, fastllm::Data *bias,
                                     std::vector <LowBitConfig> *inputConfigs,
                                     uint8_t *uinput, float *output) {
        RegisterFastllmData(weight, "linear");
        RegisterFastllmData(bias, "bias");

        int opType = ComputeTaskType::LinearInt4NoZero;
        if (weight->dataType == DataType::INT8) {
            opType = ComputeTaskType::LinearInt8;
        }

        float *biasData = bias->dims.size() > 0 ? (float *) bias->cpuData : nullptr;
        std::string biasName = biasData == nullptr ? "" : bias->name;

        int maxN = n;
        maxN = std::min(maxN, transLimit / m);
        maxN = std::min(maxN, (int)(transLimit / (k * sizeof(float))));

        // printf("maxN = %d\n", maxN);
        for (int baseN = 0; baseN < n; baseN += maxN) {
            int curN = std::min(maxN, n - baseN);
            ((int32_t*)buf)[0] = curN;
            ((int32_t*)buf)[1] = m;
            ((int32_t*)buf)[2] = k;
            ((int32_t*)buf)[3] = 1; // group
            ((int32_t*)buf)[4] = weight->name.size();
            ((int32_t*)buf)[5] = biasName.size();

            volatile uint8_t *cur = (uint8_t*)buf + 10 * sizeof(int32_t);
            for (int i = 0; i < curN; i++) {
                ((float*)cur)[0] = (*inputConfigs)[baseN + i].min;
                ((float*)cur)[1] = (*inputConfigs)[baseN + i].max;
                cur += 2 * sizeof(float);
            }
            memcpy((uint8_t*)cur, weight->name.c_str(), weight->name.size());
            cur += weight->name.size();
            memcpy((uint8_t*)cur, biasName.c_str(), biasName.size());
            cur += biasName.size();
            memcpy((uint8_t*)cur, uinput + baseN * m, curN * m);

            this->Launch(opType);
            this->Wait();
            memcpy(((uint8_t*) output) + baseN * k * sizeof(int32_t),
                    (uint8_t*) result,
                    curN * k * sizeof(int32_t));
        }
    }
}
