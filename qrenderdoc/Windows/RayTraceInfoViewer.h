#pragma once

#include <QFrame>
#include "Code/Interface/QRDInterface.h"

namespace Ui
{
class RayTraceInfoViewer;
}

struct RayTraceInfoViewerImpl;

class RayTraceInfoViewer : public QFrame,
                                   public IRayTraceInfoViewer,
                                   public ICaptureViewer
{
  Q_OBJECT
public:
  explicit RayTraceInfoViewer(ICaptureContext &ctx, QWidget *parent = 0);
  ~RayTraceInfoViewer();

  // IRayTraceInfoViewer
  QWidget *Widget() override { return this; }
  // ICaptureViewer
  void OnCaptureLoaded() override;
  void OnCaptureClosed() override;
  void OnSelectedEventChanged(uint32_t eventId) override;
  void OnEventChanged(uint32_t eventId) override;

private:
  bool IsDispatchRay();
  void setupConnections();
private:
  ICaptureContext &m_Ctx;
  Ui::RayTraceInfoViewer *ui;

  RayTraceInfoViewerImpl *m_Impl;
};
