//
//  jsonrpc.cpp
//  opencl-language-server
//
//  Created by Ilya Shoshin (Galarius) on 7/16/21.
//

#include "jsonrpc.hpp"
#include <spdlog/spdlog.h>

using namespace nlohmann;

namespace ocls {

namespace {
constexpr char logger[] = "jrpc";
constexpr char LE[] = "\r\n";
} // namespace

void JsonRPC::RegisterMethodCallback(const std::string& method, InputCallbackFunc&& func)
{
    spdlog::get(logger)->trace("Set callback for method: {}", method);
    m_callbacks[method] = std::move(func);
}

void JsonRPC::RegisterInputCallback(InputCallbackFunc&& func)
{
    spdlog::get(logger)->trace("Set callback for client responds");
    m_respondCallback = std::move(func);
}

void JsonRPC::RegisterOutputCallback(OutputCallbackFunc&& func)
{
    spdlog::get(logger)->trace("Set output callback");
    m_outputCallback = std::move(func);
}

void JsonRPC::Consume(char c)
{
    m_buffer += c;
    if (m_validHeader)
    {
        if (m_buffer.length() != m_contentLength)
            return;
        try
        {
            spdlog::get(logger)->debug("");
            spdlog::get(logger)->debug(">>>>>>>>>>>>>>>>");
            for (auto& header : m_headers)
                spdlog::get(logger)->debug(header.first, ": ", header.second);
            spdlog::get(logger)->debug(m_buffer);
            spdlog::get(logger)->debug(">>>>>>>>>>>>>>>>");
            spdlog::get(logger)->debug("");

            m_body = json::parse(m_buffer);
            const auto method = m_body["method"];
            if (method.is_string())
            {
                m_method = method.get<std::string>();
                if (m_method == "initialize")
                {
                    OnInitialize();
                }
                else if (!m_initialized)
                {
                    spdlog::get(logger)->error("Unexpected first message: '{}'", m_method);
                    WriteError(ErrorCode::NotInitialized, "Server was not initialized.");
                    return;
                }
                else if (m_method == "$/setTrace")
                {
                    OnTracingChanged(m_body);
                }
                FireMethodCallback();
            }
            else
            {
                FireRespondCallback();
            }
            m_isProcessing = false;
        }
        catch (std::exception& e)
        {
            spdlog::get(logger)->error("Failed to parse request with reason: '{}'\n{}", e.what(), "\n", m_buffer);
            m_buffer.clear();
            WriteError(ErrorCode::ParseError, "Failed to parse request");
            return;
        }
    }
    else
    {
        if (ReadHeader())
            m_buffer.clear();

        if (m_buffer == LE)
        {
            m_buffer.clear();
            m_validHeader = m_contentLength > 0;
            if (m_validHeader)
            {
                m_buffer.reserve(m_contentLength);
            }
            else
            {
                WriteError(ErrorCode::InvalidRequest, "Invalid content length");
            }
        }
    }
}

bool JsonRPC::IsReady() const
{
    return !m_isProcessing;
}

void JsonRPC::Write(const json& data) const
{
    assert(m_outputCallback);

    std::string message;
    try
    {
        json body = data;
        body.emplace("jsonrpc", "2.0");
        std::string content = body.dump();
        message.append(std::string("Content-Length: ") + std::to_string(content.size()) + LE);
        message.append(std::string("Content-Type: application/vscode-jsonrpc;charset=utf-8") + LE);
        message.append(LE);
        message.append(content);

        spdlog::get(logger)->debug("");
        spdlog::get(logger)->debug("<<<<<<<<<<<<<<<<");
        spdlog::get(logger)->debug(message);
        spdlog::get(logger)->debug("<<<<<<<<<<<<<<<<");
        spdlog::get(logger)->debug("");

        m_outputCallback(message);
    }
    catch (std::exception& err)
    {
        spdlog::get(logger)->error("Failed to write message: '{}', error: {}", message, err.what());
    }
}

void JsonRPC::Reset()
{
    m_method = std::string();
    m_buffer.clear();
    m_body.clear();
    m_headers.clear();
    m_validHeader = false;
    m_contentLength = 0;
    m_isProcessing = true;
}

void JsonRPC::LogTrace(const std::string& message, const std::string& verbose)
{
    if (!m_tracing)
    {
        spdlog::get(logger)->debug("JRPC tracing is disabled");
        spdlog::get(logger)->trace("The message was: '{}', verbose: {}", message, verbose);
        return;
    }

    if (!verbose.empty() && !m_verbosity)
    {
        spdlog::get(logger)->debug("JRPC verbose tracing is disabled");
        spdlog::get(logger)->trace("The verbose message was: {}", verbose);
        return;
    }

    Write(
        json({{"method", "$/logTrace"}, {"params", {{"message", message}, {"verbose", m_verbosity ? verbose : ""}}}}));
}

void JsonRPC::OnInitialize()
{
    try
    {
        const auto traceValue = m_body["params"]["trace"].get<std::string>();
        m_tracing = traceValue != "off";
        m_verbosity = traceValue == "verbose";
        m_initialized = true;
        spdlog::get(logger)->debug(
            "Tracing options: is verbose: {}, is on: {}", m_verbosity ? "yes" : "no", m_tracing ? "yes" : "no");
    }
    catch (std::exception& err)
    {
        spdlog::get(logger)->error("Failed to read tracing options, {}", err.what());
    }
}

void JsonRPC::OnTracingChanged(const json& data)
{
    try
    {
        const auto traceValue = data["params"]["value"].get<std::string>();
        m_tracing = traceValue != "off";
        m_verbosity = traceValue == "verbose";
        spdlog::get(logger)->debug(
            "Tracing options were changed, is verbose: {}, is on: {}",
            m_verbosity ? "yes" : "no",
            m_tracing ? "yes" : "no");
    }
    catch (std::exception& err)
    {
        spdlog::get(logger)->error("Failed to read tracing options, {}", err.what());
    }
}

bool JsonRPC::ReadHeader()
{
    std::sregex_iterator next(m_buffer.begin(), m_buffer.end(), m_headerRegex);
    std::sregex_iterator end;
    const bool found = std::distance(next, end) > 0;
    while (next != end)
    {
        std::smatch match = *next;
        std::string key = match.str(1);
        std::string value = match.str(2);
        if (key == "Content-Length")
            m_contentLength = std::stoi(value);
        m_headers[key] = value;
        ++next;
    }
    return found;
}

void JsonRPC::FireRespondCallback()
{
    if (m_respondCallback)
    {
        spdlog::get(logger)->debug("Calling handler for a client respond");
        m_respondCallback(m_body);
    }
}

void JsonRPC::FireMethodCallback()
{
    assert(m_outputCallback);
    auto callback = m_callbacks.find(m_method);
    if (callback == m_callbacks.end())
    {
        const bool isRequest = m_body["params"]["id"] != nullptr;
        const bool mustRespond = isRequest || m_method.rfind("$/", 0) == std::string::npos;
        spdlog::get(logger)->debug(
            "Got request: {}, respond is required: {}", isRequest ? "yes" : "no", mustRespond ? "yes" : "no");
        if (mustRespond)
        {
            WriteError(ErrorCode::MethodNotFound, "Method '" + m_method + "' is not supported.");
        }
    }
    else
    {
        try
        {
            spdlog::get(logger)->debug("Calling handler for method: '{}'", m_method);
            callback->second(m_body);
        }
        catch (std::exception& err)
        {
            spdlog::get(logger)->error("Failed to handle method '{}', err: {}", m_method, err.what());
        }
    }
}

void JsonRPC::WriteError(JsonRPC::ErrorCode errorCode, const std::string& message) const
{
    spdlog::get(logger)->trace("Reporting error: '{}' ({})", message, static_cast<int>(errorCode));
    json obj = {
        {"error",
         {
             {"code", static_cast<int>(errorCode)},
             {"message", message},
         }}};
    Write(obj);
}

} // namespace ocls
