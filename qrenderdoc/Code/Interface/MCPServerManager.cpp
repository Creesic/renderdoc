/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2026 Baldur Karlsson
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

#include "MCPServerManager.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include "Code/Interface/QRDInterface.h"
#include "Code/QRDUtils.h"

namespace
{
#if defined(Q_OS_WIN32)
void prependPathEnv(QProcessEnvironment &env, const QString &dir)
{
  if(dir.isEmpty())
    return;
  const QString abs = QDir::toNativeSeparators(QDir(dir).absolutePath());

  const QLatin1String kPrimary("PATH");
  const QLatin1String kAlt("Path");
  QString key = kPrimary;
  QString cur = env.value(kPrimary);
  if(cur.isEmpty())
  {
    cur = env.value(kAlt);
    key = kAlt;
  }

  const QLatin1Char sep(';');
  if(!cur.isEmpty())
  {
    const QStringList tok = cur.split(sep, QString::SkipEmptyParts);
    for(const QString &t : tok)
    {
      const QString norm = QDir::toNativeSeparators(QDir::cleanPath(t));
      if(norm.compare(abs, Qt::CaseInsensitive) == 0)
        return;
    }
    env.insert(key, abs + sep + cur);
  }
  else
  {
    env.insert(kPrimary, abs);
  }
}

bool dirContainsRenderdocPyd(const QString &dir)
{
  if(!QDir(dir).exists())
    return false;
  static const QLatin1String names[] = {QLatin1String("renderdoc.pyd"), QLatin1String("_renderdoc.pyd")};
  for(const QLatin1String &n : names)
  {
    if(QFileInfo(QDir(dir).absoluteFilePath(n)).isFile())
      return true;
  }
  return false;
}

QStringList windowsRawPymoduleCandidates(const QString &appDir)
{
  QStringList out;
  QSet<QString> seen;

  auto addOne = [&](const QString &path, bool requireExists) {
    const QString c = QDir::cleanPath(path);
    if(seen.contains(c))
      return;
    if(requireExists && !QDir(c).exists())
      return;
    seen.insert(c);
    out.push_back(c);
  };

  addOne(QDir(appDir).absoluteFilePath(lit("pymodules")), false);

  static const char kRel[][56] = {"../pymodules",
                                   "../../pymodules",
                                   "../../../pymodules",
                                   "../x64/Development/pymodules",
                                   "../../x64/Development/pymodules",
                                   "../../../x64/Development/pymodules",
                                   "../../../../x64/Development/pymodules",
                                   "../x64/Release/pymodules",
                                   "../../x64/Release/pymodules",
                                   "../Win32/Development/pymodules",
                                   "../../Win32/Development/pymodules"};
  for(const auto &rel : kRel)
    addOne(QDir(appDir).absoluteFilePath(QString::fromLatin1(rel)), true);

  return out;
}

QStringList windowsOrderedPymoduleDirs(const QString &appDir)
{
  const QStringList raw = windowsRawPymoduleCandidates(appDir);
  QStringList withPyd;
  QStringList without;
  QSet<QString> seenWith;
  QSet<QString> seenWithout;

  for(const QString &d : raw)
  {
    const QString c = QDir::cleanPath(d);
    if(dirContainsRenderdocPyd(c))
    {
      if(!seenWith.contains(c))
      {
        seenWith.insert(c);
        withPyd.push_back(c);
      }
    }
    else
    {
      if(!seenWithout.contains(c))
      {
        seenWithout.insert(c);
        without.push_back(c);
      }
    }
  }

  return withPyd + without;
}

void prependWindowsRenderDocDllSearch(QProcessEnvironment &env, const QString &appDir,
                                      const QStringList &pymDirsOrdered)
{
  QStringList wantedOrder;
  QSet<QString> seen;

  auto tryAdd = [&](const QString &pathRaw, bool mustExist) {
    const QString c = QDir::cleanPath(QDir(pathRaw).absolutePath());
    if(seen.contains(c))
      return;
    if(mustExist && !QDir(c).exists())
      return;
    seen.insert(c);
    wantedOrder.push_back(QDir::toNativeSeparators(c));
  };

  tryAdd(appDir, false);

  for(const QString &p : pymDirsOrdered)
    tryAdd(p, true);

  const QString plugins = QDir(appDir).absoluteFilePath(lit("plugins"));
  tryAdd(plugins, true);

  for(int i = wantedOrder.size() - 1; i >= 0; --i)
    prependPathEnv(env, wantedOrder.at(i));
}

enum class WinPeArch
{
  Unknown,
  Bits32,
  Bits64,
};

static quint16 readLe16(const uchar *p)
{
  return quint16(p[0]) | (quint16(p[1]) << 8);
}

static quint32 readLe32(const uchar *p)
{
  return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

static WinPeArch windowsPeArchitecture(const QString &path)
{
  QFile f(path);
  if(!f.open(QIODevice::ReadOnly))
    return WinPeArch::Unknown;
  QByteArray mz = f.read(64);
  if(mz.size() < 64)
    return WinPeArch::Unknown;
  const uchar *h = reinterpret_cast<const uchar *>(mz.constData());
  if(h[0] != 'M' || h[1] != 'Z')
    return WinPeArch::Unknown;
  const quint32 peOff = readLe32(h + 0x3C);
  if(peOff > quint32(f.size()) || quint64(peOff) + 26 > quint64(f.size()))
    return WinPeArch::Unknown;
  if(!f.seek(peOff))
    return WinPeArch::Unknown;
  QByteArray pe = f.read(26);
  if(pe.size() < 26)
    return WinPeArch::Unknown;
  const uchar *p = reinterpret_cast<const uchar *>(pe.constData());
  if(p[0] != 'P' || p[1] != 'E' || p[2] != 0 || p[3] != 0)
    return WinPeArch::Unknown;
  const quint16 magic = readLe16(p + 24);
  if(magic == 0x10b)
    return WinPeArch::Bits32;
  if(magic == 0x20b)
    return WinPeArch::Bits64;
  return WinPeArch::Unknown;
}

static bool interpreterMatchesRenderDocBuild(const QString &path)
{
#if defined(Q_OS_WIN64)
  return windowsPeArchitecture(path) == WinPeArch::Bits64;
#else
  return windowsPeArchitecture(path) == WinPeArch::Bits32;
#endif
}
#endif
}    // namespace

static QString localMCPEndpoint(int port)
{
  return QString::fromLatin1("http://127.0.0.1:%1/mcp").arg(port);
}

static QString posixBundledInterpreter(const QString &appDir)
{
  const QString bindir = QDir(appDir).absoluteFilePath(lit("python/bin"));
  QDir b(bindir);
  if(!b.exists())
    return {};
  QFileInfoList names = b.entryInfoList(QStringList{lit("python3*")},
                                        QDir::Files | QDir::Executable | QDir::System,
                                        QDir::Name | QDir::LocaleAware);
  for(const QFileInfo &fi : names)
  {
    if(fi.fileName().startsWith(lit("python3")) && fi.isExecutable())
      return fi.absoluteFilePath();
  }
  return {};
}

QString MCPServerManager::bundledPythonExecutablePath()
{
  const QString appDir = QApplication::applicationDirPath();
#if defined(Q_OS_WIN32)
  const QString exe = QDir(appDir).absoluteFilePath(lit("python/python.exe"));
  return QFileInfo::exists(exe) ? exe : QString();
#else
  return posixBundledInterpreter(appDir);
#endif
}

MCPServerManager::MCPServerManager(ICaptureContext &ctx, QObject *parent)
    : QObject(parent), m_Ctx(ctx), m_Process(this)
{
  m_Process.setProcessChannelMode(QProcess::MergedChannels);
  QObject::connect(&m_Process, &QProcess::started, this, &MCPServerManager::processStarted);
  QObject::connect(&m_Process,
                     OverloadedSlot<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                     &MCPServerManager::processFinished);
  QObject::connect(&m_Process, &QProcess::errorOccurred, this, &MCPServerManager::processError);
  QObject::connect(&m_Process, &QProcess::readyRead, this, &MCPServerManager::readStd);

  m_StatusSummary = tr("Off");
  rebuildTooltip();
}

MCPServerManager::~MCPServerManager()
{
  stop();
}

QString MCPServerManager::buildPythonPath() const
{
  const QString appDir = QApplication::applicationDirPath();
  QStringList parts;
#if defined(Q_OS_WIN32)
  parts += windowsOrderedPymoduleDirs(appDir);
#else
  parts << QDir(appDir).absoluteFilePath(lit("pymodules"));
#endif
  parts << QDir(appDir).absoluteFilePath(lit("mcp"));
  parts << QDir(appDir).absoluteFilePath(lit("mcp_site"));

#if defined(Q_OS_WIN32)
  const QLatin1Char sep(';');
#else
  const QLatin1Char sep(':');
#endif

  return parts.join(sep);
}

bool MCPServerManager::resolvePython(QString &program, QStringList &args, QString *errorDetail)
{
  args.clear();
  program.clear();
  if(errorDetail)
    errorDetail->clear();

  auto fail = [&](const QString &msg) -> bool {
    if(errorDetail)
      *errorDetail = msg;
    program.clear();
    args.clear();
    return false;
  };

  const int port = m_Ctx.Config().AI_MCP_Port;
  const QStringList moduleArgs{lit("-m"), lit("renderdoc_mcp"), lit("--host"), lit("127.0.0.1"),
                               lit("--port"), QString::number(port)};

#if defined(Q_OS_WIN32)
  auto acceptInterpreter = [&](const QString &candidate, const QStringList &argvIn) -> bool {
    if(candidate.isEmpty() || !QFileInfo::exists(candidate))
      return false;
    if(!interpreterMatchesRenderDocBuild(candidate))
      return false;
    program = candidate;
    args = argvIn;
    return true;
  };
#else
  auto acceptInterpreter = [&](const QString &candidate, const QStringList &argvIn) -> bool {
    if(candidate.isEmpty() || !QFileInfo::exists(candidate))
      return false;
    program = candidate;
    args = argvIn;
    return true;
  };
#endif

  const QString custom = m_Ctx.Config().AI_MCP_PythonPath;
  if(!custom.isEmpty())
  {
    if(!QFileInfo::exists(custom))
      return fail(tr("Python path does not exist.\n"));
#if defined(Q_OS_WIN32)
    if(!interpreterMatchesRenderDocBuild(custom))
    {
#if defined(Q_OS_WIN64)
      return fail(tr("This RenderDoc build is 64-bit; Settings → MCP → Python must point to a "
                     "64-bit python.exe (same architecture as renderdoc.pyd).\n"));
#else
      return fail(tr("This RenderDoc build is 32-bit; Settings → MCP → Python must point to a "
                     "32-bit python.exe.\n"));
#endif
    }
#endif
    program = custom;
    args = moduleArgs;
    return true;
  }

  const QString bundled = bundledPythonExecutablePath();
  if(!bundled.isEmpty())
  {
#if defined(Q_OS_WIN32)
    if(!interpreterMatchesRenderDocBuild(bundled))
      return fail(tr("Bundled python.exe does not match this RenderDoc build architecture.\n"));
#endif
    program = bundled;
    args = moduleArgs;
    return true;
  }

  if(acceptInterpreter(QStandardPaths::findExecutable(lit("python3")), moduleArgs))
    return true;

  if(acceptInterpreter(QStandardPaths::findExecutable(lit("python")), moduleArgs))
    return true;

#if defined(Q_OS_WIN32)
  QString pylaunch = QStandardPaths::findExecutable(lit("py"));
  if(!pylaunch.isEmpty())
  {
    program = pylaunch;
#if defined(Q_OS_WIN64)
    args = QStringList{lit("-3-64")} + moduleArgs;
#else
    args = QStringList{lit("-3-32")} + moduleArgs;
#endif
    return true;
  }

#if defined(Q_OS_WIN64)
  return fail(tr("No 64-bit Python interpreter found. Install 64-bit Python 3, add it to PATH, "
                 "or set an explicit path in Settings → MCP (must match this 64-bit RenderDoc "
                 "build).\n"));
#else
  return fail(tr("No 32-bit Python interpreter found. Install Python 3 or set path in Settings → "
                 "MCP.\n"));
#endif
#else
  Q_UNUSED(fail);
  program.clear();
  args.clear();
  return false;
#endif
}

void MCPServerManager::applyConfig()
{
  stop();
  m_LogTail.clear();

  if(!m_Ctx.Config().AI_MCP_Enabled)
  {
    m_StatusSummary = tr("Off");
    rebuildTooltip();
    emit stateChanged();
    return;
  }

  QString exe;
  QStringList argv;
  QString pyErr;
  if(!resolvePython(exe, argv, &pyErr))
  {
    m_StatusSummary = tr("Error");
    m_LogTail = pyErr.isEmpty()
                    ? tr("No Python interpreter found. Install Python 3 or set an explicit path in "
                         "Settings → MCP.\n")
                    : pyErr;
    rebuildTooltip();
    emit stateChanged();
    return;
  }

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString bundleDir = QApplication::applicationDirPath();
#if defined(Q_OS_WIN32)
  prependWindowsRenderDocDllSearch(env, bundleDir, windowsOrderedPymoduleDirs(bundleDir));
#endif
  const QString pyPathValue = buildPythonPath();
  env.insert(lit("PYTHONPATH"), pyPathValue);
  env.insert(lit("PYTHONUTF8"), lit("1"));
  env.insert(lit("RENDERDOC_MCP_APPDIR"), bundleDir);

  QString processWorkDir = bundleDir;
#if defined(Q_OS_WIN32)
  const QString bun = bundledPythonExecutablePath();
  const QString exeCanon = QFileInfo(exe).canonicalFilePath();
  const QString bunCanon = QFileInfo(bun).canonicalFilePath();
  if(!bun.isEmpty() && !exeCanon.isEmpty() && !bunCanon.isEmpty() &&
     exeCanon.compare(bunCanon, Qt::CaseInsensitive) == 0)
    processWorkDir = QFileInfo(bun).absolutePath();
#endif

  m_Process.setProcessEnvironment(env);
  m_Process.setWorkingDirectory(processWorkDir);

  m_StatusSummary = tr("Starting…");
  rebuildTooltip();
  emit stateChanged();

  m_Process.start(exe, argv);
}

void MCPServerManager::stop()
{
  if(m_Process.state() == QProcess::NotRunning)
    return;

  m_Process.terminate();
  if(!m_Process.waitForFinished(2500))
  {
    m_Process.kill();
    m_Process.waitForFinished(1000);
  }
}

void MCPServerManager::processStarted()
{
  m_StatusSummary = tr("Starting…");
  rebuildTooltip();
  emit stateChanged();
  scheduleListenProbe(0);
}

void MCPServerManager::processFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
  appendLog(tr("\n[MCP subprocess exit status %1, code %2]\n")
                  .arg(exitStatus == QProcess::CrashExit ? lit("crash") : lit("normal"))
                  .arg(exitCode)
                  .toUtf8());

