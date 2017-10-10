//
//  BloomEffect.cpp
//  render-utils/src/
//
//  Created by Olivier Prat on 09/25/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "BloomEffect.h"

#include "gpu/Context.h"
#include "gpu/StandardShaderLib.h"

#include <render/BlurTask.h>
#include <render/ResampleTask.h>

#include "BloomThreshold_frag.h"
#include "BloomApply_frag.h"

#define BLOOM_BLUR_LEVEL_COUNT  3

BloomThreshold::BloomThreshold() {

}

void BloomThreshold::configure(const Config& config) {
    _threshold = config.threshold;
}

void BloomThreshold::run(const render::RenderContextPointer& renderContext, const Inputs& inputs, Outputs& outputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());

    RenderArgs* args = renderContext->args;

    const auto frameTransform = inputs.get0();
    const auto inputFrameBuffer = inputs.get1();

    assert(inputFrameBuffer->hasColor());

    auto inputBuffer = inputFrameBuffer->getRenderBuffer(0);
    auto sourceViewport = args->_viewport;
    auto bufferSize = glm::ivec2(inputBuffer->getDimensions());

    if (!_outputBuffer || _outputBuffer->getSize() != inputFrameBuffer->getSize()) {
        auto colorTexture = gpu::TexturePointer(gpu::Texture::createRenderBuffer(inputBuffer->getTexelFormat(), bufferSize.x, bufferSize.y,
                                                gpu::Texture::SINGLE_MIP, gpu::Sampler(gpu::Sampler::FILTER_MIN_MAG_LINEAR_MIP_POINT)));

        _outputBuffer = gpu::FramebufferPointer(gpu::Framebuffer::create("BloomThreshold"));
        _outputBuffer->setRenderBuffer(0, colorTexture);
    }

    static const int COLOR_MAP_SLOT = 0;
    static const int THRESHOLD_SLOT = 1;

    if (!_pipeline) {
        auto vs = gpu::StandardShaderLib::getDrawTransformUnitQuadVS();
        auto ps = gpu::Shader::createPixel(std::string(BloomThreshold_frag));
        gpu::ShaderPointer program = gpu::Shader::createProgram(vs, ps);

        gpu::Shader::BindingSet slotBindings;
        slotBindings.insert(gpu::Shader::Binding("colorMap", COLOR_MAP_SLOT));
        slotBindings.insert(gpu::Shader::Binding("threshold", THRESHOLD_SLOT));
        gpu::Shader::makeProgram(*program, slotBindings);

        gpu::StatePointer state = gpu::StatePointer(new gpu::State());
        _pipeline = gpu::Pipeline::create(program, state);
    }

    gpu::doInBatch(args->_context, [&](gpu::Batch& batch) {
        batch.enableStereo(false);

        batch.setViewportTransform(args->_viewport);
        batch.setProjectionTransform(glm::mat4());
        batch.resetViewTransform();
        batch.setModelTransform(gpu::Framebuffer::evalSubregionTexcoordTransform(bufferSize, args->_viewport));
        batch.setPipeline(_pipeline);

        batch.setFramebuffer(_outputBuffer);
        batch.setResourceTexture(COLOR_MAP_SLOT, inputBuffer);
        batch._glUniform1f(THRESHOLD_SLOT, _threshold);
        batch.draw(gpu::TRIANGLE_STRIP, 4);
    });

    outputs = _outputBuffer;
}

BloomApply::BloomApply() {

}

void BloomApply::configure(const Config& config) {
    _intensity = config.intensity;
}

