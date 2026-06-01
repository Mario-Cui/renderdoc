#include "RayTraceInfoViewer.h"
#include <QFontDatabase>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include "Code/QRDUtils.h"
#include "shader_types.h"
#include "ui_RayTraceInfoViewer.h"

class RayHitItemModel : public QAbstractItemModel
{
public:
  RayHitItemModel(ICaptureContext &ctx, QObject *parent) : QAbstractItemModel(parent), m_Ctx(ctx)
  {
    m_NumRows = 0;
  }

  void refresh(rdcarray<RayHitInfo> *invocations)
  {
    emit beginResetModel();

    if(NULL == invocations)
    {
      m_Data = NULL;
    }
    else
    {
      m_Data = invocations;
    }
    resetFilter();

    emit endResetModel();
  }

  bool isFilterChanged(int shaderTypeFilter, int dispatchXFilter, int dispatchYFilter,
                       int dispatchZFilter)
  {
    return (shaderTypeFilter != m_shaderTypeFilter) || (dispatchXFilter != m_dispatchXFilter) ||
           (dispatchYFilter != m_dispatchYFilter) || (dispatchZFilter != m_dispatchZFilter);
  }

  void setFilter(int shaderTypeFilter, int dispatchXFilter = -1, int dispatchYFilter = -1,
                 int dispatchZFilter = -1)
  {
    if(!isFilterChanged(shaderTypeFilter, dispatchXFilter, dispatchYFilter, dispatchZFilter))
      return;

    m_shaderTypeFilter = shaderTypeFilter;
    m_dispatchXFilter = dispatchXFilter;
    m_dispatchYFilter = dispatchYFilter;
    m_dispatchZFilter = dispatchZFilter;

    applyFilterAsync();
  }

  void resetFilter()
  {
    m_shaderTypeFilter = -1;
    m_dispatchXFilter = -1;
    m_dispatchYFilter = -1;
    m_dispatchZFilter = -1;

    if(m_Data)
    {
      m_FilteredIndices.resize(m_Data->size());
      for(int i = 0; i < m_Data->size(); i++)
        m_FilteredIndices[i] = i;

      m_NumRows = (int)m_FilteredIndices.size();
      emit beginResetModel();
      emit endResetModel();
    }
    else
    {
      m_FilteredIndices.clear();
      m_NumRows = 0;
    }
  }

  QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
  {
    if(row < 0 || row >= rowCount())
      return QModelIndex();
    return createIndex(row, column);
  }

  QModelIndex parent(const QModelIndex &) const override { return QModelIndex(); }
  int rowCount(const QModelIndex & = QModelIndex()) const override { return m_NumRows; }
  int columnCount(const QModelIndex & = QModelIndex()) const override { return 18; }

