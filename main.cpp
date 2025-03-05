#include <level_zero/ze_api.h>
#include <level_zero/zes_api.h>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <iomanip>
#include <algorithm>
#include <fstream>

class LevelZeroMonitor
{
public:
    LevelZeroMonitor()
    {
        initialize();
    }

    [[noreturn]] void run()
    {
        // Track the max number of lines ever printed (instead of just previous lines)
        long maxLinesUsed = 0;

        while (true)
        {
            std::stringstream buffer;

            printDeviceInfo(buffer);

            std::string output = buffer.str();

            long currentLines = std::count(output.begin(), output.end(), '\n');

            if (maxLinesUsed > 0)
            {
                std::cout << "\033[" << maxLinesUsed << "A";
            }

            std::cout << "\033[J";
            std::cout << output << std::flush;

            if (currentLines > maxLinesUsed)
            {
                maxLinesUsed = currentLines;
            }

            if (currentLines < maxLinesUsed)
            {
                for (int i = 0; i < maxLinesUsed - currentLines; i++)
                {
                    std::cout << '\n';
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

private:
    ze_driver_handle_t driverHandle{};
    std::vector<ze_device_handle_t> devices;
    std::unordered_map<zes_pwr_handle_t, std::pair<uint64_t, uint64_t>> powerData;
    static constexpr uint32_t MAX_PROCESS = 2048;

    void initialize()
    {
        if (zeInit(0) != ZE_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to initialize Level Zero!");
        }

        uint32_t driverCount = 1;
        if (zeDriverGet(&driverCount, &driverHandle) != ZE_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to get Level Zero driver!");
        }

        uint32_t deviceCount = 0;
        if (zeDeviceGet(driverHandle, &deviceCount, nullptr) != ZE_RESULT_SUCCESS || deviceCount == 0)
        {
            throw std::runtime_error("No Level Zero devices found!");
        }

        if (zesInit(0) != ZE_RESULT_SUCCESS)
        {
            throw std::runtime_error("Failed to initialize Level Zero Sysman!");
        }

        devices.resize(deviceCount);
        zeDeviceGet(driverHandle, &deviceCount, devices.data());
    }

    static zes_device_handle_t getSysmanDeviceHandleFromCoreDeviceHandle(const ze_device_handle_t *hDevice)
    {
        ze_device_properties_t deviceProperties = {ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
        ze_result_t result = zeDeviceGetProperties(*hDevice, &deviceProperties);
        if (result != ZE_RESULT_SUCCESS)
        {
            printf("Error: zeDeviceGetProperties failed, result = %d\n", result);
            return nullptr;
        }

        zes_uuid_t uuid = {};
        memcpy(uuid.id, deviceProperties.uuid.id, ZE_MAX_DEVICE_UUID_SIZE);

        uint32_t driverCount = 0;
        result = zesDriverGet(&driverCount, nullptr);
        if (driverCount == 0 || result != ZE_RESULT_SUCCESS)
        {
            printf("Error could not retrieve driver\n");
            exit(-1);
        }

        auto allDrivers = static_cast<zes_driver_handle_t*>(malloc(driverCount * sizeof(zes_driver_handle_t)));
        result = zesDriverGet(&driverCount, allDrivers);
        if (result != ZE_RESULT_SUCCESS)
        {
            free(allDrivers);
            printf("Error:  zesDriverGet failed, result = %d\n", result);
            return nullptr;
        }

        zes_device_handle_t phSysmanDevice = nullptr;
        ze_bool_t onSubdevice = false;
        uint32_t subdeviceId = 0;
        for (int it = 0; it < driverCount; it++)
        {
            result = zesDriverGetDeviceByUuidExp(allDrivers[it], uuid, &phSysmanDevice, &onSubdevice, &subdeviceId);
            if (result == ZE_RESULT_SUCCESS && (phSysmanDevice != nullptr))
            {
                break;
            }
        }
        free(allDrivers);

        return phSysmanDevice;
    }

    void printDeviceInfo(std::ostream &out)
    {
        for (const auto& device : devices)
        {
            ze_device_properties_t deviceProperties = {ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
            zeDeviceGetProperties(device, &deviceProperties);

            out << "Device: " << deviceProperties.name << "\n";

            auto sysmanDevice = getSysmanDeviceHandleFromCoreDeviceHandle(&device);
            if (!sysmanDevice)
                continue;

            getPowerStats(sysmanDevice, out);
            getMemoryInfo(sysmanDevice, out);
            getTemperatureInfo(sysmanDevice, out);
            getProcessInfo(sysmanDevice, out);
        }
    }

    void getPowerStats(const zes_device_handle_t sysmanDevice, std::ostream &out)
    {
        uint32_t powerCount = 0;
        if (zesDeviceEnumPowerDomains(sysmanDevice, &powerCount, nullptr) != ZE_RESULT_SUCCESS)
            return;


        double totalPowerUsage = 0.0;
        size_t validSamples = 0;

        if (powerCount > 0)
        {
            std::vector<zes_pwr_handle_t> powerHandles(powerCount);
            zesDeviceEnumPowerDomains(sysmanDevice, &powerCount, powerHandles.data());

            for (const auto& handle : powerHandles)
            {
                zes_power_energy_counter_t counter{};
                if (zesPowerGetEnergyCounter(handle, &counter) == ZE_RESULT_SUCCESS)
                {
                    if (powerData.find(handle) != powerData.end())
                    {
                        uint64_t prevEnergy = powerData[handle].first;
                        uint64_t prevTimestamp = powerData[handle].second;
                        uint64_t energyDelta = counter.energy - prevEnergy;
                        uint64_t timeDelta = counter.timestamp - prevTimestamp;
                        double powerUsage = (timeDelta > 0) ? (static_cast<double>(energyDelta) / static_cast<double>(timeDelta)) : 0;

                        totalPowerUsage += powerUsage;
                        ++validSamples;
                    }
                    powerData[handle] = {counter.energy, counter.timestamp};
                }
            }
        }
        if (validSamples > 0)
        {
            double averagePowerUsage = totalPowerUsage / static_cast<double>(validSamples);
            out << std::fixed << std::setprecision(2) << "Power: " << averagePowerUsage << " W\n";
        } else
        {
            out <<  "Power: n/a\n";
        }
    }

    static void getMemoryInfo(const zes_device_handle_t sysmanDevice, std::ostream &out)
    {
        uint32_t memCount = 0;
        if (zesDeviceEnumMemoryModules(sysmanDevice, &memCount, nullptr) != ZE_RESULT_SUCCESS || memCount == 0)
            return;

        std::vector<zes_mem_handle_t> memHandles(memCount);
        zesDeviceEnumMemoryModules(sysmanDevice, &memCount, memHandles.data());

        for (uint32_t i = 0; i < memCount; i++)
        {
            zes_mem_state_t memState{};
            if (zesMemoryGetState(memHandles[i], &memState) == ZE_RESULT_SUCCESS)
            {
                out << "Memory Module " << i << std::endl;
                out << "    Health: " << std::hex << memState.health << std::dec << "\n";
                out << "    Total Memory: " << memState.size / (1024 * 1024) << " MiB\n";
                out << "    Free Memory: " << memState.free / (1024 * 1024) << " MiB\n";
                const double usage = (static_cast<double>(memState.size - memState.free) / static_cast<double>(memState.size)) * 100;
                out << "    Used Memory: " << (memState.size - memState.free) / (1024 * 1024) << " MiB (" << std::fixed << std::setprecision(2) <<  usage << "%)\n";
            }
        }
    }

    static void getTemperatureInfo(const zes_device_handle_t sysmanDevice, std::ostream &out)
    {
        uint32_t tempSensorCount = 10;
        zes_temp_handle_t tempSensors[10];
        zesDeviceEnumTemperatureSensors(sysmanDevice, &tempSensorCount, tempSensors);

        out << "Temperature Sensors " << tempSensorCount << "\n";
        for (uint32_t i = 0; i < tempSensorCount; i++)
        {
            double temperature;
            if (zesTemperatureGetState(tempSensors[i], &temperature) == ZE_RESULT_SUCCESS)
            {
                out << "    Temperature Sensor " << i << ": " << temperature << "C\n";
            }
        }
    }

    static std::string getProcessName(uint32_t pid)
    {
        // Read the command line file
        std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::in | std::ios::binary);
        if (!file)
        {
            return ""; // Return empty string if file can't be read
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string cmdline = buffer.str();

        for (char &c : cmdline)
        {
            if (c == '\0')
                c = ' ';
        }

        return cmdline;
    }

    static void getProcessInfo(const zes_device_handle_t sysmanDevice, std::ostream &out)
    {
        zes_process_state_t processes[MAX_PROCESS];
        uint32_t count = MAX_PROCESS;

        ze_result_t ret = zesDeviceProcessesGetState(sysmanDevice, &count, processes);
        if (ret != ZE_RESULT_SUCCESS && ret != ZE_RESULT_ERROR_INVALID_SIZE)
        {
            out << "Unable to get process information (ret " << std::hex << ret << ")" << std::endl;
            return;
        }

        if (ret == ZE_RESULT_ERROR_INVALID_SIZE)
        {
            count = std::min(count, MAX_PROCESS);
            ret = zesDeviceProcessesGetState(sysmanDevice, &count, processes);
            if (ret != ZE_RESULT_SUCCESS)
            {
                out << "Retry failed to get process info (ret " << std::hex << ret << ")" << std::endl;
                return;
            }
        }

        out << std::endl;

        for (size_t i = 0; i < count; ++i)
        {
            out << "PID: " << processes[i].processId << " / Mem size: " << processes[i].memSize / (1024*1024) << " MiB / Shared mem: " << processes[i].sharedSize / (1024*1024) << " MiB \n";
            out << "    " << getProcessName(processes[i].processId) << std::endl;
        }


    }
};

int main()
{
    try
    {
        LevelZeroMonitor monitor;
        monitor.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n";
        return -1;
    }
    return 0;
}