void BloomApply::run(const render::RenderContextPointer& renderContext, const Inputs& inputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());
    RenderArgs* args = renderContext->args;

    static auto BLUR0_SLOT = 0;
    static auto BLUR1_SLOT = 1;
    static auto BLUR2_SLOT = 2;
    static auto INTENSITY_SLOT = 3;

    if (!_pipeline) {
        auto vs = gpu::StandardShaderLib::getDrawTransformUnitQuadVS();
        auto ps = gpu::Shader::createPixel(std::string(BloomApply_frag));
        gpu::ShaderPointer program = gpu::Shader::createProgram(vs, ps);

        gpu::Shader::BindingSet slotBindings;
        slotBindings.insert(gpu::Shader::Binding("blurMap0", BLUR0_SLOT));
        slotBindings.insert(gpu::Shader::Binding("blurMap1", BLUR1_SLOT));
        slotBindings.insert(gpu::Shader::Binding("blurMap2", BLUR2_SLOT));
        slotBindings.insert(gpu::Shader::Binding("intensity", INTENSITY_SLOT));
        gpu::Shader::makeProgram(*program, slotBindings);

        gpu::StatePointer state = gpu::StatePointer(new gpu::State());
        state->setBlendFunction(true, gpu::State::SRC_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::ONE,
                                gpu::State::SRC_ALPHA, gpu::State::BLEND_OP_ADD, gpu::State::ONE);
        _pipeline = gpu::Pipeline::create(program, state);
    }

    const auto frameBuffer = inputs.get0();
    const auto framebufferSize = frameBuffer->getSize();
    const auto blur0FB = inputs.get1();
    const auto blur1FB = inputs.get2();
    const auto blur2FB = inputs.get3();

    gpu::doInBatch(args->_context, [&](gpu::Batch& batch) {
        batch.enableStereo(false);

        batch.setFramebuffer(frameBuffer);

        batch.setViewportTransform(args->_viewport);
        batch.setProjectionTransform(glm::mat4());
        batch.resetViewTransform();
        batch.setPipeline(_pipeline);

        batch.setModelTransform(gpu::Framebuffer::evalSubregionTexcoordTransform(framebufferSize, args->_viewport));
        batch.setResourceTexture(BLUR0_SLOT, blur0FB->getRenderBuffer(0));
        batch.setResourceTexture(BLUR1_SLOT, blur1FB->getRenderBuffer(0));
        batch.setResourceTexture(BLUR2_SLOT, blur2FB->getRenderBuffer(0));
        batch._glUniform1f(INTENSITY_SLOT, _intensity);
        batch.draw(gpu::TRIANGLE_STRIP, 4);
    });
}

DebugBloom::DebugBloom() {
}

void DebugBloom::configure(const Config& config) {
    _mode = static_cast<DebugBloomConfig::Mode>(config.mode);
    assert(_mode < DebugBloomConfig::MODE_COUNT);
}

void DebugBloom::run(const render::RenderContextPointer& renderContext, const Inputs& inputs) {
    assert(renderContext->args);
    assert(renderContext->args->hasViewFrustum());
    RenderArgs* args = renderContext->args;

    const auto frameBuffer = inputs.get0();
    const auto framebufferSize = frameBuffer->getSize();
    const auto level0FB = inputs.get1();
    const auto level1FB = inputs.get2();
    const auto level2FB = inputs.get3();
    const gpu::TexturePointer levelTextures[BLOOM_BLUR_LEVEL_COUNT] = {
        level0FB->getRenderBuffer(0),
        level1FB->getRenderBuffer(0),
        level2FB->getRenderBuffer(0)
    };

    static auto TEXCOORD_RECT_SLOT = 1;

    if (!_pipeline) {
        auto vs = gpu::StandardShaderLib::getDrawTexcoordRectTransformUnitQuadVS();
        auto ps = gpu::StandardShaderLib::getDrawTextureOpaquePS();
        gpu::ShaderPointer program = gpu::Shader::createProgram(vs, ps);

        gpu::Shader::BindingSet slotBindings;
        slotBindings.insert(gpu::Shader::Binding(std::string("texcoordRect"), TEXCOORD_RECT_SLOT));
        gpu::Shader::makeProgram(*program, slotBindings);

        gpu::StatePointer state = gpu::StatePointer(new gpu::State());
        state->setDepthTest(gpu::State::DepthTest(false));
        _pipeline = gpu::Pipeline::create(program, state);
    }

    gpu::doInBatch(args->_context, [&](gpu::Batch& batch) {
        batch.enableStereo(false);

        batch.setFramebuffer(frameBuffer);

        batch.setViewportTransform(args->_viewport);
        batch.setProjectionTransform(glm::mat4());
        batch.resetViewTransform();
        batch.setPipeline(_pipeline);

        Transform modelTransform;
        if (_mode == DebugBloomConfig::MODE_ALL_LEVELS) {
            batch._glUniform4f(TEXCOORD_RECT_SLOT, 0.0f, 0.0f, 1.f, 1.f);

            modelTransform = gpu::Framebuffer::evalSubregionTexcoordTransform(framebufferSize, args->_viewport / 2);
            modelTransform.postTranslate(glm::vec3(-1.0f, 1.0f, 0.0f));
            batch.setModelTransform(modelTransform);
            batch.setResourceTexture(0, levelTextures[0]);
            batch.draw(gpu::TRIANGLE_STRIP, 4);

            modelTransform.postTranslate(glm::vec3(2.0f, 0.0f, 0.0f));
            batch.setModelTransform(modelTransform);
            batch.setResourceTexture(0, levelTextures[1]);
            batch.draw(gpu::TRIANGLE_STRIP, 4);

            modelTransform.postTranslate(glm::vec3(-2.0f, -2.0f, 0.0f));
            batch.setModelTransform(modelTransform);
            batch.setResourceTexture(0, levelTextures[2]);
            batch.draw(gpu::TRIANGLE_STRIP, 4);
        } else {
            auto viewport = args->_viewport;
            auto blurLevel = _mode - DebugBloomConfig::MODE_LEVEL0;

            viewport.z /= 2;

            batch._glUniform4f(TEXCOORD_RECT_SLOT, 0.5f, 0.0f, 0.5f, 1.f);

            modelTransform = gpu::Framebuffer::evalSubregionTexcoordTransform(framebufferSize, viewport);
            modelTransform.postTranslate(glm::vec3(-1.0f, 0.0f, 0.0f));
            batch.setModelTransform(modelTransform);
            batch.setResourceTexture(0, levelTextures[blurLevel]);
            batch.draw(gpu::TRIANGLE_STRIP, 4);
        }
    });
}

