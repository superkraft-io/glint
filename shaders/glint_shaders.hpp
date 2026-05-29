#pragma once
/**
 * glint_shaders.hpp -- umbrella include for the glint SkSL shader system.
 * Include AFTER glint.hpp or glint_window.hpp.
 *
 * Usage (backdrop shader):
 *   comp->style.backdropFilter = "shader(gl, liquid_glass)";
 *   comp->shaders["gl"]->params["glassThickness"] = 28.f;
 *
 * Usage (procedural overlay):
 *   comp->style.filter = "shader(aur, aurora)";
 *   comp->shaders["aur"]->params["speed"] = 1.5f;
 *
 * Interactive ripple:
 *   comp->style.backdropFilter = "shader(rip, ripple)";
 *   // click element -- fires automatically via onMouseDown forwarding
 *
 * Built-in names: "aurora", "vignette", "liquid_glass", "wave",
 *                 "chromatic_aberration", "ripple"
 */
#include "glint_shader_base.hpp"
#include "glint_shader_registry.hpp"
#include "aurora.hpp"
#include "vignette.hpp"
#include "liquid_glass.hpp"
#include "wave.hpp"
#include "chromatic_aberration.hpp"
#include "ripple.hpp"
#include "lens_refraction.hpp"

#ifdef SK_Glint_UserDefinedShaders
    #if __has_include(SK_Glint_UserDefinedShaders)
        #include SK_Glint_UserDefinedShaders
    #endif
#endif