  Qt::ItemFlags flags(const QModelIndex &index) const override
  {
    if(!index.isValid())
      return Qt::NoItemFlags;
    return QAbstractItemModel::flags(index);
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override
  {
    if(orientation == Qt::Horizontal)
    {
      if(role == Qt::DisplayRole)
      {
        switch(section)
        {
          case 0: return lit("Type");
          case 1: return lit("Dispatch X");
          case 2: return lit("Dispatch Y");
          case 3: return lit("Dispatch Z");
          case 4: return lit("Origin X");
          case 5: return lit("Origin Y");
          case 6: return lit("Origin Z");
          case 7: return lit("Dir X");
          case 8: return lit("Dir Y");
          case 9: return lit("Dir Z");
          case 10: return lit("tMin");
          case 11: return lit("tCurrent");
          case 12: return lit("Flags");
          case 13: return lit("InstanceIndex");
          case 14: return lit("InstanceId");
          case 15: return lit("GeometryIndex");
          case 16: return lit("PrimitiveIndex");
          case 17: return lit("HitKind");
        }
      }
    }

    return QVariant();
  }

  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
  {
    if(!index.isValid())
      return QVariant();
    if(m_NumRows == 0)
      return QVariant();

    int realIdx = m_FilteredIndices[index.row()];
    const RayHitInfo &inv = m_Data->data()[realIdx];
    int col = index.column();

    if(role == Qt::TextAlignmentRole)
    {
      if(col == 0)
        return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
      return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }

    if(role == Qt::DisplayRole)
    {
      switch(col)
      {
        case 0:
        {
          if(inv.shaderType == int32_t(ShaderStage::Miss))
          {
            return lit("Miss");
          }
          else if(inv.shaderType == int32_t(ShaderStage::ClosestHit))
          {
            return lit("ClosestHit");
          }
          else if(inv.shaderType == int32_t(ShaderStage::AnyHit))
          {
            return lit("AnyHit");
          }
          else if(inv.shaderType == int32_t(ShaderStage::Intersection))
          {
            return lit("Intersection");
          }
          else if(inv.shaderType == int32_t(ShaderStage::Callable))
          {
            return lit("Callable");
          }
          else if(inv.shaderType == 0xFF)
          {
            return lit("DebugSummaryCount");
          }
          else
          {
            return lit("Unknown");
          }
        }
        case 1: return inv.dispatchX;
        case 2: return inv.dispatchY;
        case 3: return inv.dispatchZ;
        case 4: return inv.originX;
        case 5: return inv.originY;
        case 6: return inv.originZ;
        case 7: return inv.dirX;
        case 8: return inv.dirY;
        case 9: return inv.dirZ;
        case 10: return inv.tMin;
        case 11: return inv.tCurrent;
        case 12: return inv.flags;
        case 13: return inv.instanceIndex;
        case 14: return inv.instanceId;
        case 15: return inv.geometryIndex;
        case 16: return inv.primitiveIndex;
        case 17: return inv.hitKind;
        default: return QVariant();
      }
    }

    return QVariant();
  }

private:
  void applyFilterAsync()
  {
    if(!m_Data)
      return;

    auto shaderTypeFilter = m_shaderTypeFilter;
    auto dispatchXFilter = m_dispatchXFilter;
    auto dispatchYFilter = m_dispatchYFilter;
    auto dispatchZFilter = m_dispatchZFilter;
    rdcarray<RayHitInfo> *data = m_Data;

    bool done = false;
    m_Ctx.Replay().AsyncInvoke([this, &done, data, shaderTypeFilter, dispatchXFilter, dispatchYFilter,
                                dispatchZFilter](IReplayController *controller) -> void {
      rdcarray<int> filtered;

      filtered.reserve(data->size() / 1000);    // estimate

      for(size_t i = 0; i < data->size(); i++)
      {
        const RayHitInfo &inv = data->at(i);

        if(shaderTypeFilter != -1 && (int)inv.shaderType != shaderTypeFilter)
          continue;

        if(dispatchXFilter != -1 && (int)inv.dispatchX != dispatchXFilter)
          continue;

        if(dispatchYFilter != -1 && (int)inv.dispatchY != dispatchYFilter)
          continue;

        if(dispatchZFilter != -1 && (int)inv.dispatchZ != dispatchZFilter)
          continue;

        filtered.push_back(int(i));
      }

      GUIInvoke::call(this, [this, filtered]() {
        beginResetModel();
        m_FilteredIndices = filtered;
        m_NumRows = (int)m_FilteredIndices.size();
        endResetModel();
      });

      done = true;
    });

    ShowProgressDialog(NULL, tr("filter invocation"), [&done]() -> bool { return done; });
  }

private:
  ICaptureContext &m_Ctx;
  rdcarray<RayHitInfo> *m_Data;
  int m_NumRows;
  rdcarray<int> m_FilteredIndices;
  // filter condition
  int m_shaderTypeFilter = -1;    // -1 = All
  int m_dispatchXFilter = -1;     // -1 = no limit
  int m_dispatchYFilter = -1;
  int m_dispatchZFilter = -1;
};

class RayCallItemModel : public QAbstractItemModel
{
public:
  RayCallItemModel(ICaptureContext &ctx, QObject *parent) : QAbstractItemModel(parent), m_Ctx(ctx)
  {
    m_NumRows = 0;
  }

