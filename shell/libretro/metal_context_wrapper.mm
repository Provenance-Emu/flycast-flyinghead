/*
    Copyright 2025 flyinghead

    This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifdef HAVE_METAL

#include "rend/metal/metal_context.h"

extern "C" {

MetalContext* createMetalContext()
{
    return new MetalContext();
}

bool initMetalContext(MetalContext* context)
{
    if (context == nullptr)
        return false;
    return context->init();
}

void termMetalContext(MetalContext* context)
{
    if (context != nullptr)
        context->term();
}

void deleteMetalContext(MetalContext* context)
{
    delete context;
}

void updateMetalLayerSize(MetalContext* context, unsigned int width, unsigned int height)
{
    if (context != nullptr)
        context->UpdateLayerSize(width, height);
}

bool getMetalFrameBuffer(MetalContext* context, std::vector<unsigned int>& data, int width, int height)
{
    if (context != nullptr)
        return context->GetFrameBuffer(data, width, height);
    return false;
}

} // extern "C"

#endif // HAVE_METAL
