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

#include "MetalPipelineStateViewer.h"

#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include "Code/QRDUtils.h"

MetalPipelineStateViewer::MetalPipelineStateViewer(ICaptureContext &ctx, QWidget *parent)
    : QFrame(parent), m_Ctx(ctx)
{
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);

  m_Summary = new QLabel(this);
  m_Summary->setWordWrap(true);
  layout->addWidget(m_Summary);

  m_Features = new QTreeWidget(this);
  m_Features->setColumnCount(3);
  m_Features->setHeaderLabels({tr("Feature"), tr("Status"), tr("Details")});
  m_Features->setRootIsDecorated(false);
  m_Features->setAlternatingRowColors(true);
  m_Features->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_Features->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_Features->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  layout->addWidget(m_Features, 1);

  refresh();
}

void MetalPipelineStateViewer::OnCaptureLoaded()
{
  refresh();
}

void MetalPipelineStateViewer::OnCaptureClosed()
{
  m_Summary->setText(tr("No Metal capture is loaded."));
  m_Features->clear();
}

void MetalPipelineStateViewer::OnEventChanged(uint32_t eventId)
{
  refresh();
}

void MetalPipelineStateViewer::refresh()
{
  m_Features->clear();

  if(!m_Ctx.IsCaptureLoaded())
  {
    m_Summary->setText(tr("No Metal capture is loaded."));
    return;
  }

  const APIProperties &props = m_Ctx.APIProps();
  const bool executable = props.HasFeature(ReplayFeature::ExecutableReplay);

  m_Summary->setText(tr("Metal %1%2\nEvent %3 | %4 resources (%5 buffers, %6 textures)")
                         .arg(executable ? tr("executable replay") : tr("read-only inspection"))
                         .arg(props.degraded && executable ? tr(" (degraded)") : QString())
                         .arg(m_Ctx.CurEvent())
                         .arg(m_Ctx.GetResources().size())
                         .arg(m_Ctx.GetBuffers().size())
                         .arg(m_Ctx.GetTextures().size()));

  if(props.features.empty())
  {
    QTreeWidgetItem *item = new QTreeWidgetItem(m_Features);
    item->setText(0, tr("Feature reporting"));
    item->setText(1, tr("Legacy"));
    item->setText(2, tr("This driver did not report per-feature capabilities."));
    return;
  }

  for(const ReplayFeatureCapability &capability : props.features)
  {
    const QString reason = QString::fromUtf8(capability.reason.c_str());
    QTreeWidgetItem *item = new QTreeWidgetItem(m_Features);
    item->setText(0, ToQStr(capability.feature));
    item->setText(1, capability.available ? tr("Available") : tr("Unavailable"));
    item->setText(2, capability.available ? QString() : reason);

    if(!capability.available)
      item->setToolTip(2, reason);
  }
}