  void refresh(rdcarray<RayCallInfo> *generateInfos)
  {
    emit beginResetModel();

    if(NULL == generateInfos)
    {
      m_Data = NULL;
    }
    else
    {
      m_Data = generateInfos;
    }
    resetFilter();

    emit endResetModel();
  }

  bool isFilterChanged(int shaderTypeFilter, int dispatchXFilter, int dispatchYFilter,
                       int dispatchZFilter)
  {
    return (shaderTypeFilter != m_shaderTypeFilter) || (dispatchXFilter != m_dispatchXFilter) ||
           (dispatchYFilter != m_dispatchYFilter) || (dispatchZFilter != m_dispatchZFilter);
  }

  void setFilter(int shaderTypeFilter, int dispatchXFilter = -1, int dispatchYFilter = -1,
                 int dispatchZFilter = -1)
  {
    if(!isFilterChanged(shaderTypeFilter, dispatchXFilter, dispatchYFilter, dispatchZFilter))
      return;

    m_shaderTypeFilter = shaderTypeFilter;
    m_dispatchXFilter = dispatchXFilter;
    m_dispatchYFilter = dispatchYFilter;
    m_dispatchZFilter = dispatchZFilter;

    applyFilterAsync();
  }

  void resetFilter()
  {
    m_shaderTypeFilter = -1;
    m_dispatchXFilter = -1;
    m_dispatchYFilter = -1;
    m_dispatchZFilter = -1;

    if(m_Data)
    {
      m_FilteredIndices.resize(m_Data->size());
      for(int i = 0; i < m_Data->size(); i++)
        m_FilteredIndices[i] = i;

      m_NumRows = (int)m_FilteredIndices.size();
      emit beginResetModel();
      emit endResetModel();
    }
    else
    {
      m_FilteredIndices.clear();
      m_NumRows = 0;
    }
  }

  QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
  {
    if(row < 0 || row >= rowCount())
      return QModelIndex();
    return createIndex(row, column);
  }

  QModelIndex parent(const QModelIndex &) const override { return QModelIndex(); }
  int rowCount(const QModelIndex & = QModelIndex()) const override { return m_NumRows; }
  int columnCount(const QModelIndex & = QModelIndex()) const override { return 17; }

