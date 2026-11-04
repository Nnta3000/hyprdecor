#pragma once
#include <hyprland/src/render/pass/PassElement.hpp>

class CHyprWindowDecorator;

// Hyprland 0.57 threads a Render::CRenderContext through every draw call. The pass
// element receives it in draw(), so the context must be carried into the decorator's
// drawing code rather than reached for globally -- hence the parameter on every virtual.
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

  virtual std::vector<UP<IPassElement>> draw(Render::CRenderContext &ctx);
  virtual bool needsLiveBlur(Render::CRenderContext &ctx);
  virtual bool needsPrecomputeBlur(Render::CRenderContext &ctx);
  virtual std::optional<CBox> boundingBox(Render::CRenderContext &ctx);

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
