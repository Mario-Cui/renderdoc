#pragma once
#include <cstdint>
#include "apidefs.h"

enum class PerfRangeType
{
  PerAction = 0,
  PerFrame = 1,
  PerRange = 2,
};

struct PerfRangeData
{
  uint32_t rangeBeginEid;
  uint32_t rangeEndEid;
};

struct ApiPerfConfigParams
{
  PerfRangeType rangeType = PerfRangeType::PerAction;
};

struct ExternalConfigParams
{
  ApiPerfConfigParams apiPerfParams;
};

extern "C" RENDERDOC_API void RENDERDOC_CC
RENDERDOC_SetExternalConfig(const ExternalConfigParams *configParams);

extern "C" RENDERDOC_API const ExternalConfigParams *RENDERDOC_CC RENDERDOC_GetExternalConfig();