  Qt::ItemFlags flags(const QModelIndex &index) const override
  {
    if(!index.isValid())
      return Qt::NoItemFlags;
    return QAbstractItemModel::flags(index);
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override
  {
    if(orientation == Qt::Horizontal)
    {
      if(role == Qt::DisplayRole)
      {
        switch(section)
        {
          case 0: return lit("Type");
          case 1: return lit("Dispatch X");
          case 2: return lit("Dispatch Y");
          case 3: return lit("Dispatch Z");
          case 4: return lit("Origin X");
          case 5: return lit("Origin Y");
          case 6: return lit("Origin Z");
          case 7: return lit("Dir X");
          case 8: return lit("Dir Y");
          case 9: return lit("Dir Z");
          case 10: return lit("tMin");
          case 11: return lit("tMax");
          case 12: return lit("Flags");
          case 13: return lit("hitGroupIndex");
          case 14: return lit("hitGroupMul");
          case 15: return lit("missIndex");
          case 16: return lit("InstanceMask");
        }
      }
    }

    return QVariant();
  }

  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
  {
    if(!index.isValid())
      return QVariant();
    if(m_NumRows == 0)
      return QVariant();

    int realIdx = m_FilteredIndices[index.row()];
    const RayCallInfo &rayGenInfo = m_Data->data()[realIdx];
    int col = index.column();

    if(role == Qt::TextAlignmentRole)
    {
      if(col == 0)
        return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
      return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }

    if(role == Qt::DisplayRole)
    {
      switch(col)
      {
        case 0:
        {
          int32_t shaderType = rayGenInfo.maskAndShderType >> 8;

          if(shaderType == int32_t(ShaderStage::Miss))
          {
            return lit("Miss");
          }
          else if(shaderType == int32_t(ShaderStage::ClosestHit))
          {
            return lit("ClosestHit");
          }
          else if(shaderType == int32_t(ShaderStage::RayGen))
          {
            return lit("RayGen");
          }
          else if(shaderType == 0xFF)
          {
            return lit("DebugSummaryCount");
          }
          else
          {
            return lit("Unknown");
          }
        }
        case 1: return rayGenInfo.dispatchX;
        case 2: return rayGenInfo.dispatchY;
        case 3: return rayGenInfo.dispatchZ;
        case 4: return rayGenInfo.originX;
        case 5: return rayGenInfo.originY;
        case 6: return rayGenInfo.originZ;
        case 7: return rayGenInfo.dirX;
        case 8: return rayGenInfo.dirY;
        case 9: return rayGenInfo.dirZ;
        case 10: return rayGenInfo.tMin;
        case 11: return rayGenInfo.tMax;
        case 12: return rayGenInfo.flags;
        case 13: return rayGenInfo.hitGroupIndex;
        case 14: return rayGenInfo.hitGroupMul;
        case 15: return rayGenInfo.missIndex;
        case 16:
        {
          uint32_t mask = rayGenInfo.maskAndShderType & 0xFF;
          return mask;
        }
        default: return QVariant();
      }
    }

    return QVariant();
  }

private:
  void applyFilterAsync()
  {
    if(!m_Data)
      return;

    auto shaderTypeFilter = m_shaderTypeFilter;
    auto dispatchXFilter = m_dispatchXFilter;
    auto dispatchYFilter = m_dispatchYFilter;
    auto dispatchZFilter = m_dispatchZFilter;
    rdcarray<RayCallInfo> *data = m_Data;

    bool done = false;
    m_Ctx.Replay().AsyncInvoke([this, &done, data, shaderTypeFilter, dispatchXFilter, dispatchYFilter,
                                dispatchZFilter](IReplayController *controller) -> void {
      rdcarray<int> filtered;

      filtered.reserve(data->size() / 1000);    // estimate

      for(size_t i = 0; i < data->size(); i++)
      {
        const RayCallInfo &rayGenInfo = data->at(i);
        int32_t shaderType = rayGenInfo.maskAndShderType >> 8;
        if(shaderTypeFilter != -1 && (int)shaderType != shaderTypeFilter)
          continue;

        if(dispatchXFilter != -1 && (int)rayGenInfo.dispatchX != dispatchXFilter)
          continue;

        if(dispatchYFilter != -1 && (int)rayGenInfo.dispatchY != dispatchYFilter)
          continue;

        if(dispatchZFilter != -1 && (int)rayGenInfo.dispatchZ != dispatchZFilter)
          continue;

        filtered.push_back(int(i));
      }

      GUIInvoke::call(this, [this, filtered]() {
        beginResetModel();
        m_FilteredIndices = filtered;
        m_NumRows = (int)m_FilteredIndices.size();
        endResetModel();
      });

      done = true;
    });

    ShowProgressDialog(NULL, tr("filter rayGen info"), [&done]() -> bool { return done; });
  }

private:
  ICaptureContext &m_Ctx;
  rdcarray<RayCallInfo> *m_Data;
  int m_NumRows;
  rdcarray<int> m_FilteredIndices;
  // filter condition
  int m_shaderTypeFilter = -1;    // -1 = All
  int m_dispatchXFilter = -1;     // -1 = no limit
  int m_dispatchYFilter = -1;
  int m_dispatchZFilter = -1;
};

struct RayTraceInfoViewerImpl
{
  RayHitItemModel *rayHitModel = NULL;
  RayCallItemModel *rayCallModel = NULL;
  rdcarray<RayHitInfo> rayHitInfos = {};
  rdcarray<RayCallInfo> rayCallInfos = {};
  bool showRayHitInfo = true;
};

RayTraceInfoViewer::RayTraceInfoViewer(ICaptureContext &ctx, QWidget *parent)
    : m_Ctx(ctx), ui(new Ui::RayTraceInfoViewer)
{
  this->setWindowTitle(lit("Ray Trace Info Viewer"));
  ui->setupUi(this);
  ui->saveCSV->setDisabled(true);

  m_Impl = new RayTraceInfoViewerImpl;

  QRegularExpression regex(lit("^\\d{0,10}$"));
  QValidator *v = new QRegularExpressionValidator(regex, parent);

  ui->dispatchXEdit->setValidator(v);
  ui->dispatchYEdit->setValidator(v);
  ui->dispatchZEdit->setValidator(v);
  ui->dispatchXEdit->setPlaceholderText(lit("X"));
  ui->dispatchYEdit->setPlaceholderText(lit("Y"));
  ui->dispatchZEdit->setPlaceholderText(lit("Z"));

  ui->captureInvocations->setEnabled(m_Ctx.IsCaptureLoaded());
  ui->saveCSV->setEnabled(m_Ctx.IsCaptureLoaded());
  ui->rayInfoType->addItem(lit("RayHitInfo"));
  ui->rayInfoType->addItem(lit("RayCallInfo"));

  ui->shaderTypes->addItem(lit("All Shader Types"));
  ui->shaderTypes->addItem(lit("Intersection"));
  ui->shaderTypes->addItem(lit("AnyHit"));
  ui->shaderTypes->addItem(lit("ClosesHit"));
  ui->shaderTypes->addItem(lit("Miss"));
  ui->shaderTypes->addItem(lit("Callable"));

  m_Impl->rayHitModel = new RayHitItemModel(m_Ctx, this);
  m_Impl->rayCallModel = new RayCallItemModel(m_Ctx, this);

  ui->counterResults->setModel(m_Impl->rayHitModel);
  ui->counterResults->setColumnGroupRole(Qt::UserRole + 500);
  ui->counterResults->horizontalHeader()->setSectionsMovable(true);
  ui->counterResults->horizontalHeader()->setStretchLastSection(false);
  // ui->counterResults->setCustomHeaderSizing(true);

  ui->counterResults->setFont(Formatter::PreferredFont());

  ui->counterResults->setSortingEnabled(false);
  ui->counterResults->sortByColumn(0, Qt::AscendingOrder);
  ui->counterResults->horizontalHeader()->reset();
  m_Ctx.AddCaptureViewer(this);
  m_Impl->rayHitModel->refresh(&m_Impl->rayHitInfos);
  setupConnections();
}

RayTraceInfoViewer::~RayTraceInfoViewer()
{
  m_Ctx.BuiltinWindowClosed(this);
  m_Ctx.RemoveCaptureViewer(this);
  delete ui;
}

void RayTraceInfoViewer::setupConnections()
{
  connect(ui->rayInfoType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int index) {
            QSignalBlocker b0(this->ui->shaderTypes);
            if(index == 0)
            {
              this->m_Impl->showRayHitInfo = true;
              this->ui->shaderTypes->clear();
              this->ui->shaderTypes->addItem(lit("All Shader Types"));
              this->ui->shaderTypes->addItem(lit("Intersection"));
              this->ui->shaderTypes->addItem(lit("AnyHit"));
              this->ui->shaderTypes->addItem(lit("ClosesHit"));
              this->ui->shaderTypes->addItem(lit("Miss"));
              this->ui->shaderTypes->addItem(lit("Callable"));
              this->ui->counterResults->setModel(this->m_Impl->rayHitModel);
            }
            else if(index == 1)
            {
              this->m_Impl->showRayHitInfo = false;
              this->ui->shaderTypes->clear();
              this->ui->shaderTypes->addItem(lit("All Shader Types"));
              this->ui->shaderTypes->addItem(lit("RayGen"));
              this->ui->shaderTypes->addItem(lit("ClosesHit"));
              this->ui->shaderTypes->addItem(lit("Miss"));
              this->ui->counterResults->setModel(this->m_Impl->rayCallModel);
            }
          });

