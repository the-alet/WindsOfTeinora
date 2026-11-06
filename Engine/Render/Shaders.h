#pragma once

#include "Rhi/Rhi.h"

namespace Teinora
{
	/** Returns the compiled form of a shader for the backend: DXIL or SPIR-V. */
	Rhi::ShaderBytes GetShader(const char* Name, Rhi::EBackend Backend);
}
