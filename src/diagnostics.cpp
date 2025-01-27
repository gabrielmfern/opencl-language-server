//
//  diagnostics.cpp
//  opencl-language-server
//
//  Created by Ilya Shoshin (Galarius) on 7/16/21.
//

#include "diagnostics.hpp"
#include "utils.hpp"

#include <CL/opencl.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <spdlog/spdlog.h>
#include <stdexcept> // std::runtime_error, std::invalid_argument
#include <tuple>

using namespace nlohmann;

namespace {

constexpr char logger[] = "diagnostics";

int ParseSeverity(const std::string& severity)
{
    if (severity == "error")
        return 1;
    else if (severity == "warning")
        return 2;
    else
        return -1;
}

// <program source>:13:5: warning: no previous prototype for function 'getChannel'
std::tuple<std::string, long, long, long, std::string> ParseOutput(const std::smatch& matches)
{
    std::string source = matches[1];
    const long line = std::stoi(matches[2]) - 1; // LSP assumes 0-indexed lines
    const long col = std::stoi(matches[3]);
    const int severity = ParseSeverity(matches[4]);
    // matches[5] - 'fatal'
    std::string message = matches[6];
    return std::make_tuple(std::move(source), line, col, severity, std::move(message));
}

size_t GetDevicePowerIndex(const cl::Device& device)
{
    const size_t maxComputeUnits = device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();
    const size_t maxClockFrequency = device.getInfo<CL_DEVICE_MAX_CLOCK_FREQUENCY>();
    return maxComputeUnits * maxClockFrequency;
}

} // namespace

namespace ocls {

class Diagnostics final : public IDiagnostics
{
public:
    explicit Diagnostics(std::shared_ptr<ICLInfo> clInfo);