  connect(ui->shaderTypes, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    int shaderTypeFilter = -1;
    if(this->m_Impl->showRayHitInfo)
    {
      switch(idx)
      {
        case 1: shaderTypeFilter = int32_t(ShaderStage::Intersection); break;
        case 2: shaderTypeFilter = int32_t(ShaderStage::AnyHit); break;
        case 3: shaderTypeFilter = int32_t(ShaderStage::ClosestHit); break;
        case 4: shaderTypeFilter = int32_t(ShaderStage::Miss); break;
        case 5: shaderTypeFilter = int32_t(ShaderStage::Callable); break;
        default: shaderTypeFilter = -1; break;
      }
    }
    else
    {
      switch(idx)
      {
        case 1: shaderTypeFilter = int32_t(ShaderStage::RayGen); break;
        case 2: shaderTypeFilter = int32_t(ShaderStage::ClosestHit); break;
        case 3: shaderTypeFilter = int32_t(ShaderStage::Miss); break;
        default: shaderTypeFilter = -1; break;
      }
    }

    int dx = ui->dispatchXEdit->text().isEmpty() ? -1 : ui->dispatchXEdit->text().toInt();
    int dy = ui->dispatchYEdit->text().isEmpty() ? -1 : ui->dispatchYEdit->text().toInt();
    int dz = ui->dispatchZEdit->text().isEmpty() ? -1 : ui->dispatchZEdit->text().toInt();
    if(m_Impl->showRayHitInfo)
    {
      m_Impl->rayHitModel->setFilter(shaderTypeFilter, dx, dy, dz);
    }
    else
    {
      m_Impl->rayCallModel->setFilter(shaderTypeFilter, dx, dy, dz);
    }
  });