void BloomConfig::setIntensity(float value) {
    auto task = static_cast<render::Task::TaskConcept*>(_task);
    auto blurJobIt = task->editJob("BloomApply");
    assert(blurJobIt != task->_jobs.end());
    blurJobIt->getConfiguration()->setProperty("intensity", value);
}

float BloomConfig::getIntensity() const {
    auto task = static_cast<render::Task::TaskConcept*>(_task);
    auto blurJobIt = task->getJob("BloomApply");
    assert(blurJobIt != task->_jobs.end());
    return blurJobIt->getConfiguration()->property("intensity").toFloat();
}

void BloomConfig::setSize(float value) {
    std::string blurName{ "BloomBlurN" };
    auto sigma = value*3.0f;

    for (auto i = 0; i < BLOOM_BLUR_LEVEL_COUNT; i++) {
        blurName.back() = '0' + i;
        auto task = static_cast<render::Task::TaskConcept*>(_task);
        auto blurJobIt = task->editJob(blurName);
        assert(blurJobIt != task->_jobs.end());
        auto& gaussianBlur = blurJobIt->edit<render::BlurGaussian>();
        auto gaussianBlurParams = gaussianBlur.getParameters();
        gaussianBlurParams->setFilterGaussianTaps((BLUR_MAX_NUM_TAPS - 1) / 2, sigma);
        // Gaussian blur increases at each level to have a slower rolloff on the edge
        // of the response
        sigma *= 1.5f;
    }
}

Bloom::Bloom() {

}

void Bloom::configure(const Config& config) {
    std::string blurName{ "BloomBlurN" };

    for (auto i = 0; i < BLOOM_BLUR_LEVEL_COUNT; i++) {
        blurName.back() = '0' + i;
        auto blurConfig = config.getConfig<render::BlurGaussian>(blurName);
        blurConfig->setProperty("filterScale", 1.0f);
    }
}

void Bloom::build(JobModel& task, const render::Varying& inputs, render::Varying& outputs) {
    const auto bloomInputBuffer = task.addJob<BloomThreshold>("BloomThreshold", inputs);
    const auto bloomHalfInputBuffer = task.addJob<render::HalfDownsample>("BloomHalf", bloomInputBuffer);
    const auto bloomQuarterInputBuffer = task.addJob<render::HalfDownsample>("BloomQuarter", bloomHalfInputBuffer);

#if 1
    // Multi-scale blur
    const auto blurFB0 = task.addJob<render::BlurGaussian>("BloomBlur0", bloomQuarterInputBuffer);
    const auto halfBlurFB0 = task.addJob<render::HalfDownsample>("BloomHalfBlur0", blurFB0);
    const auto blurFB1 = task.addJob<render::BlurGaussian>("BloomBlur1", halfBlurFB0, true);
    const auto halfBlurFB1 = task.addJob<render::HalfDownsample>("BloomHalfBlur1", blurFB1);
    const auto blurFB2 = task.addJob<render::BlurGaussian>("BloomBlur2", halfBlurFB1, true);
#else
    // Multi-scale downsampling debug
    const auto blurFB0 = bloomQuarterInputBuffer;
    const auto blurFB1 = task.addJob<render::HalfDownsample>("BloomHalfBlur1", blurFB0);
    const auto blurFB2 = task.addJob<render::HalfDownsample>("BloomHalfBlur2", blurFB1);
    // This is only needed so as not to crash as we expect to have the three blur jobs
    task.addJob<render::BlurGaussian>("BloomBlur0", bloomHalfInputBuffer, true);
    task.addJob<render::BlurGaussian>("BloomBlur1", blurFB1, true);
    task.addJob<render::BlurGaussian>("BloomBlur2", blurFB2, true);
#endif

    const auto& input = inputs.get<Inputs>();
    const auto& frameBuffer = input[1];

    const auto applyInput = DebugBloom::Inputs(frameBuffer, blurFB0, blurFB1, blurFB2).asVarying();
    task.addJob<BloomApply>("BloomApply", applyInput);
    task.addJob<DebugBloom>("DebugBloom", applyInput);
}
