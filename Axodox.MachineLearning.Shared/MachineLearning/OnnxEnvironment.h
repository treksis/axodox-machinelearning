#pragma once
#include "pch.h"

namespace Axodox::MachineLearning
{
  class AXODOX_MACHINELEARNING_API OnnxEnvironment
  {
  public:
    OnnxEnvironment(const std::filesystem::path& rootPath);

    const std::filesystem::path& RootPath() const;
    Ort::Env& Environment();
    Ort::MemoryInfo& MemoryInfo();
    Ort::SessionOptions DefaultSessionOptions();
    Ort::SessionOptions CpuSessionOptions();

    Ort::Session CreateSession(const std::filesystem::path& modelPath);
    Ort::Session CreateOptimizedSession(const std::filesystem::path& modelPath);

  private:
    std::filesystem::path _rootPath;
    Ort::Env _environment;
    Ort::MemoryInfo _memoryInfo;
    void OrtLoggingFunction(OrtLoggingLevel severity, const char* category, const char* logid, const char* code_location, const char* message);
    static void ORT_API_CALL OrtLoggingFunctionCallback(void* param, OrtLoggingLevel severity, const char* category, const char* logid, const char* code_location, const char* message);
#ifdef PLATFORM_WINDOWS
    winrt::Windows::Foundation::Diagnostics::LoggingChannel _logChannel = nullptr;
    static winrt::Windows::Foundation::Diagnostics::FileLoggingSession _loggingSession;
#endif
  };
}