  auto onDispatchFilterChanged = [this]() {
    int shaderTypeFilter = -1;
    int idx = ui->shaderTypes->currentIndex();
    if(this->m_Impl->showRayHitInfo)
    {
      switch(idx)
      {
        case 1: shaderTypeFilter = int32_t(ShaderStage::Intersection); break;
        case 2: shaderTypeFilter = int32_t(ShaderStage::AnyHit); break;
        case 3: shaderTypeFilter = int32_t(ShaderStage::ClosestHit); break;
        case 4: shaderTypeFilter = int32_t(ShaderStage::Miss); break;
        case 5: shaderTypeFilter = int32_t(ShaderStage::Callable); break;
        default: shaderTypeFilter = -1; break;
      }
    }
    else
    {
      switch(idx)
      {
        case 1: shaderTypeFilter = int32_t(ShaderStage::RayGen); break;
        case 2: shaderTypeFilter = int32_t(ShaderStage::ClosestHit); break;
        case 3: shaderTypeFilter = int32_t(ShaderStage::Miss); break;
        default: shaderTypeFilter = -1; break;
      }
    }
    int dx = ui->dispatchXEdit->text().isEmpty() ? -1 : ui->dispatchXEdit->text().toInt();
    int dy = ui->dispatchYEdit->text().isEmpty() ? -1 : ui->dispatchYEdit->text().toInt();
    int dz = ui->dispatchZEdit->text().isEmpty() ? -1 : ui->dispatchZEdit->text().toInt();

    if(m_Impl->showRayHitInfo)
    {
      m_Impl->rayHitModel->setFilter(shaderTypeFilter, dx, dy, dz);
    }
    else
    {
      m_Impl->rayCallModel->setFilter(shaderTypeFilter, dx, dy, dz);
    }
  };

  connect(ui->dispatchXEdit, &QLineEdit::returnPressed, this, onDispatchFilterChanged);
  connect(ui->dispatchYEdit, &QLineEdit::returnPressed, this, onDispatchFilterChanged);
  connect(ui->dispatchZEdit, &QLineEdit::returnPressed, this, onDispatchFilterChanged);

