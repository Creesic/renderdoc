/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "apple_trace_session.h"

#import <Foundation/Foundation.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include "common/formatting.h"
#include "os/os_specific.h"

namespace
{
static constexpr const char *GPUDebugPath = "/usr/bin/gpudebug";
static constexpr size_t MaxToolOutput = 64ULL * 1024ULL * 1024ULL;
static constexpr size_t MaxRawListings = 256ULL * 1024ULL * 1024ULL;
static constexpr uint64_t MaxFetchedResource = 1024ULL * 1024ULL * 1024ULL;
static constexpr size_t MaxNodes = 1024ULL * 1024ULL;
static constexpr uint32_t MaxDepth = 64;

static bool RunningUnderRosetta()
{
#if defined(__x86_64__)
  int translated = 0;
  size_t translatedSize = sizeof(translated);
  return sysctlbyname("sysctl.proc_translated", &translated, &translatedSize, NULL, 0) == 0 &&
         translated == 1;
#else
  return false;
#endif
}

static void AppendPipeOutput(int &fd, rdcstr &output, bool &overflow)
{
  if(fd < 0)
    return;

  char buf[16384];
  for(;;)
  {
    ssize_t count = read(fd, buf, sizeof(buf));
    if(count > 0)
    {
      if(output.size() + (size_t)count > MaxToolOutput)
      {
        size_t remaining = output.size() < MaxToolOutput ? MaxToolOutput - output.size() : 0;
        output.append(buf, remaining);
        overflow = true;
      }
      else
      {
        output.append(buf, (size_t)count);
      }
      continue;
    }

    if(count == 0)
    {
      close(fd);
      fd = -1;
    }
    else if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
      close(fd);
      fd = -1;
    }
    return;
  }
}

class GPUDebugToolRunner final : public AppleTraceToolRunner
{
public:
  explicit GPUDebugToolRunner(const char *toolPath = GPUDebugPath) : m_ToolPath(toolPath) {}

  AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                           const std::atomic<bool> &cancelled) override
  {
    AppleTraceToolResult result;
    if(cancelled.load())
    {
      result.cancelled = true;
      return result;
    }

    // gpudebug's replay service is native on Apple Silicon. When the x86_64 QRenderDoc build is
    // running under Rosetta, explicitly launch gpudebug's arm64 slice instead of inheriting the
    // translated architecture preference. The x86 slice can browse, but its replay XPC connection
    // intermittently fails before resources can be fetched.
    rdcstr executable = m_ToolPath;
    rdcarray<rdcstr> launchArguments;
    if(strcmp(m_ToolPath, GPUDebugPath) == 0 && RunningUnderRosetta())
    {
      executable = "/usr/bin/arch";
      launchArguments = {"-arm64", GPUDebugPath};
    }
    launchArguments.append(arguments);

    // Construct argv before fork. The child may be created from a multi-threaded replay process,
    // so it must only call async-signal-safe functions before exec.
    std::vector<char *> argv;
    argv.reserve(launchArguments.size() + 2);
    argv.push_back(const_cast<char *>(executable.c_str()));
    for(const rdcstr &argument : launchArguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(NULL);

    int nullInput = open("/dev/null", O_RDONLY);
    if(nullInput < 0)
    {
      result.standardError = StringFormat::Fmt("Could not open gpudebug stdin: %s", strerror(errno));
      return result;
    }

    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    if(pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0)
    {
      close(nullInput);
      if(stdoutPipe[0] >= 0)
      {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
      }
      if(stderrPipe[0] >= 0)
      {
        close(stderrPipe[0]);
        close(stderrPipe[1]);
      }
      result.standardError =
          StringFormat::Fmt("Could not create gpudebug pipes: %s", strerror(errno));
      return result;
    }

    pid_t child = fork();
    if(child == 0)
    {
      if(dup2(nullInput, STDIN_FILENO) < 0 || dup2(stdoutPipe[1], STDOUT_FILENO) < 0 ||
         dup2(stderrPipe[1], STDERR_FILENO) < 0)
        _exit(126);
      close(nullInput);
      close(stdoutPipe[0]);
      close(stdoutPipe[1]);
      close(stderrPipe[0]);
      close(stderrPipe[1]);

      execv(executable.c_str(), argv.data());
      _exit(127);
    }

    close(nullInput);
    close(stdoutPipe[1]);
    close(stderrPipe[1]);

    if(child < 0)
    {
      close(stdoutPipe[0]);
      close(stderrPipe[0]);
      result.standardError = StringFormat::Fmt("Could not launch gpudebug: %s", strerror(errno));
      return result;
    }

    fcntl(stdoutPipe[0], F_SETFL, fcntl(stdoutPipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(stderrPipe[0], F_SETFL, fcntl(stderrPipe[0], F_GETFL) | O_NONBLOCK);

    int stdoutFD = stdoutPipe[0];
    int stderrFD = stderrPipe[0];
    int status = 0;
    bool childRunning = true;
    bool overflow = false;
    bool terminateSent = false;
    bool killSent = false;
    bool childExitObserved = false;
    auto start = std::chrono::steady_clock::now();
    auto terminateTime = start;
    auto childExitTime = start;

    while(childRunning || stdoutFD >= 0 || stderrFD >= 0)
    {
      AppendPipeOutput(stdoutFD, result.standardOutput, overflow);
      AppendPipeOutput(stderrFD, result.standardError, overflow);

      if(childRunning)
      {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if(waited == child || (waited < 0 && errno == ECHILD))
        {
          childRunning = false;
          childExitObserved = true;
          childExitTime = std::chrono::steady_clock::now();
        }
      }

      auto now = std::chrono::steady_clock::now();
      uint64_t elapsedMS =
          (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if(cancelled.load())
        result.cancelled = true;
      if(timeoutMS > 0 && elapsedMS >= timeoutMS)
        result.timedOut = true;

      if((result.cancelled || result.timedOut || overflow) && childRunning && !terminateSent)
      {
        kill(child, SIGTERM);
        terminateSent = true;
        terminateTime = now;
      }
      else if(terminateSent && childRunning && !killSent &&
              std::chrono::duration_cast<std::chrono::milliseconds>(now - terminateTime).count() >=
                  250)
      {
        kill(child, SIGKILL);
        killSent = true;
      }

      // A correctly behaved child closes both pipes at exec-process exit. Do not allow inherited
      // descriptors in an unexpected descendant to keep this call alive indefinitely.
      if(childExitObserved &&
         std::chrono::duration_cast<std::chrono::milliseconds>(now - childExitTime).count() >= 250)
      {
        if(stdoutFD >= 0)
        {
          close(stdoutFD);
          stdoutFD = -1;
        }
        if(stderrFD >= 0)
        {
          close(stderrFD);
          stderrFD = -1;
        }
      }

      if(!childRunning && stdoutFD < 0 && stderrFD < 0)
        break;

      struct pollfd descriptors[2];
      nfds_t count = 0;
      if(stdoutFD >= 0)
        descriptors[count++] = {stdoutFD, POLLIN | POLLHUP, 0};
      if(stderrFD >= 0)
        descriptors[count++] = {stderrFD, POLLIN | POLLHUP, 0};
      if(count > 0)
        poll(descriptors, count, 20);
      else
        usleep(20000);
    }

    if(childRunning)
    {
      kill(child, SIGKILL);
      while(waitpid(child, &status, 0) < 0 && errno == EINTR)
      {
      }
    }

    if(stdoutFD >= 0)
      close(stdoutFD);
    if(stderrFD >= 0)
      close(stderrFD);

    if(overflow)
    {
      result.exitCode = -2;
      result.standardError += "\ngpudebug output exceeded the 64 MiB safety limit";
    }
    else if(WIFEXITED(status))
    {
      result.exitCode = WEXITSTATUS(status);
    }
    else if(WIFSIGNALED(status))
    {
      result.exitCode = 128 + WTERMSIG(status);
    }

    return result;
  }

private:
  const char *m_ToolPath;
};

static rdcstr StringFromObject(id object)
{
  if(object == nil || object == [NSNull null])
    return {};
  if([object isKindOfClass:[NSString class]])
  {
    const char *utf8 = [(NSString *)object UTF8String];
    return utf8 ? rdcstr(utf8) : rdcstr();
  }
  if([object isKindOfClass:[NSNumber class]])
  {
    const char *utf8 = [[(NSNumber *)object stringValue] UTF8String];
    return utf8 ? rdcstr(utf8) : rdcstr();
  }
  return {};
}

static NSDictionary *ParseJSONObject(const rdcstr &json, rdcstr &error)
{
  NSData *data = [NSData dataWithBytes:json.data() length:json.size()];
  NSError *jsonError = nil;
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jsonError];
  if(![object isKindOfClass:[NSDictionary class]])
  {
    error = jsonError ? StringFromObject([jsonError localizedDescription])
                      : rdcstr("root JSON value is not an object");
    return nil;
  }
  return (NSDictionary *)object;
}

static RDResult ToolFailure(const AppleTraceToolResult &result, const rdcstr &operation)
{
  if(result.cancelled)
    return RDResult(ResultCode::APIReplayFailed,
                    StringFormat::Fmt("gpudebug %s was cancelled", operation.c_str()));
  if(result.timedOut)
    return RDResult(ResultCode::APIReplayFailed,
                    StringFormat::Fmt("gpudebug %s timed out", operation.c_str()));
  if(result.exitCode != 0)
  {
    rdcstr detail = result.standardError.trimmed();
    if(detail.empty())
      detail = result.standardOutput.trimmed();
    if(detail.size() > 4096)
      detail.resize(4096);
    return RDResult(ResultCode::APIReplayFailed,
                    StringFormat::Fmt("gpudebug %s failed with exit code %d: %s", operation.c_str(),
                                      result.exitCode, detail.c_str()));
  }
  return ResultCode::Succeeded;
}

static bool ParseSessionID(const rdcstr &output, uint64_t &sessionID)
{
  int32_t begin = output.find("Session ");
  if(begin < 0)
    return false;
  begin += 8;
  int32_t end = output.find(" created.", begin);
  if(end < 0 || end == begin)
    return false;
  for(int32_t i = begin; i < end; i++)
    if(output[i] < '0' || output[i] > '9')
      return false;
  sessionID = strtoull(output.substr(begin, end - begin).c_str(), NULL, 10);
  return sessionID != 0;
}

static uint64_t StableHash(const rdcstr &key)
{
  uint64_t hash = 14695981039346656037ULL;
  for(char c : key)
  {
    hash ^= (byte)c;
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

static rdcstr FindLabel(const rdcarray<rdcstr> &values)
{
  for(const rdcstr &value : values)
    if(value.size() >= 2 && value.front() == '"' && value.back() == '"')
      return value.substr(1, value.size() - 2);
  return values.empty() ? rdcstr() : values[0];
}

static rdcstr FindObjectName(const rdcarray<rdcstr> &values)
{
  for(const rdcstr &value : values)
  {
    int32_t at = value.find('@');
    if(at < 0)
      continue;
    int32_t end = at + 1;
    while((size_t)end < value.size() &&
          ((value[end] >= 'a' && value[end] <= 'z') || (value[end] >= 'A' && value[end] <= 'Z') ||
           (value[end] >= '0' && value[end] <= '9') || value[end] == '_'))
      end++;
    if(end > at + 1)
      return value.substr(at + 1, end - at - 1);
  }
  return {};
}

static uint64_t FindByteSize(const rdcarray<rdcstr> &values)
{
  for(const rdcstr &value : values)
  {
    int32_t suffix = value.find(" bytes");
    if(suffix < 0)
      continue;
    int32_t begin = suffix;
    while(begin > 0 && value[begin - 1] >= '0' && value[begin - 1] <= '9')
      begin--;
    if(begin < suffix)
      return strtoull(value.substr(begin, suffix - begin).c_str(), NULL, 10);
  }
  return 0;
}

static bool ReportsNoChildren(const rdcarray<rdcstr> &values)
{
  for(const rdcstr &value : values)
    if(value.beginsWith("0 "))
      return true;
  return false;
}

static MetalTrace::NodeKind ClassifyNode(const rdcstr &path, const rdcstr &name)
{
  if(path.beginsWith("/resources/"))
  {
    rdcstr category = path.substr(11);
    int32_t slash = category.find('/');
    if(slash >= 0)
      category.resize(slash);
    if(category == "buffers")
      return MetalTrace::NodeKind::Buffer;
    if(category == "textures")
      return MetalTrace::NodeKind::Texture;
    if(category == "libraries")
      return MetalTrace::NodeKind::Library;
    if(category == "shaders" || category == "functions")
      return MetalTrace::NodeKind::Shader;
    if(category == "render_pipelines")
      return MetalTrace::NodeKind::RenderPipeline;
    if(category == "compute_pipelines")
      return MetalTrace::NodeKind::ComputePipeline;
    if(category == "depth_stencil")
      return MetalTrace::NodeKind::DepthStencil;
    if(category == "samplers")
      return MetalTrace::NodeKind::Sampler;
    if(category == "command_queues")
      return MetalTrace::NodeKind::CommandQueue;
    if(category == "residency_sets")
      return MetalTrace::NodeKind::ResidencySet;
    return MetalTrace::NodeKind::Unknown;
  }

  if(name.beginsWith("cb"))
    return MetalTrace::NodeKind::CommandBuffer;
  if(name.beginsWith("grp"))
    return MetalTrace::NodeKind::DebugGroup;
  if(name.beginsWith("re"))
    return MetalTrace::NodeKind::RenderEncoder;
  if(name.beginsWith("ce"))
    return MetalTrace::NodeKind::ComputeEncoder;
  if(name.beginsWith("be"))
    return MetalTrace::NodeKind::BlitEncoder;
  if(name.beginsWith("draw"))
    return MetalTrace::NodeKind::Draw;
  if(name.beginsWith("dispatch"))
    return MetalTrace::NodeKind::Dispatch;
  return MetalTrace::NodeKind::Binding;
}

static bool ShouldWalkChildren(const MetalTrace::Node &node)
{
  if(!node.canGo)
    return false;

  if(node.path.beginsWith("/resources/"))
  {
    // Resource categories are needed to enumerate the catalog. Descendants such as library
    // sources are outside the current read-only resource/action surface.
    return node.path.find('/', 11) < 0;
  }

  if(!node.path.beginsWith("/commands/"))
    return false;

  switch(node.kind)
  {
    case MetalTrace::NodeKind::CommandBuffer:
    case MetalTrace::NodeKind::DebugGroup:
    case MetalTrace::NodeKind::RenderEncoder:
    case MetalTrace::NodeKind::ComputeEncoder:
    case MetalTrace::NodeKind::BlitEncoder:
    case MetalTrace::NodeKind::Draw:
    case MetalTrace::NodeKind::Dispatch: return true;
    default: break;
  }

  // Stage bindings directly below a draw/dispatch contain the resources needed to populate the
  // Texture Viewer's Inputs strip. Their children are terminal buffer/texture leaves except for
  // shader source groups, which remain outside this inspection profile.
  if(node.kind == MetalTrace::NodeKind::Binding &&
     (node.name == "vertex" || node.name == "fragment" || node.name == "compute"))
    return true;

  return false;
}

static bool IsFetchArtifactPath(const rdcstr &path)
{
  return path.beginsWith("/") && !path.contains("/../") && !path.contains("/./") &&
         (path.contains("/gpudebug_out/") || path.contains("/gpudebug_out."));
}

static bool IsTransientReplayerFailure(const rdcstr &message)
{
  return message.contains("XPC error") &&
         (message.contains("Connection invalid") || message.contains("Connection interrupted"));
}

class GPUDebugAppleTraceSession final : public AppleTraceSession
{
public:
  GPUDebugAppleTraceSession(const rdcstr &tracePath, AppleTraceToolRunner *ownedRunner)
      : m_TracePath(tracePath), m_Runner(ownedRunner ? ownedRunner : new GPUDebugToolRunner)
  {
  }

  ~GPUDebugAppleTraceSession() override
  {
    Shutdown();
    delete m_Runner;
  }

  RDResult Normalise(MetalTrace::Index &index) override
  {
    std::lock_guard<std::mutex> lock(m_Lock);
    if(m_Shutdown)
      return RDResult(ResultCode::APIReplayFailed, "gpudebug session is shut down");

    m_ReplayerRestartAttempts = 0;
    RDResult result = EnsureSession();
    if(result != ResultCode::Succeeded)
      return result;

    index = {};
    index.toolVersion = m_ToolVersion;
    m_IDKeys.clear();
    m_RawListingBytes = 0;

    result = Walk("/commands", 0, index);
    if(result == ResultCode::Succeeded)
      result = Walk("/resources", 0, index);
    if(result != ResultCode::Succeeded)
      return result;

    // Static browsing remains valid when gpudebug cannot replay the trace on this device. Probe
    // replay readiness only after the command/resource trees have been captured, and downgrade
    // fetch capabilities instead of rejecting otherwise inspectable traces.
    result = WaitForReplayerReady(false);
    if(result != ResultCode::Succeeded)
      return result;
    if(!m_ReplayerReady)
    {
      index.bufferFetchUnavailableReason = m_ReplayerUnavailableReason;
      for(MetalTrace::Node &node : index.nodes)
        node.canFetch = false;
    }

    for(const MetalTrace::Node &node : index.nodes)
    {
      if(index.actionName.empty() &&
         (node.kind == MetalTrace::NodeKind::Draw || node.kind == MetalTrace::NodeKind::Dispatch))
        index.actionName = node.label.empty() ? node.name : node.label;
      if(index.resourceName.empty() && node.kind == MetalTrace::NodeKind::Buffer)
        index.resourceName = node.label.empty() ? node.name : node.label;
    }
    if(index.actionName.empty())
      index.actionName = "Apple GPU Trace";
    if(index.resourceName.empty())
      index.resourceName = "Apple GPU Trace Resource";

    return ResultCode::Succeeded;
  }

  RDResult Fetch(uint64_t stableId, const rdcstr &path, bytebuf &data) override
  {
    std::lock_guard<std::mutex> lock(m_Lock);
    data.clear();
    if(m_Shutdown)
      return RDResult(ResultCode::APIReplayFailed, "gpudebug session is shut down");

    auto cached = m_Cache.find(stableId);
    if(cached != m_Cache.end())
    {
      data = cached->second;
      return ResultCode::Succeeded;
    }

    RDResult result = EnsureSession();
    if(result != ResultCode::Succeeded)
      return result;
    result = WaitForReplayerReady(true);
    if(result != ResultCode::Succeeded)
      return result;

    rdcstr json;
    result = RunCommand("fetch " + path, 30000, json);
    if(result != ResultCode::Succeeded)
      return result;

    @autoreleasepool
    {
      rdcstr parseError;
      NSDictionary *object = ParseJSONObject(json, parseError);
      if(object == nil)
        return RDResult(
            ResultCode::APIDataCorrupted,
            StringFormat::Fmt("gpudebug fetch returned malformed JSON: %s", parseError.c_str()));

      rdcstr artifactPath = StringFromObject([object objectForKey:@"path"]);
      NSNumber *reportedSize = [object objectForKey:@"size"];
      if(artifactPath.empty() || ![reportedSize isKindOfClass:[NSNumber class]] ||
         !IsFetchArtifactPath(artifactPath))
        return RDResult(ResultCode::APIDataCorrupted,
                        "gpudebug fetch did not return a safe resource artifact");

      uint64_t size = [reportedSize unsignedLongLongValue];
      if(size > MaxFetchedResource || !FileIO::exists(artifactPath) ||
         FileIO::GetFileSize(artifactPath) != size)
      {
        if(IsFetchArtifactPath(artifactPath))
          FileIO::Delete(artifactPath);
        return RDResult(ResultCode::APIDataCorrupted,
                        "gpudebug fetch returned an invalid resource artifact size");
      }

      if(size > 0 && !FileIO::ReadAll(artifactPath, data))
      {
        FileIO::Delete(artifactPath);
        data.clear();
        return RDResult(ResultCode::FileIOFailed,
                        "Could not read the resource artifact produced by gpudebug");
      }
      FileIO::Delete(artifactPath);
    }

    m_Cache[stableId] = data;
    return ResultCode::Succeeded;
  }

  void Cancel() override { m_Cancelled.store(true); }

  void Shutdown() override
  {
    m_Cancelled.store(true);
    std::lock_guard<std::mutex> lock(m_Lock);
    if(m_Shutdown)
      return;

    if(m_SessionID != 0)
    {
      std::atomic<bool> notCancelled(false);
      m_Runner->Run({"--json", "-q", "--terminate", ToStr(m_SessionID)}, 5000, notCancelled);
      m_SessionID = 0;
    }
    m_Cache.clear();
    m_Shutdown = true;
  }

private:
  RDResult EnsureSession()
  {
    if(m_Cancelled.load())
      return RDResult(ResultCode::APIReplayFailed, "gpudebug session was cancelled");
    if(m_SessionID != 0)
      return ResultCode::Succeeded;
    if(!FileIO::exists(m_TracePath))
      return RDResult(ResultCode::FileNotFound,
                      StringFormat::Fmt("Apple GPU Trace does not exist: %s", m_TracePath.c_str()));
    if(!FileIO::exists(GPUDebugPath))
      return RDResult(ResultCode::APIUnsupported, "gpudebug is not installed at /usr/bin/gpudebug");

    AppleTraceToolResult version = m_Runner->Run({"--version"}, 5000, m_Cancelled);
    RDResult result = ToolFailure(version, "version probe");
    if(result != ResultCode::Succeeded)
      return result;
    m_ToolVersion = version.standardOutput.trimmed();
    if(!m_ToolVersion.beginsWith("gpudebug 1."))
      return RDResult(ResultCode::APIIncompatibleVersion,
                      StringFormat::Fmt("Unsupported gpudebug version: %s", m_ToolVersion.c_str()));

    AppleTraceToolResult start =
        m_Runner->Run({"--json", "-q", "--timeout", "0", "-t", m_TracePath}, 30000, m_Cancelled);
    result = ToolFailure(start, "session creation");
    if(result != ResultCode::Succeeded)
      return result;
    if(!ParseSessionID(start.standardOutput, m_SessionID))
      return RDResult(ResultCode::APIDataCorrupted,
                      "gpudebug did not return a persistent session identifier");

    m_ReplayerReady = false;
    m_ReplayerStatusFinal = false;
    m_ReplayerUnavailableReason.clear();
    return ResultCode::Succeeded;
  }

  RDResult WaitForReplayerReady(bool required)
  {
    if(m_ReplayerReady)
      return ResultCode::Succeeded;
    if(m_ReplayerStatusFinal)
      return required ? RDResult(ResultCode::APIUnsupported, m_ReplayerUnavailableReason)
                      : RDResult(ResultCode::Succeeded);

    auto readyStart = std::chrono::steady_clock::now();
    for(;;)
    {
      rdcstr statusJSON;
      RDResult result = RunCommand("status", 5000, statusJSON);
      if(result != ResultCode::Succeeded)
      {
        if(m_Cancelled.load())
          return result;
        m_ReplayerUnavailableReason = result.message;
        m_ReplayerStatusFinal = true;
        return required ? result : RDResult(ResultCode::Succeeded);
      }

      @autoreleasepool
      {
        rdcstr parseError;
        NSDictionary *status = ParseJSONObject(statusJSON, parseError);
        NSDictionary *replayer = [status objectForKey:@"replayer"];
        rdcstr state = StringFromObject([replayer objectForKey:@"state"]);
        if(status == nil || ![replayer isKindOfClass:[NSDictionary class]] || state.empty())
        {
          m_ReplayerUnavailableReason =
              StringFormat::Fmt("gpudebug status returned malformed JSON: %s", parseError.c_str());
          m_ReplayerStatusFinal = true;
          return required ? RDResult(ResultCode::APIDataCorrupted, m_ReplayerUnavailableReason)
                          : RDResult(ResultCode::Succeeded);
        }
        if(state == "ready")
        {
          m_ReplayerReady = true;
          m_ReplayerStatusFinal = true;
          return ResultCode::Succeeded;
        }
        if(state != "loading")
        {
          rdcstr message = StringFromObject([replayer objectForKey:@"message"]);
          if(message.empty())
            message = StringFormat::Fmt("entered state '%s'", state.c_str());

          if(IsTransientReplayerFailure(message) && m_ReplayerRestartAttempts < 3)
          {
            m_ReplayerRestartAttempts++;
            result = RestartSession();
            if(result != ResultCode::Succeeded)
              return required ? result : RDResult(ResultCode::Succeeded);
            readyStart = std::chrono::steady_clock::now();
            continue;
          }

          m_ReplayerUnavailableReason =
              StringFormat::Fmt("gpudebug replayer is unavailable: %s", message.c_str());
          m_ReplayerStatusFinal = true;
          return required ? RDResult(ResultCode::APIUnsupported, m_ReplayerUnavailableReason)
                          : RDResult(ResultCode::Succeeded);
        }
      }

      if(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - readyStart)
             .count() >= 30)
      {
        m_ReplayerUnavailableReason = "gpudebug replayer did not become ready within 30 seconds";
        m_ReplayerStatusFinal = true;
        return required ? RDResult(ResultCode::APIReplayFailed, m_ReplayerUnavailableReason)
                        : RDResult(ResultCode::Succeeded);
      }
      usleep(100000);
    }
  }

  RDResult RestartSession()
  {
    if(m_SessionID != 0)
    {
      std::atomic<bool> notCancelled(false);
      m_Runner->Run({"--json", "-q", "--terminate", ToStr(m_SessionID)}, 5000, notCancelled);
      m_SessionID = 0;
    }

    m_ReplayerReady = false;
    m_ReplayerStatusFinal = false;
    m_ReplayerUnavailableReason.clear();
    usleep(m_ReplayerRestartAttempts * 1000000);
    return EnsureSession();
  }

  RDResult RunCommand(const rdcstr &command, uint32_t timeoutMS, rdcstr &json)
  {
    AppleTraceToolResult tool = m_Runner->Run(
        {"--json", "-q", "-s", ToStr(m_SessionID), "-c", command}, timeoutMS, m_Cancelled);
    RDResult result = ToolFailure(tool, command);
    if(result != ResultCode::Succeeded)
      return result;
    json = tool.standardOutput.trimmed();
    if(json.empty())
      return RDResult(ResultCode::APIDataCorrupted,
                      StringFormat::Fmt("gpudebug command '%s' returned no JSON", command.c_str()));
    return ResultCode::Succeeded;
  }

  RDResult Walk(const rdcstr &path, uint32_t depth, MetalTrace::Index &index)
  {
    if(depth > MaxDepth)
      return RDResult(ResultCode::APIDataCorrupted,
                      "gpudebug tree exceeded the maximum traversal depth");

    rdcstr json;
    RDResult result = RunCommand("go " + path, 30000, json);
    if(result != ResultCode::Succeeded)
      return result;

    bool needsFullListing = false;
    @autoreleasepool
    {
      rdcstr parseError;
      NSDictionary *listing = ParseJSONObject(json, parseError);
      NSArray *children = [listing objectForKey:@"children"];
      NSNumber *totalCount = [listing objectForKey:@"totalCount"];
      if([children isKindOfClass:[NSArray class]] && [totalCount isKindOfClass:[NSNumber class]] &&
         [totalCount unsignedLongLongValue] > [children count])
        needsFullListing = true;
    }
    if(needsFullListing)
    {
      result = RunCommand("list --all", 30000, json);
      if(result != ResultCode::Succeeded)
        return result;
    }

    if(m_RawListingBytes + json.size() > MaxRawListings)
      return RDResult(ResultCode::APIDataCorrupted,
                      "gpudebug tree exceeded the normalized listing size limit");
    m_RawListingBytes += json.size();
    index.rawListings.push_back({path, json});

    @autoreleasepool
    {
      rdcstr parseError;
      NSDictionary *listing = ParseJSONObject(json, parseError);
      NSArray *children = [listing objectForKey:@"children"];
      if(listing == nil || ![children isKindOfClass:[NSArray class]])
        return RDResult(ResultCode::APIDataCorrupted,
                        StringFormat::Fmt("gpudebug listing '%s' is malformed: %s", path.c_str(),
                                          parseError.c_str()));

      for(id childObject in children)
      {
        if(![childObject isKindOfClass:[NSDictionary class]] || index.nodes.size() >= MaxNodes)
          return RDResult(ResultCode::APIDataCorrupted,
                          "gpudebug tree contains invalid or excessive child nodes");
        NSDictionary *child = (NSDictionary *)childObject;
        rdcstr name = StringFromObject([child objectForKey:@"name"]);
        if(name.empty() || name.contains("/") || name.contains("\n") || name.contains("\r"))
          return RDResult(ResultCode::APIDataCorrupted,
                          "gpudebug tree contains an invalid child name");

        MetalTrace::Node node;
        node.path = path + "/" + name;
        node.name = name;
        rdcstr actions = StringFromObject([child objectForKey:@"actions"]);
        node.canGo = actions.contains("go");
        node.canInfo = actions.contains("info");
        node.canFetch = actions.contains("fetch");

        id valuesObject = [child objectForKey:@"values"];
        if(valuesObject != nil && valuesObject != [NSNull null] &&
           ![valuesObject isKindOfClass:[NSArray class]])
          return RDResult(ResultCode::APIDataCorrupted,
                          "gpudebug tree contains an invalid values array");
        for(id valueObject in(NSArray *)valuesObject)
        {
          if(valueObject == [NSNull null])
          {
            node.values.push_back({});
          }
          else if([valueObject isKindOfClass:[NSDictionary class]])
          {
            node.values.push_back(
                StringFromObject([(NSDictionary *)valueObject objectForKey:@"value"]));
          }
          else
          {
            return RDResult(ResultCode::APIDataCorrupted,
                            "gpudebug tree contains an invalid value entry");
          }
        }

        node.label = FindLabel(node.values);
        node.objectName = FindObjectName(node.values);
        node.canGo &= !ReportsNoChildren(node.values);
        node.kind = ClassifyNode(node.path, node.name);
        if(node.path.beginsWith("/resources/") && node.path.find('/', 11) >= 0)
          node.objectName = node.name;
        node.byteSize = FindByteSize(node.values);

        rdcstr stableKey =
            node.objectName.empty() ? "path:" + node.path : "object:" + node.objectName;
        node.stableId = StableHash(stableKey);
        auto existing = m_IDKeys.find(node.stableId);
        if(existing != m_IDKeys.end() && existing->second != stableKey)
          return RDResult(ResultCode::APIDataCorrupted,
                          "gpudebug tree produced a stable identifier collision");
        m_IDKeys[node.stableId] = stableKey;
        index.nodes.push_back(node);

        if(ShouldWalkChildren(node))
        {
          result = Walk(node.path, depth + 1, index);
          if(result != ResultCode::Succeeded)
            return result;
        }
      }
    }

    return ResultCode::Succeeded;
  }

  rdcstr m_TracePath;
  AppleTraceToolRunner *m_Runner = NULL;
  std::atomic<bool> m_Cancelled = false;
  std::mutex m_Lock;
  uint64_t m_SessionID = 0;
  bool m_Shutdown = false;
  bool m_ReplayerReady = false;
  bool m_ReplayerStatusFinal = false;
  uint32_t m_ReplayerRestartAttempts = 0;
  rdcstr m_ReplayerUnavailableReason;
  rdcstr m_ToolVersion;
  size_t m_RawListingBytes = 0;
  std::map<uint64_t, rdcstr> m_IDKeys;
  std::map<uint64_t, bytebuf> m_Cache;
};
};    // namespace

AppleTraceSession *CreateGPUDebugAppleTraceSession(const rdcstr &tracePath,
                                                   AppleTraceToolRunner *ownedRunner)
{
  return new GPUDebugAppleTraceSession(tracePath, ownedRunner);
}

#if ENABLED(ENABLE_UNIT_TESTS)

#include <thread>
#include "catch/catch.hpp"

namespace
{
struct FakeRunnerState
{
  uint32_t fetchCalls = 0;
  uint32_t terminateCalls = 0;
  std::atomic<bool> entered = false;
};

class NormalisingFakeRunner : public AppleTraceToolRunner
{
public:
  explicit NormalisingFakeRunner(FakeRunnerState &state) : m_State(state) {}

  AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                           const std::atomic<bool> &cancelled) override
  {
    (void)timeoutMS;
    AppleTraceToolResult result;
    result.exitCode = 0;
    if(arguments == rdcarray<rdcstr>({"--version"}))
    {
      result.standardOutput = "gpudebug 1.0\n";
    }
    else if(arguments.contains("--terminate"))
    {
      m_State.terminateCalls++;
    }
    else if(arguments.contains("-t"))
    {
      result.standardOutput =
          "Session 77 created.\ngpudebug -s 77 -c <command> to send commands.\n";
    }
    else
    {
      rdcstr command = arguments.back();
      if(command == "status")
      {
        result.standardOutput = "{\"replayer\":{\"state\":\"ready\"}}";
      }
      else if(command == "go /commands")
      {
        result.standardOutput =
            "{\"children\":[{\"actions\":\"go\",\"name\":\"cb0\",\"values\":[]}] }";
      }
      else if(command == "go /commands/cb0")
      {
        result.standardOutput =
            "{\"children\":[{\"actions\":\"go, info\",\"name\":\"draw0\","
            "\"values\":[{\"value\":\"Synthetic Draw\"}]}]}";
      }
      else if(command == "go /commands/cb0/draw0")
      {
        result.standardOutput =
            "{\"children\":[{\"actions\":\"go\",\"name\":\"vertex\",\"values\":[]},"
            "{\"actions\":\"info, fetch\",\"name\":\"color0\","
            "\"values\":[{\"value\":\"\\\"Synthetic Target\\\"\"},"
            "{\"value\":\"@tex0 4x4 BGRA8Unorm\"}]}]}";
      }
      else if(command == "go /commands/cb0/draw0/vertex")
      {
        result.standardOutput =
            "{\"children\":[{\"actions\":\"info, fetch\",\"name\":\"tex[2]\","
            "\"values\":[{\"value\":\"\\\"Synthetic Input\\\"\"},"
            "{\"value\":\"@tex1 2x2 RGBA8Unorm\"}]}]}";
      }
      else if(command == "go /resources")
      {
        result.standardOutput =
            "{\"children\":[{\"actions\":\"go\",\"name\":\"buffers\",\"values\":[]},"
            "{\"actions\":\"go\",\"name\":\"textures\",\"values\":[]}]}";
      }
      else if(command == "go /resources/buffers")
      {
        result.standardOutput =
            "{\"children\":[{\"actions\":\"info, fetch\",\"name\":\"buf0\","
            "\"values\":[{\"value\":\"\\\"Synthetic Buffer\\\"\"},{\"value\":\"4 bytes\"}]}]}";
      }
      else if(command == "go /resources/textures")
      {
        result.standardOutput =
            "{\"children\":[{\"actions\":\"info, fetch\",\"name\":\"tex0\","
            "\"values\":[{\"value\":\"\\\"Synthetic Target\\\"\"},"
            "{\"value\":\"4x4 BGRA8Unorm\"}]},"
            "{\"actions\":\"info, fetch\",\"name\":\"tex1\","
            "\"values\":[{\"value\":\"\\\"Synthetic Input\\\"\"},"
            "{\"value\":\"2x2 RGBA8Unorm\"}]}]}";
      }
      else if(command == "fetch /resources/buffers/buf0")
      {
        m_State.fetchCalls++;
        const rdcstr artifact = "/tmp/gpudebug_out.renderdoc-metal-session-test.bin";
        bytebuf bytes = {1, 2, 3, 4};
        FileIO::WriteAll(artifact, bytes);
        result.standardOutput = StringFormat::Fmt("{\"path\":\"%s\",\"size\":4}", artifact.c_str());
      }
      else
      {
        result.exitCode = 2;
        result.standardError = "unexpected fake gpudebug command";
      }
    }
    if(cancelled.load())
      result.cancelled = true;
    return result;
  }

private:
  FakeRunnerState &m_State;
};

class ProbeFailureFakeRunner final : public AppleTraceToolRunner
{
public:
  enum class Mode
  {
    Timeout,
    Crash,
    Incompatible,
  };

  explicit ProbeFailureFakeRunner(Mode mode) : m_Mode(mode) {}
  AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                           const std::atomic<bool> &cancelled) override
  {
    (void)arguments;
    (void)timeoutMS;
    (void)cancelled;
    AppleTraceToolResult result;
    if(m_Mode == Mode::Timeout)
      result.timedOut = true;
    else if(m_Mode == Mode::Crash)
    {
      result.exitCode = 9;
      result.standardError = "synthetic tool crash";
    }
    else
    {
      result.exitCode = 0;
      result.standardOutput = "gpudebug 2.0";
    }
    return result;
  }

private:
  Mode m_Mode;
};

class MalformedFakeRunner final : public NormalisingFakeRunner
{
public:
  explicit MalformedFakeRunner(FakeRunnerState &state) : NormalisingFakeRunner(state) {}
  AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                           const std::atomic<bool> &cancelled) override
  {
    if(!arguments.empty() && arguments.back() == "go /commands")
    {
      AppleTraceToolResult result;
      result.exitCode = 0;
      result.standardOutput = "{not-json";
      return result;
    }
    return NormalisingFakeRunner::Run(arguments, timeoutMS, cancelled);
  }
};

