#pragma once

#include "glint_shader_base.hpp"
#include "glint_shader_registry.hpp"

class glint_liquid_glass : public glint_shader_base
{
public:
  glint_liquid_glass() { animated = false; isBackdrop = true; }

  float sampleRadius() const override
  {
    const float sampleOffsetX = std::abs(getFloat("sampleOffsetX", 0.f));
    const float sampleOffsetY = std::abs(getFloat("sampleOffsetY", 0.f));
    const float bezelWidth    = std::max(getFloat("bezelWidth", 18.f), 1.f);
    const float thickness     = std::max(getFloat("glassThickness", 28.f), 0.f);
    const float chroma        = std::max(getFloat("chromaticStrength", 0.06f), 0.f)
                              * std::max(getFloat("chromaticBase", 0.75f), 0.f)
                              * bezelWidth;
    return std::max(sampleOffsetX, sampleOffsetY) + bezelWidth + thickness * 0.4f + chroma + 6.f;
  }

  const char* sksl() const override
  {
    return R"(
      uniform shader src;
      uniform float2 resolution;
      uniform float2 origin;
      uniform float2 sampleOffset;
      uniform float  bezelWidth;
      uniform float  glassThickness;
      uniform float  refractiveIndex;
      uniform float  magnification;
      uniform float  surfaceType;
      uniform float  cornerRadius;
      uniform float  maxDisplacementScale;
      uniform float  tintOpacity;
      uniform float  tintR;
      uniform float  tintG;
      uniform float  tintB;
      uniform float  specularOpacity;
      uniform float  specularAngle;
      uniform float  specularWidth;
      uniform float  shadowOpacity;
      uniform float  shadowWidth;
      uniform float  chromaticStrength;
      uniform float  chromaticBase;

      float surfaceConvexCircle(float x) {
        return sqrt(max(1.0 - pow(1.0 - x, 2.0), 0.0));
      }

      float surfaceConvexSquircle(float x) {
        return pow(max(1.0 - pow(1.0 - x, 4.0), 0.0), 0.25);
      }

      float surfaceConcave(float x) {
        return 1.0 - sqrt(max(1.0 - pow(1.0 - x, 2.0), 0.0));
      }

      float surfaceLip(float x) {
        float convex = pow(max(1.0 - pow(1.0 - x * 2.0, 4.0), 0.0), 0.25);
        float concave = 1.0 - sqrt(max(1.0 - pow(1.0 - x, 2.0), 0.0)) + 0.1;
        float smoother = 6.0 * pow(x, 5.0) - 15.0 * pow(x, 4.0) + 10.0 * pow(x, 3.0);
        return mix(convex, concave, smoother);
      }

      float surfaceHeight(float x, float kind) {
        if (kind < 0.5) return surfaceConvexCircle(x);
        if (kind < 1.5) return surfaceConvexSquircle(x);
        if (kind < 2.5) return surfaceConcave(x);
        return surfaceLip(x);
      }

      float surfaceDerivative(float x, float kind) {
        float eps = 0.001;
        float x1 = max(x - eps, 0.0);
        float x2 = min(x + eps, 1.0);
        float y1 = surfaceHeight(x1, kind);
        float y2 = surfaceHeight(x2, kind);
        return (y2 - y1) / max(x2 - x1, 0.000001);
      }

      float roundedRectSdf(float2 p, float2 halfSize, float radius) {
        float2 q = abs(p) - halfSize + float2(radius);
        return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
      }

      float2 shapeNormal(float2 p, float2 halfSize, float radius) {
        float eps = 1.0;
        float dx = roundedRectSdf(p + float2(eps, 0.0), halfSize, radius)
                 - roundedRectSdf(p - float2(eps, 0.0), halfSize, radius);
        float dy = roundedRectSdf(p + float2(0.0, eps), halfSize, radius)
                 - roundedRectSdf(p - float2(0.0, eps), halfSize, radius);
        float2 normal = float2(dx, dy);
        float normalLen = length(normal);
        if (normalLen < 0.0001) {
          float2 fallback = normalize(p + float2(0.001, 0.0));
          return float2(fallback.x, fallback.y);
        }
        return normal / normalLen;
      }

      float calculateDisplacement(float bezelT, float kind, float bezelPx, float thickness, float ior) {
        float eta = 1.0 / max(ior, 1.0001);
        float height = surfaceHeight(bezelT, kind);
        float derivative = surfaceDerivative(bezelT, kind);

        float magnitude = sqrt(derivative * derivative + 1.0);
        float normalX = -derivative / magnitude;
        float normalY = -1.0 / magnitude;

        float dotNI = normalY;
        float k = 1.0 - eta * eta * (1.0 - dotNI * dotNI);
        if (k < 0.0) return 0.0;

        float kSqrt = sqrt(k);
        float refractedX = -(eta * dotNI + kSqrt) * normalX;
        float refractedY = eta - (eta * dotNI + kSqrt) * normalY;
        float remainingHeight = height * bezelPx + thickness;

        if (abs(refractedY) < 0.001) return 0.0;
        return refractedX * (remainingHeight / refractedY);
      }

      float calculateSpecular(float distanceFromEdge, float rimWidth, float2 direction, float angle) {
        if (distanceFromEdge > rimWidth) return 0.0;
        float2 specularDir = float2(cos(angle), sin(angle));
        float2 normal2d = float2(direction.x, -direction.y);
        float dotProduct = abs(dot(normal2d, specularDir));
        float t = distanceFromEdge / max(rimWidth, 0.001);
        float rimCoefficient = sqrt(max(1.0 - (1.0 - t) * (1.0 - t), 0.0));
        float intensity = dotProduct * rimCoefficient;
        return intensity * intensity;
      }

      float2 applyMagnification(float2 coord, float2 localFromCenter, float2 offset, float magnify) {
        return coord + offset - localFromCenter * magnify;
      }

      half3 applyTint(half3 color) {
        return mix(color, half3(tintR, tintG, tintB), tintOpacity);
      }

      half4 main(float2 coord) {
        float2 center = resolution * 0.5;
        float2 local = coord - origin - center;
        float2 halfSize = resolution * 0.5;
        float radius = clamp(cornerRadius, 0.0, min(halfSize.x, halfSize.y));
        float sdf = roundedRectSdf(local, halfSize, radius);

        half4 baseSample = src.eval(coord);
        if (sdf > 0.0) {
          return baseSample;
        }

        float distanceFromEdge = -sdf;
        float bezelPixels = clamp(bezelWidth, 1.0, min(halfSize.x, halfSize.y));
        float2 sampleBase = applyMagnification(coord, local, sampleOffset, magnification);

        half3 color;
        if (distanceFromEdge >= bezelPixels) {
          color = src.eval(sampleBase).rgb;
        } else {
          float bezelT = distanceFromEdge / bezelPixels;
          float rawDisplacement = calculateDisplacement(
            bezelT,
            surfaceType,
            bezelPixels,
            glassThickness,
            refractiveIndex
          );
          float maxDisplacement = bezelPixels * max(maxDisplacementScale, 0.0);
          float displacement = min(rawDisplacement, maxDisplacement);
          float2 direction = shapeNormal(local, halfSize, radius);

          if (chromaticStrength > 0.0001 && chromaticBase > 0.0001) {
            float aberration = displacement * chromaticStrength * chromaticBase;
            float2 displacedR = sampleBase - direction * (displacement - aberration);
            float2 displacedG = sampleBase - direction * displacement;
            float2 displacedB = sampleBase - direction * (displacement + aberration);
            color = half3(
              src.eval(displacedR).r,
              src.eval(displacedG).g,
              src.eval(displacedB).b
            );
          } else {
            float2 displaced = sampleBase - direction * displacement;
            color = src.eval(displaced).rgb;
          }

          float specular = calculateSpecular(distanceFromEdge, max(specularWidth, 1.0), direction, specularAngle);
          color = mix(color, half3(1.0), specular * specularOpacity);
        }

        float edgeFactor = 1.0 - clamp(distanceFromEdge / max(bezelPixels, 1.0), 0.0, 1.0);
        color = applyTint(color);
        color *= 1.0 - edgeFactor * shadowOpacity;
        color = mix(color, half3(1.0), edgeFactor * 0.06 * max(shadowWidth, 0.0));

        return half4(color, baseSample.a);
      }
    )";
  }

  void setUniforms(SkRuntimeShaderBuilder& b, float w, float h, float) override
  {
    b.uniform("resolution") = SkV2{w, h};
    b.uniform("origin")     = SkV2{mCurrentRect.L, mCurrentRect.T};
    b.uniform("sampleOffset") = SkV2{
      getFloat("sampleOffsetX", 0.f),
      getFloat("sampleOffsetY", 0.f)
    };
    b.uniform("bezelWidth") = getFloat("bezelWidth", 18.f);
    b.uniform("glassThickness") = getFloat("glassThickness", 28.f);
    b.uniform("refractiveIndex") = getFloat("refractiveIndex", 1.24f);
    b.uniform("magnification") = getFloat("magnification", 0.18f);
    b.uniform("surfaceType") = getFloat("surfaceType", 1.f);
    b.uniform("cornerRadius") = getFloat("cornerRadius", std::min(w, h) * 0.5f);
    b.uniform("maxDisplacementScale") = getFloat("maxDisplacementScale", 1.65f);
    b.uniform("tintOpacity") = getFloat("tintOpacity", 0.08f);
    b.uniform("tintR") = getFloat("tintR", 1.f);
    b.uniform("tintG") = getFloat("tintG", 1.f);
    b.uniform("tintB") = getFloat("tintB", 1.f);
    b.uniform("specularOpacity") = getFloat("specularOpacity", 0.42f);
    b.uniform("specularAngle") = getFloat("specularAngle", -0.8f);
    b.uniform("specularWidth") = getFloat("specularWidth", 2.5f);
    b.uniform("shadowOpacity") = getFloat("shadowOpacity", 0.08f);
    b.uniform("shadowWidth") = getFloat("shadowWidth", 1.f);
    b.uniform("chromaticStrength") = getFloat("chromaticStrength", 0.06f);
    b.uniform("chromaticBase") = getFloat("chromaticBase", 0.75f);
  }
};

static bool _sk_liquid_glass_reg = glint_shader_registry::add(
  "liquid_glass", [] { return std::make_unique<glint_liquid_glass>(); });