#ifndef XGEOM_SKIN_COMPILER_H
#define XGEOM_SKIN_COMPILER_H
#pragma once

#include "dependencies/xresource_pipeline_v2/source/xresource_pipeline.h"

namespace xgeom_skin_compiler
{
    enum class state : std::uint8_t
    { OK
    , FAILURE
    };

    struct instance : xresource_pipeline::compiler::base
    {
        static std::unique_ptr<instance> Create(void);
    };

    // -SELFTEST: the bone addressing on synthetic meshes (xgeom_skin_compiler.cpp); the number of failed checks
    int SelfTest(void);
}

#endif