class ReplayerErrorFakeRunner final : public NormalisingFakeRunner
{
public:
  explicit ReplayerErrorFakeRunner(FakeRunnerState &state) : NormalisingFakeRunner(state) {}
  AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                           const std::atomic<bool> &cancelled) override
  {
    if(!arguments.empty() && arguments.back() == "status")
    {
      AppleTraceToolResult result;
      result.exitCode = 0;
      result.standardOutput =
          "{\"replayer\":{\"state\":\"error\",\"message\":\"synthetic XPC failure\"}}";
      return result;
    }
    return NormalisingFakeRunner::Run(arguments, timeoutMS, cancelled);
  }
};

class TransientReplayerErrorFakeRunner final : public NormalisingFakeRunner
{
public:
  explicit TransientReplayerErrorFakeRunner(FakeRunnerState &state)
      : NormalisingFakeRunner(state)
  {
  }
  AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                           const std::atomic<bool> &cancelled) override
  {
    if(!arguments.empty() && arguments.back() == "status" && m_StatusCalls++ == 0)
    {
      AppleTraceToolResult result;
      result.exitCode = 0;
      result.standardOutput =
          "{\"replayer\":{\"state\":\"error\",\"message\":\"failed to load trace: "
          "Encountered an XPC error: Connection invalid\"}}";
      return result;
    }
    return NormalisingFakeRunner::Run(arguments, timeoutMS, cancelled);
  }

