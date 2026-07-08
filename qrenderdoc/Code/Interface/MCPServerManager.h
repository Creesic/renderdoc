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

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

struct ICaptureContext;

// Launches renderdoc-mcp as a child process (Python) with PYTHONPATH beside qrenderdoc.
class MCPServerManager : public QObject
{
  Q_OBJECT

public:
  explicit MCPServerManager(ICaptureContext &ctx, QObject *parent = NULL);
  ~MCPServerManager();

  void applyConfig();
  void stop();

  /*! Path to python shipped next to RenderDoc (\c python/python.exe or \c python/bin/python3), or empty. */
  static QString bundledPythonExecutablePath();

  QString statusSummary() const { return m_StatusSummary; }
  QString statusToolTip() const { return m_StatusTooltip; }

signals:
  void stateChanged();

private slots:
  void processStarted();
  void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void processError(QProcess::ProcessError error);
  void readStd();

private:
  ICaptureContext &m_Ctx;
  QProcess m_Process;
  QString m_StatusSummary;
  QString m_StatusTooltip;
  QString m_LogTail;

  // last config actually applied, so redundant applyConfig() calls (the settings dialog can
  // emit several times per edit) don't stop/restart a server whose config hasn't changed.
  bool m_AppliedValid = false;
  bool m_AppliedEnabled = false;
  int m_AppliedPort = 0;
  QString m_AppliedPython;

  QString buildPythonPath() const;
  bool resolvePython(QString &program, QStringList &args, QString *errorDetail = NULL);
  void appendLog(const QByteArray &chunk);
  void rebuildTooltip();
  void scheduleListenProbe(int attempt);
};