    void SetBuildOptions(const nlohmann::json& options);
    void SetMaxProblemsCount(int maxNumberOfProblems);
    void SetOpenCLDevice(uint32_t identifier);
    nlohmann::json Get(const Source& source);

private:
    nlohmann::json BuildDiagnostics(const std::string& buildLog, const std::string& name);
    std::string BuildSource(const std::string& source) const;

private:
    std::shared_ptr<ICLInfo> m_clInfo;
    std::optional<cl::Device> m_device;
    std::regex m_regex {"^(.*):(\\d+):(\\d+): ((fatal )?error|warning|Scholar): (.*)$"};
    std::string m_BuildOptions;
    int m_maxNumberOfProblems = 100;
};

Diagnostics::Diagnostics(std::shared_ptr<ICLInfo> clInfo) : m_clInfo {std::move(clInfo)}
{
    SetOpenCLDevice(0);
}

void Diagnostics::SetOpenCLDevice(uint32_t identifier)
{
    spdlog::get(logger)->trace("Selecting OpenCL platform...");
    std::vector<cl::Platform> platforms;
    try
    {
        cl::Platform::get(&platforms);
    }
    catch (cl::Error& err)
    {
        spdlog::get(logger)->error("No OpenCL platforms were found, {}", err.what());
    }

    spdlog::get(logger)->info("Found OpenCL platforms: {}", platforms.size());
    if (platforms.size() == 0)
    {
        return;
    }

    std::string description;
    std::optional<cl::Device> selectedDevice;
    for (auto& platform : platforms)
    {
        std::vector<cl::Device> devices;
        try
        {
            platform.getDevices(CL_DEVICE_TYPE_ALL, &devices);
        }
        catch (cl::Error& err)
        {
            spdlog::get(logger)->error("No OpenCL devices were found, {}", err.what());
        }
        spdlog::get(logger)->info("Found OpenCL devices: {}", devices.size());
        if (devices.size() == 0)
        {
            continue;
        }

        size_t maxPowerIndex = 0;
        spdlog::get(logger)->trace("Selecting OpenCL device (total: {})...", devices.size());
        for (auto& device : devices)
        {
            size_t powerIndex = 0;
            try
            {
                description = m_clInfo->GetDeviceDescription(device);
                auto deviceID = m_clInfo->GetDeviceID(device);
                if (identifier == deviceID)
                {
                    selectedDevice = device;
                    break;
                }
                else
                {
                    powerIndex = GetDevicePowerIndex(device);
                }
            }
            catch (cl::Error& err)
            {
                spdlog::get(logger)->error("Failed to get info for a device, {}", err.what());
                continue;
            }

            if (powerIndex > maxPowerIndex)
            {
                maxPowerIndex = powerIndex;
                selectedDevice = device;
            }
        }
    }
    m_device = selectedDevice;
    spdlog::get(logger)->info("Selected OpenCL device: {}", description);
}

std::string Diagnostics::BuildSource(const std::string& source) const
{
    if (!m_device.has_value())
    {
        throw std::runtime_error("missing OpenCL device");
    }

    std::vector<cl::Device> ds {*m_device};
    cl::Context context(ds, NULL, NULL, NULL);
    cl::Program program;
    try
    {
        spdlog::get(logger)->debug("Building program with options: {}", m_BuildOptions);
        program = cl::Program(context, source, false);
        program.build(ds, m_BuildOptions.c_str());
    }
    catch (cl::Error& err)
    {
        if (err.err() != CL_BUILD_PROGRAM_FAILURE)
        {
            spdlog::get(logger)->error("Failed to build program, error, {}", err.what());
        }
    }

    std::string build_log;

    try
    {
        program.getBuildInfo(*m_device, CL_PROGRAM_BUILD_LOG, &build_log);
    }
    catch (cl::Error& err)
    {
        spdlog::get(logger)->error("Failed get build info, error, {}", err.what());
    }

    return build_log;
}

nlohmann::json Diagnostics::BuildDiagnostics(const std::string& buildLog, const std::string& name)
{
    std::smatch matches;
    auto errorLines = utils::SplitString(buildLog, "\n");
    json diagnostics;
    int count = 0;
    for (auto errLine : errorLines)
    {
        std::regex_search(errLine, matches, m_regex);
        if (matches.size() != 7)
            continue;

        if (count++ > m_maxNumberOfProblems)
        {
            spdlog::get(logger)->info("Maximum number of problems reached, other problems will be slipped");
            break;
        }

        auto [source, line, col, severity, message] = ParseOutput(matches);
        json diagnostic;
        json range {
            {"start",
             {
                 {"line", line},
                 {"character", col},
             }},
            {"end",
             {
                 {"line", line},
                 {"character", col},
             }},
        };
        diagnostic["source"] = name.empty() ? source : name;
        diagnostic["range"] = range;
        diagnostic["severity"] = severity;
        diagnostic["message"] = message;
        diagnostics.emplace_back(diagnostic);
    }
    return diagnostics;
}

nlohmann::json Diagnostics::Get(const Source& source)
{
    if (!m_device.has_value())
    {
        throw std::runtime_error("missing OpenCL device");
    }

    spdlog::get(logger)->trace("Getting diagnostics...");
    std::string buildLog;
    std::string srcName;

    if (!source.filePath.empty())
    {
        auto filePath = std::filesystem::path(source.filePath).string();
        srcName = std::filesystem::path(filePath).filename().string();
    }

    buildLog = BuildSource(source.text);
    utils::RemoveNullTerminator(buildLog);
    spdlog::get(logger)->trace("BuildLog:\n", buildLog);

    return BuildDiagnostics(buildLog, srcName);
}

void Diagnostics::SetBuildOptions(const json& options)
{
    try
    {
        std::string args;
        for (auto option : options)
        {
            args.append(option.get<std::string>());
            args.append(" ");
        }
        m_BuildOptions = std::move(args);
        spdlog::get(logger)->trace("Set build options, {}", m_BuildOptions);
    }
    catch (std::exception& e)
    {
        spdlog::get(logger)->error("Failed to parse build options, {}", e.what());
    }
}

void Diagnostics::SetMaxProblemsCount(int maxNumberOfProblems)
{
    spdlog::get(logger)->trace("Set max number of problems: {}", maxNumberOfProblems);
    m_maxNumberOfProblems = maxNumberOfProblems;
}

std::shared_ptr<IDiagnostics> CreateDiagnostics(std::shared_ptr<ICLInfo> clInfo)
{
    return std::make_shared<Diagnostics>(std::move(clInfo));
}

} // namespace ocls