private:
  uint32_t m_StatusCalls = 0;
};

class CancellingFakeRunner final : public AppleTraceToolRunner
{
public:
  explicit CancellingFakeRunner(FakeRunnerState &state) : m_State(state) {}
  AppleTraceToolResult Run(const rdcarray<rdcstr> &arguments, uint32_t timeoutMS,
                           const std::atomic<bool> &cancelled) override
  {
    (void)arguments;
    (void)timeoutMS;
    m_State.entered.store(true);
    while(!cancelled.load())
      usleep(1000);
    AppleTraceToolResult result;
    result.cancelled = true;
    return result;
  }

private:
  FakeRunnerState &m_State;
};
};    // namespace

TEST_CASE("Apple GPU Trace session normalizes, caches, and shuts down", "[metal][apple-trace]")
{
  const rdcstr tracePath = "/tmp/renderdoc-metal-session-normalise.gputrace";
  REQUIRE(FileIO::WriteAll(tracePath, "test"_lit));
  FakeRunnerState state;
  AppleTraceSession *session =
      CreateGPUDebugAppleTraceSession(tracePath, new NormalisingFakeRunner(state));

  MetalTrace::Index index;
  REQUIRE(session->Normalise(index).code == ResultCode::Succeeded);
  CHECK(index.toolVersion == "gpudebug 1.0");
  REQUIRE(index.nodes.size() == 10);
  CHECK((uint32_t)index.nodes[0].kind == (uint32_t)MetalTrace::NodeKind::CommandBuffer);
  CHECK((uint32_t)index.nodes[1].kind == (uint32_t)MetalTrace::NodeKind::Draw);
  CHECK(index.nodes[2].name == "vertex");
  CHECK(index.nodes[3].name == "tex[2]");
  CHECK(index.nodes[3].objectName == "tex1");
  CHECK(index.nodes[4].name == "color0");
  CHECK(index.nodes[4].objectName == "tex0");
  CHECK((uint32_t)index.nodes[6].kind == (uint32_t)MetalTrace::NodeKind::Buffer);
  CHECK(index.nodes[6].label == "Synthetic Buffer");
  CHECK(index.nodes[6].byteSize == 4);
  CHECK(index.rawListings.size() == 7);

  bytebuf first, second;
  REQUIRE(session->Fetch(index.nodes[6].stableId, index.nodes[6].path, first).code ==
          ResultCode::Succeeded);
  REQUIRE(session->Fetch(index.nodes[6].stableId, index.nodes[6].path, second).code ==
          ResultCode::Succeeded);
  CHECK(first == bytebuf({1, 2, 3, 4}));
  CHECK(second == first);
  CHECK(state.fetchCalls == 1);
  CHECK_FALSE(FileIO::exists("/tmp/gpudebug_out.renderdoc-metal-session-test.bin"));

  session->Shutdown();
  session->Shutdown();
  CHECK(state.terminateCalls == 1);
  delete session;
  FileIO::Delete(tracePath);
}

