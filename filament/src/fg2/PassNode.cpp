/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fg2/FrameGraph.h"
#include "fg2/details/PassNode.h"
#include "fg2/details/ResourceNode.h"
#include "ResourceAllocator.h"

#include <string>

namespace filament::fg2 {

PassNode::PassNode(FrameGraph& fg) noexcept : DependencyGraph::Node(fg.getGraph()) {
}

PassNode::PassNode(PassNode&& rhs) noexcept = default;
PassNode::~PassNode() noexcept = default;

utils::CString PassNode::graphvizifyEdgeColor() const noexcept {
    return utils::CString{"red"};
}

// ------------------------------------------------------------------------------------------------

RenderPassNode::RenderPassNode(FrameGraph& fg, const char* name, PassExecutor* base) noexcept
        : PassNode(fg), mFrameGraph(fg), mName(name), mPassExecutor(base, fg.getArena()) {
}
RenderPassNode::RenderPassNode(RenderPassNode&& rhs) noexcept = default;
RenderPassNode::~RenderPassNode() noexcept = default;

void RenderPassNode::onCulled(DependencyGraph* graph) noexcept {
}

void RenderPassNode::execute(FrameGraphResources const& resources,
        backend::DriverApi& driver) noexcept {

    ResourceAllocatorInterface& resourceAllocator = mFrameGraph.getResourceAllocator();

    // create the render targets
    for (auto& rt : mRenderTargetData) {
        assert(any(rt.targetBufferFlags));

        backend::TargetBufferInfo info[6] = {};
        for (size_t i = 0; i < 6; i++) {
            if (rt.attachmentInfo[i].isValid()) {
                info[i].handle = resources.get(rt.attachmentInfo[i]).texture;
                // TODO: we need to get the sub-resource part of this
            }
        }

        // TODO: handle special case for imported render target

        // TODO: we need a name
        rt.backend.target = resourceAllocator.createRenderTarget(
                "name", rt.targetBufferFlags,
                rt.backend.params.viewport.width,
                rt.backend.params.viewport.height,
                rt.descriptor.samples,
                { info[0], info[1], info[2], info[3] },
                info[4], info[5]);
    }

    mPassExecutor->execute(resources, driver);

    // destroy the render targets
    for (auto& rt : mRenderTargetData) {
        resourceAllocator.destroyRenderTarget(rt.backend.target);
    }
}

RenderTarget RenderPassNode::declareRenderTarget(FrameGraph& fg, FrameGraph::Builder& builder,
        RenderTarget::Descriptor const& descriptor) noexcept {

    RenderTargetData data;
    data.descriptor = descriptor;
    RenderTarget::Attachments& attachments = data.descriptor.attachments;

    // retrieve the ResourceNode of the attachments coming to us -- this will be used later
    // to compute the discard flags.
    for (size_t i = 0; i < 4; i++) {
        if (descriptor.attachments.color[i].isValid()) {
            data.incoming[i] = fg.getResourceNode(attachments.color[i]);
            attachments.color[i] = builder.write(attachments.color[i],
                    Texture::Usage::COLOR_ATTACHMENT);
            data.outgoing[i] = fg.getResourceNode(attachments.color[i]);
            data.attachmentInfo[i] = attachments.color[i];
        }
    }
    if (descriptor.attachments.depth.isValid()) {
        data.incoming[4] = fg.getResourceNode(attachments.depth);
        attachments.depth = builder.write(attachments.depth,
                Texture::Usage::DEPTH_ATTACHMENT);
        data.outgoing[4] = fg.getResourceNode(attachments.depth);
        data.attachmentInfo[4] = attachments.depth;
    }
    if (descriptor.attachments.stencil.isValid()) {
        data.incoming[5] = fg.getResourceNode(attachments.stencil);
        attachments.stencil = builder.write(attachments.stencil,
                Texture::Usage::STENCIL_ATTACHMENT);
        data.outgoing[5] = fg.getResourceNode(attachments.stencil);
        data.attachmentInfo[5] = attachments.stencil;
    }

    for (size_t i = 0; i < 6; i++) {
        // if the outgoing node is the same than the incoming node, it means we in fact
        // didn't have a incoming node (the node was created but not used yet).
        if (data.outgoing[i] == data.incoming[i]) {
            data.incoming[i] = nullptr;
        }
    }

    uint32_t id = mRenderTargetData.size();
    mRenderTargetData.push_back(data);
    return { data.descriptor.attachments, id };
}

void RenderPassNode::resolve() noexcept {
    using namespace backend;

    const backend::TargetBufferFlags flags[6] = {
            TargetBufferFlags::COLOR0,
            TargetBufferFlags::COLOR1,
            TargetBufferFlags::COLOR2,
            TargetBufferFlags::COLOR3,
            TargetBufferFlags::DEPTH,
            TargetBufferFlags::STENCIL
    };

    for (auto& rt : mRenderTargetData) {
        /*
         * Compute discard flags
         */
        for (size_t i = 0; i < 6; i++) {
            // we use 'outgoing' has a proxy for 'do we have an attachment here?'
            if (rt.outgoing[i]) {
                rt.targetBufferFlags |= flags[i];

                // start by discarding all the attachments we have
                // (we could set to ALL, but this is cleaner)
                rt.backend.params.flags.discardStart |= flags[i];
                rt.backend.params.flags.discardEnd   |= flags[i];
                if (rt.outgoing[i]->hasActiveReaders()) {
                    rt.backend.params.flags.discardEnd &= ~flags[i];
                }
                if (rt.incoming[i] && rt.incoming[i]->hasWriter()) {
                    rt.backend.params.flags.discardStart &= ~flags[i];
                }
            }
            // additionally, clear implies discardStart
            rt.backend.params.flags.discardStart |= (rt.descriptor.clearFlags & rt.targetBufferFlags);
        }

        /*
         * Compute other parameters, such as viewport
         */
        // TODO: compute viewport
        // TODO: compute samples

        rt.backend.params.clearColor = rt.descriptor.clearColor;
        rt.backend.params.flags.clear = (rt.descriptor.clearFlags & rt.targetBufferFlags);
        rt.backend.params.viewport = rt.descriptor.viewport;
        rt.descriptor.samples = rt.descriptor.samples;
    }
}

RenderPassNode::RenderTargetData const& RenderPassNode::getRenderTargetData(
        uint32_t id) const noexcept {
    assert(id < mRenderTargetData.size());
    return mRenderTargetData[id];
}

utils::CString RenderPassNode::graphvizify() const noexcept {
    std::string s;

    uint32_t id = getId();
    const char* const nodeName = getName();
    uint32_t refCount = getRefCount();

    s.append("[label=\"");
    s.append(nodeName);
    s.append("\\nrefs: ");
    s.append(std::to_string(refCount));
    s.append(", id: ");
    s.append(std::to_string(id));

    for (auto const& rt :mRenderTargetData) {
        s.append("\\nS:");
        s.append(utils::to_string(rt.backend.params.flags.discardStart).c_str());
        s.append(", E:");
        s.append(utils::to_string(rt.backend.params.flags.discardEnd).c_str());
    }

    s.append("\", ");

    s.append("style=filled, fillcolor=");
    s.append(refCount ? "darkorange" : "darkorange4");
    s.append("]");

    return utils::CString{ s.c_str() };
}

// ------------------------------------------------------------------------------------------------

PresentPassNode::PresentPassNode(FrameGraph& fg) noexcept
        : PassNode(fg) {
}
PresentPassNode::PresentPassNode(PresentPassNode&& rhs) noexcept = default;
PresentPassNode::~PresentPassNode() noexcept = default;

char const* PresentPassNode::getName() const noexcept {
    return "Present";
}

void PresentPassNode::onCulled(DependencyGraph* graph) noexcept {
}

utils::CString PresentPassNode::graphvizify() const noexcept {
    std::string s;
    s.reserve(128);
    uint32_t id = getId();
    s.append("[label=\"Present , id: ");
    s.append(std::to_string(id));
    s.append("\", style=filled, fillcolor=red3]");
    s.shrink_to_fit();
    return utils::CString{ s.c_str() };
}

void PresentPassNode::execute(FrameGraphResources const&, backend::DriverApi&) noexcept {
}

void PresentPassNode::resolve() noexcept {
}

} // namespace filament::fg2