  if(!m_Ctx.Config().AI_MCP_Enabled)
  {
    m_StatusSummary = tr("Off");
  }
  else if(exitStatus == QProcess::CrashExit)
  {
    m_StatusSummary = tr("Crashed");
  }
  else
  {
    m_StatusSummary = tr("Stopped");
  }

  rebuildTooltip();
  emit stateChanged();
}

void MCPServerManager::processError(QProcess::ProcessError error)
{
  QString errstr = m_Process.errorString();
  appendLog((lit("\nProcess error: ") + QString::number((int)error) + lit(" ") + errstr + lit("\n"))
                .toUtf8());

  if(m_Ctx.Config().AI_MCP_Enabled && m_Process.state() != QProcess::Running)
  {
    m_StatusSummary = tr("Error");
    if(error == QProcess::FailedToStart)
      appendLog(tr("Interpreter failed to start. Check Settings → MCP → Python path.\n").toUtf8());
  }

  rebuildTooltip();
  emit stateChanged();
}

void MCPServerManager::readStd()
{
  appendLog(m_Process.readAll());
}

void MCPServerManager::appendLog(const QByteArray &chunk)
{
  if(chunk.isEmpty())
    return;

  m_LogTail.append(QString::fromUtf8(chunk));

  const int maxChars = 4000;
  if(m_LogTail.size() > maxChars)
    m_LogTail = m_LogTail.right(maxChars);
}