TEST_CASE("Apple GPU Trace session cancellation is observable", "[metal][apple-trace]")
{
  const rdcstr tracePath = "/tmp/renderdoc-metal-session-cancel.gputrace";
  REQUIRE(FileIO::WriteAll(tracePath, "test"_lit));
  FakeRunnerState state;
  AppleTraceSession *session =
      CreateGPUDebugAppleTraceSession(tracePath, new CancellingFakeRunner(state));
  MetalTrace::Index index;
  RDResult result;
  std::thread worker([&]() { result = session->Normalise(index); });
  while(!state.entered.load())
    usleep(1000);
  session->Cancel();
  worker.join();
  CHECK(result.code == ResultCode::APIReplayFailed);
  CHECK(rdcstr(result.message).contains("cancelled"));
  session->Shutdown();
  delete session;
  FileIO::Delete(tracePath);
}

TEST_CASE("Apple GPU Trace session preserves static browsing when replay is unavailable",
          "[metal][apple-trace]")
{
  const rdcstr tracePath = "/tmp/renderdoc-metal-session-static-only.gputrace";
  REQUIRE(FileIO::WriteAll(tracePath, "test"_lit));
  FakeRunnerState state;
  AppleTraceSession *session =
      CreateGPUDebugAppleTraceSession(tracePath, new ReplayerErrorFakeRunner(state));

  MetalTrace::Index index;
  REQUIRE(session->Normalise(index).code == ResultCode::Succeeded);
  REQUIRE(index.nodes.size() == 10);
  CHECK_FALSE(index.nodes[6].canFetch);
  CHECK(index.bufferFetchUnavailableReason.contains("synthetic XPC failure"));

  bytebuf data;
  RDResult fetch = session->Fetch(index.nodes[6].stableId, index.nodes[6].path, data);
  CHECK(fetch.code == ResultCode::APIUnsupported);
  CHECK(rdcstr(fetch.message).contains("synthetic XPC failure"));
  CHECK(data.empty());

  session->Shutdown();
  CHECK(state.terminateCalls == 1);
  delete session;
  FileIO::Delete(tracePath);
}

