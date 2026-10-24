#pragma once
#include <hyprland/src/render/pass/PassElement.hpp>

class CHyprWindowDecorator;

class CRenderPassElement : public IPassElement
{
public:
  struct SBarData
  {
    CHyprWindowDecorator *deco = nullptr;
    PHLMONITOR pMonitor = nullptr;
    float a = 1.F;
  };

  CRenderPassElement(const SBarData &data_);
  virtual ~CRenderPassElement() = default;

  virtual std::vector<UP<IPassElement>> draw();
  virtual bool needsLiveBlur();
  virtual bool needsPrecomputeBlur();
  virtual std::optional<CBox> boundingBox();

  virtual ePassElementType type()
  {
    return EK_CUSTOM;
  }

  virtual const char *passName()
  {
    return "CRenderPassElement";
  }

private:
  SBarData data;
};