void MCPServerManager::rebuildTooltip()
{
  const int port = m_Ctx.Config().AI_MCP_Port;
  const QString url = localMCPEndpoint(port);
  QString tip = tr("Model Context Protocol (replay introspection)\n"
                   "Endpoint: %1\n\n"
                   "Local-only HTTP binding (127.0.0.1). Copy the URL for your MCP client.\n"
                   "Configure in Settings → MCP.\n")
                    .arg(url);

  if(!m_LogTail.isEmpty())
  {
    tip += lit("\n--- output (tail) ---\n");
    tip += m_LogTail;
  }

  m_StatusTooltip = tip;
}

void MCPServerManager::scheduleListenProbe(int attempt)
{
  constexpr int kMaxAttempts = 40;
  constexpr int kIntervalMs = 50;

  if(m_Process.state() != QProcess::Running)
    return;

  QTcpSocket s;
  s.connectToHost(QHostAddress::LocalHost, (quint16)m_Ctx.Config().AI_MCP_Port);
  const bool ok = s.waitForConnected(80);
  if(ok)
  {
    m_StatusSummary = tr("Running");
    s.disconnectFromHost();
    rebuildTooltip();
    emit stateChanged();
    return;
  }

  if(attempt + 1 >= kMaxAttempts)
  {
    m_StatusSummary = tr("Running?");
    rebuildTooltip();
    emit stateChanged();
    return;
  }

  QTimer::singleShot(kIntervalMs, this, [this, attempt]() { scheduleListenProbe(attempt + 1); });
}