TEST_CASE("Apple GPU Trace session retries transient replay XPC startup failures",
          "[metal][apple-trace]")
{
  const rdcstr tracePath = "/tmp/renderdoc-metal-session-transient-replay.gputrace";
  REQUIRE(FileIO::WriteAll(tracePath, "test"_lit));
  FakeRunnerState state;
  AppleTraceSession *session =
      CreateGPUDebugAppleTraceSession(tracePath, new TransientReplayerErrorFakeRunner(state));

  MetalTrace::Index index;
  REQUIRE(session->Normalise(index).code == ResultCode::Succeeded);
  REQUIRE(index.nodes.size() == 10);
  CHECK(index.nodes[6].canFetch);
  CHECK(index.bufferFetchUnavailableReason.empty());

  session->Shutdown();
  CHECK(state.terminateCalls == 2);
  delete session;
  FileIO::Delete(tracePath);
}

TEST_CASE("gpudebug tool runner detaches inherited stdin", "[metal][apple-trace]")
{
  int inheritedInput[2] = {-1, -1};
  int savedInput = dup(STDIN_FILENO);
  bool setup = savedInput >= 0 && pipe(inheritedInput) == 0;
  if(setup)
    setup = dup2(inheritedInput[0], STDIN_FILENO) >= 0;
  if(inheritedInput[0] >= 0)
  {
    close(inheritedInput[0]);
    inheritedInput[0] = -1;
  }

  AppleTraceToolResult result;
  if(setup)
  {
    std::atomic<bool> cancelled(false);
    GPUDebugToolRunner runner("/bin/sh");
    result = runner.Run({"-c", "read ignored; exit 0"}, 1000, cancelled);
  }

  if(savedInput >= 0)
  {
    dup2(savedInput, STDIN_FILENO);
    close(savedInput);
  }
  if(inheritedInput[1] >= 0)
    close(inheritedInput[1]);

  REQUIRE(setup);
  CHECK_FALSE(result.timedOut);
  CHECK(result.exitCode == 0);
}

