#include <mbgl/gl/drawable_gl.hpp>
#include <mbgl/gl/drawable_gl_impl.hpp>
#include <mbgl/gl/texture2d.hpp>
#include <mbgl/gl/upload_pass.hpp>
#include <mbgl/gl/vertex_array.hpp>
#include <mbgl/gl/vertex_attribute_gl.hpp>
#include <mbgl/gl/vertex_buffer_resource.hpp>
#include <mbgl/programs/segment.hpp>
#include <mbgl/shaders/gl/shader_program_gl.hpp>

namespace mbgl {
namespace gl {

DrawableGL::DrawableGL(std::string name_)
    : Drawable(std::move(name_)),
      impl(std::make_unique<Impl>()) {}

DrawableGL::~DrawableGL() {
    impl->indexBuffer = {0, nullptr};
    impl->attributeBuffer.reset();
}

void DrawableGL::draw(const PaintParameters& parameters) const {
    auto& context = static_cast<gl::Context&>(parameters.context);

    if (shader) {
        const auto& shaderGL = static_cast<const ShaderProgramGL&>(*shader);
        if (shaderGL.getGLProgramID() != context.program.getCurrentValue()) {
            context.program = shaderGL.getGLProgramID();
        }
    } else {
        context.program = value::Program::Default;
    }

    context.setDepthMode(parameters.depthModeForSublayer(getSubLayerIndex(), getDepthType()));

    // force disable depth test for debugging
    // setDepthMode({gfx::DepthFunctionType::Always, gfx::DepthMaskType::ReadOnly, {0,1}});

    if (tileID) {
        // Doesn't work until the clipping masks are generated
        // parameters.stencilModeForClipping(tileID->toUnwrapped());
        context.setStencilMode(gfx::StencilMode::disabled());
    } else {
        context.setStencilMode(gfx::StencilMode::disabled());
    }

    context.setColorMode(parameters.colorModeForRenderPass());
    context.setCullFaceMode(gfx::CullFaceMode::disabled());

    bindUniformBuffers();
    bindTextures();

    auto& glContext = static_cast<gl::Context&>(parameters.context);
    const auto saveVertexArray = glContext.bindVertexArray.getCurrentValue();

    for (const auto& seg : impl->segments) {
        const auto& glSeg = static_cast<DrawSegmentGL&>(*seg);
        const auto& mlSeg = glSeg.getSegment();
        if (glSeg.getVertexArray().isValid()) {
            glContext.bindVertexArray = glSeg.getVertexArray().getID();
        }
        glContext.draw(glSeg.getMode(), mlSeg.indexOffset, mlSeg.indexLength);
    }

    glContext.bindVertexArray = saveVertexArray;

    unbindTextures();
    unbindUniformBuffers();
}

void DrawableGL::setIndexData(std::vector<std::uint16_t> indexes, std::vector<UniqueDrawSegment> segments) {
    impl->indexes = std::move(indexes);
    impl->segments = std::move(segments);
}

const gfx::VertexAttributeArray& DrawableGL::getVertexAttributes() const {
    return impl->vertexAttributes;
}

gfx::VertexAttributeArray& DrawableGL::mutableVertexAttributes() {
    return impl->vertexAttributes;
}

void DrawableGL::setVertexAttributes(const gfx::VertexAttributeArray& value) {
    impl->vertexAttributes = static_cast<const VertexAttributeArrayGL&>(value);
}
void DrawableGL::setVertexAttributes(gfx::VertexAttributeArray&& value) {
    impl->vertexAttributes = std::move(static_cast<VertexAttributeArrayGL&&>(value));
}

const gfx::UniformBufferArray& DrawableGL::getUniformBuffers() const {
    return impl->uniformBuffers;
}

gfx::UniformBufferArray& DrawableGL::mutableUniformBuffers() {
    return impl->uniformBuffers;
}

void DrawableGL::resetColor(const Color& newColor) {
    if (const auto& colorAttr = impl->vertexAttributes.get("a_color")) {
        colorAttr->clear();
        colorAttr->set(0, Drawable::colorAttrRGBA(newColor));
    }
}

void DrawableGL::bindUniformBuffers() const {
    if (shader) {
        const auto& shaderGL = static_cast<const ShaderProgramGL&>(*shader);
        for (const auto& element : shaderGL.getUniformBlocks().getMap()) {
            const auto& uniformBuffer = getUniformBuffers().get(element.first);
            if (!uniformBuffer) {
                continue;
            }
            element.second->bindBuffer(*uniformBuffer);
        }
    }
}

void DrawableGL::unbindUniformBuffers() const {
    if (shader) {
        const auto& shaderGL = static_cast<const ShaderProgramGL&>(*shader);
        for (const auto& element : shaderGL.getUniformBlocks().getMap()) {
            element.second->unbindBuffer();
        }
    }
}

void DrawableGL::upload(gfx::Context& context, gfx::UploadPass& uploadPass) {
    if (!shader) {
        return;
    }

    const bool build = impl->vertexAttributes.isDirty() ||
                       std::any_of(impl->segments.begin(), impl->segments.end(), [](const auto& seg) {
                           return !static_cast<const DrawSegmentGL&>(*seg).getVertexArray().isValid();
                       });

    if (build) {
        auto& glContext = static_cast<gl::Context&>(context);
        constexpr auto usage = gfx::BufferUsageType::StaticDraw;

        const auto indexBytes = impl->indexes.size() * sizeof(decltype(impl->indexes)::value_type);
        auto indexBufferResource = uploadPass.createIndexBufferResource(impl->indexes.data(), indexBytes, usage);
        auto indexBuffer = gfx::IndexBuffer{impl->indexes.size(), std::move(indexBufferResource)};

        // Apply drawable values to shader defaults
        const auto& defaults = shader->getVertexAttributes();
        const auto& overrides = impl->vertexAttributes;
        const auto vertexCount = overrides.getMaxCount();

        std::unique_ptr<gfx::VertexBufferResource> vertexBuffer;
        auto bindings = uploadPass.buildAttributeBindings(vertexCount, defaults, overrides, usage, vertexBuffer);

        impl->attributeBuffer = std::move(vertexBuffer);
        impl->indexBuffer = std::move(indexBuffer);

        // Create a VAO for each group of vertexes described by a segment
        for (const auto& seg : impl->segments) {
            auto& glSeg = static_cast<DrawSegmentGL&>(*seg);
            const auto& mlSeg = glSeg.getSegment();

            for (auto& binding : bindings) {
                binding->vertexOffset = static_cast<uint32_t>(mlSeg.vertexOffset);
            }

            auto vertexArray = glContext.createVertexArray();

            vertexArray.bind(glContext, impl->indexBuffer, bindings);

            assert(vertexArray.isValid());

            glSeg.setVertexArray(std::move(vertexArray));
        };
    }
}

void DrawableGL::bindTextures() const {
    int32_t unit = 0;
    for (const auto& tex : textures) {
        std::static_pointer_cast<gl::Texture2D>(tex.texture)->bind(tex.location, unit++);
    }
}

void DrawableGL::unbindTextures() const {
    for (const auto& tex : textures) {
        std::static_pointer_cast<gl::Texture2D>(tex.texture)->unbind();
    }
}

} // namespace gl
} // namespace mbgl