  auto onCaptureInvocationClicked = [this]() {
    bool done = false;
    m_Ctx.Replay().AsyncInvoke([this, &done](IReplayController *controller) -> void {
      this->m_Impl->rayHitModel->refresh(NULL);
      this->m_Impl->rayCallModel->refresh(NULL);

      rdcarray<RayHitInfo> invocations;
      rdcarray<RayCallInfo> traceCalls;
      bool hasInvocations = controller->GetRayHitData(this->m_Ctx.CurEvent(), invocations);
      bool hasTraceCalls = controller->GetRayCallData(this->m_Ctx.CurEvent(), traceCalls);
      if(hasInvocations || hasTraceCalls)
      {
        this->m_Impl->rayHitInfos = invocations;
        this->m_Impl->rayCallInfos = traceCalls;

        GUIInvoke::call(this, [this]() {
          this->m_Impl->rayHitModel->refresh(&this->m_Impl->rayHitInfos);
          this->m_Impl->rayCallModel->refresh(&this->m_Impl->rayCallInfos);
        });
      }
      done = true;
    });

    ShowProgressDialog(this, tr("Capturing RayInvocations"), [&done]() -> bool { return done; });
  };

  connect(ui->captureInvocations, &QToolButton::clicked, this, onCaptureInvocationClicked);

  connect(ui->saveCSV, &QToolButton::clicked, this, [this]() {
    QString filename = RDDialog::getSaveFileName(this, tr("Export rayinfo results as CSV"),
                                                 QString(), tr("CSV Files (*.csv)"));

    if(!filename.isEmpty())
    {
      QDir dirinfo = QFileInfo(filename).dir();
      if(dirinfo.exists())
      {
        QFile f(filename, this);
        bool retOk = false;
        LambdaThread *exportThread = new LambdaThread([this, &f, &retOk]() {
          if(f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
          {
            QTextStream ts(&f);

            QAbstractItemModel *model = ui->counterResults->model();

            for(int col = 0; col < model->columnCount(); col++)
            {
              ts << model->headerData(col, Qt::Horizontal).toString();

              if(col == model->columnCount() - 1)
                ts << lit("\n");
              else
                ts << lit(",");
            }

            for(int row = 0; row < model->rowCount(); row++)
            {
              for(int col = 0; col < model->columnCount(); col++)
              {
                ts << model->index(row, col).data().toString();

                if(col == model->columnCount() - 1)
                  ts << lit("\n");
                else
                  ts << lit(",");
              }
            }
            retOk = true;
            return;
          }
        });

        exportThread->start();

        ShowProgressDialog(this, tr("Exporting data"),
                           [exportThread]() { return !exportThread->isRunning(); });

        exportThread->deleteLater();
        if(!retOk)
        {
          RDDialog::critical(
              this, tr("Error exporting rayinfo results"),
              tr("Couldn't open path %1 for write.\n%2").arg(filename).arg(f.errorString()));
        }
      }
      else
      {
        RDDialog::critical(this, tr("Invalid directory"),
                           tr("Cannot find target directory to save to"));
      }
    }
  });
}

void RayTraceInfoViewer::OnCaptureLoaded()
{
}

void RayTraceInfoViewer::OnCaptureClosed()
{
}

void RayTraceInfoViewer::OnSelectedEventChanged(uint32_t eventId)
{
}

void RayTraceInfoViewer::OnEventChanged(uint32_t eventId)
{
  if(IsDispatchRay())
  {
    ui->captureInvocations->setEnabled(true);
    ui->saveCSV->setEnabled(true);
  }
  else
  {
    ui->captureInvocations->setDisabled(true);
    ui->saveCSV->setDisabled(false);
  }
}

bool RayTraceInfoViewer::IsDispatchRay()
{
  const ActionDescription *action = m_Ctx.CurAction();
  return action && action->flags & ActionFlags::DispatchRay;
}