TEST_CASE("Apple GPU Trace session maps tool and schema failures", "[metal][apple-trace]")
{
  const rdcstr tracePath = "/tmp/renderdoc-metal-session-errors.gputrace";
  REQUIRE(FileIO::WriteAll(tracePath, "test"_lit));
  MetalTrace::Index index;

  SECTION("timeout")
  {
    AppleTraceSession *session = CreateGPUDebugAppleTraceSession(
        tracePath, new ProbeFailureFakeRunner(ProbeFailureFakeRunner::Mode::Timeout));
    RDResult result = session->Normalise(index);
    CHECK(result.code == ResultCode::APIReplayFailed);
    CHECK(rdcstr(result.message).contains("timed out"));
    delete session;
  }

  SECTION("tool crash")
  {
    AppleTraceSession *session = CreateGPUDebugAppleTraceSession(
        tracePath, new ProbeFailureFakeRunner(ProbeFailureFakeRunner::Mode::Crash));
    RDResult result = session->Normalise(index);
    CHECK(result.code == ResultCode::APIReplayFailed);
    CHECK(rdcstr(result.message).contains("exit code 9"));
    delete session;
  }

  SECTION("incompatible version")
  {
    AppleTraceSession *session = CreateGPUDebugAppleTraceSession(
        tracePath, new ProbeFailureFakeRunner(ProbeFailureFakeRunner::Mode::Incompatible));
    RDResult result = session->Normalise(index);
    CHECK(result.code == ResultCode::APIIncompatibleVersion);
    CHECK(rdcstr(result.message).contains("gpudebug 2.0"));
    delete session;
  }

  SECTION("malformed JSON")
  {
    FakeRunnerState state;
    AppleTraceSession *session =
        CreateGPUDebugAppleTraceSession(tracePath, new MalformedFakeRunner(state));
    RDResult result = session->Normalise(index);
    CHECK(result.code == ResultCode::APIDataCorrupted);
    CHECK(rdcstr(result.message).contains("malformed"));
    delete session;
    CHECK(state.terminateCalls == 1);
  }

  FileIO::Delete(tracePath);
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
