#include "Renderer.h"

#include <glfw3webgpu.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#define GLM_FORCE_SILENT_WARNINGS
#include <glm/ext.hpp>
#include <glm/ext/matrix_transform.hpp> // glm::translate, glm::rotate, glm::scale
#include <glm/ext/matrix_clip_space.hpp> // glm::perspective

#define TINYOBJLOADER_IMPLEMENTATION // add this to exactly 1 of your C++ files
#include "tiny_obj_loader.h"

#include <iostream>
#include <cassert>
#include <vector>

#include <fstream>
#include <sstream>
#include <string>
#include <array>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif // __EMSCRIPTEN__

static mat4x4 T1S = mat4x4(1.0);

static uint32_t ceilToNextMultiple(uint32_t value, uint32_t step) {
	uint32_t divide_and_ceil = value / step + (value % step == 0 ? 0 : 1);
	return step * divide_and_ceil;
}


Renderer::Renderer(): device(nullptr), queue(nullptr), surface(nullptr), pipeline(nullptr), 
		pointBuffer(nullptr), colorBuffer(nullptr), normalBuffer(nullptr), indexBuffer(nullptr), uniformBuffer(nullptr),
		bindGroup(nullptr), depthTexture(nullptr), depthTextureView(nullptr)
{
	uniformStride = new uint32_t();
};


bool Renderer::Initialize() {
	// Open window
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	window = glfwCreateWindow(640, 480, "Learn WebGPU", nullptr, nullptr);
	
	Instance instance = wgpuCreateInstance(nullptr);
	
	// Get adapter
	std::cout << "Requesting adapter..." << std::endl;
	surface = glfwGetWGPUSurface(instance, window);
	RequestAdapterOptions adapterOpts = {};
	adapterOpts.compatibleSurface = surface;
	Adapter adapter = instance.requestAdapter(adapterOpts);
	std::cout << "Got adapter: " << adapter << std::endl;
	
	instance.release();
	
	std::cout << "Requesting device..." << std::endl;
	DeviceDescriptor deviceDesc = {};
	deviceDesc.label = "My Device";
	deviceDesc.requiredFeatureCount = 0;
	//deviceDesc.defaultQueue.nextInChain = nullptr;
	deviceDesc.defaultQueue.label = "The default queue";
	deviceDesc.deviceLostCallback = [](WGPUDeviceLostReason reason, char const* message, void* /* pUserData */) {
		std::cout << "Device lost: reason " << reason;
		if (message) std::cout << " (" << message << ")";
		std::cout << std::endl;
	};

	// Before adapter.requestDevice(deviceDesc)
	RequiredLimits requiredLimits = GetRequiredLimits(adapter);
	
	deviceDesc.requiredLimits = &requiredLimits;
	device = adapter.requestDevice(deviceDesc);

	std::cout << "Got device: " << device << std::endl;

	// Device error callback
	uncapturedErrorCallbackHandle = device.setUncapturedErrorCallback([](ErrorType type, char const* message) {
		std::cout << "Uncaptured device error: type " << type;
		if (message) std::cout << " (" << message << ")";
		std::cout << std::endl;
	});
	
	queue = device.getQueue();

	// Configure the surface
	SurfaceConfiguration config = {};
	
	// Configuration of the textures created for the underlying swap chain
	config.width = 640;
	config.height = 480;
	config.usage = TextureUsage::RenderAttachment;
	surfaceFormat = surface.getPreferredFormat(adapter);
	config.format = surfaceFormat;

	// And we do not need any particular view format:
	config.viewFormatCount = 0;
	config.viewFormats = nullptr;
	config.device = device;
	config.presentMode = PresentMode::Fifo;
	config.alphaMode = CompositeAlphaMode::Auto;

	surface.configure(config);

	// Release the adapter only after it has been fully utilized
	adapter.release();

	InitializePipeline();

	return true;
}


void Renderer::Terminate() {

	pointBuffer.release();
	indexBuffer.release();
	colorBuffer.release();

	depthTextureView.release();
	depthTexture.destroy();
	depthTexture.release();

	pipeline.release();
	surface.unconfigure();
	queue.release();
	surface.release();
	device.release();
	glfwDestroyWindow(window);
	glfwTerminate();
}


void Renderer::MainLoop() {
	glfwPollEvents();

	uniforms.time = static_cast<float>(glfwGetTime()); // glfwGetTime returns a double
	queue.writeBuffer(uniformBuffer, offsetof(MyUniforms, time), &uniforms.time, sizeof(MyUniforms::time));
	
	float angle1 = uniforms.time * 0.25;
	mat4x4 R1 = glm::rotate(mat4x4(1.0), angle1, glm::vec3(0.0, 0.0, 1.0));
	uniforms.modelMatrix = R1 * T1S;
	queue.writeBuffer(uniformBuffer, offsetof(MyUniforms, modelMatrix), &uniforms.modelMatrix, sizeof(MyUniforms::modelMatrix));

	// Get the next target texture view
	TextureView targetView = GetNextSurfaceTextureView();
	if (!targetView) return;

	// Create a command encoder for the draw call
	CommandEncoderDescriptor encoderDesc = {};
	encoderDesc.label = "My command encoder";
	CommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, &encoderDesc);

	// Create the render pass that clears the screen with our color
	RenderPassDescriptor renderPassDesc = {};

	// The attachment part of the render pass descriptor describes the target texture of the pass
	RenderPassColorAttachment renderPassColorAttachment = {};
	renderPassColorAttachment.view = targetView;
	renderPassColorAttachment.resolveTarget = nullptr;
	renderPassColorAttachment.loadOp = LoadOp::Clear;
	renderPassColorAttachment.storeOp = StoreOp::Store;
	renderPassColorAttachment.clearValue = WGPUColor{ 0.2, 0.2, 0.2, 1.0 };

	// Add depth/stencil attachment:
	RenderPassDepthStencilAttachment depthStencilAttachment;
	depthStencilAttachment.view = depthTextureView;	
	depthStencilAttachment.depthClearValue = 1.0f; // The initial value of the depth buffer, meaning "far"	
	depthStencilAttachment.depthLoadOp = LoadOp::Clear; // Operation settings comparable to the color attachment
	depthStencilAttachment.depthStoreOp = StoreOp::Store;	
	depthStencilAttachment.depthReadOnly = false; // we could turn off writing to the depth buffer globally here	
	depthStencilAttachment.stencilClearValue = 0; // Stencil setup, mandatory but unused
	depthStencilAttachment.stencilLoadOp = LoadOp::Undefined;
	depthStencilAttachment.stencilStoreOp = StoreOp::Undefined;
	depthStencilAttachment.stencilReadOnly = true;
	
	renderPassDesc.depthStencilAttachment = &depthStencilAttachment;

#ifndef WEBGPU_BACKEND_WGPU
	renderPassColorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif // NOT WEBGPU_BACKEND_WGPU

	renderPassDesc.colorAttachmentCount = 1;
	renderPassDesc.colorAttachments = &renderPassColorAttachment;
	
	renderPassDesc.timestampWrites = nullptr;

	RenderPassEncoder renderPass = encoder.beginRenderPass(renderPassDesc);

	// Select which render pipeline to use
	renderPass.setPipeline(pipeline);

	// Set vertex buffer while encoding the render pass
	renderPass.setIndexBuffer(indexBuffer, IndexFormat::Uint16, 0, indexBuffer.getSize());
	renderPass.setVertexBuffer(0, pointBuffer, 0, pointBuffer.getSize());
	renderPass.setVertexBuffer(1, normalBuffer, 0, normalBuffer.getSize());
	renderPass.setVertexBuffer(2, colorBuffer, 0, colorBuffer.getSize());	

	uint32_t dynamicOffset = 0;

	// Set binding group
	dynamicOffset =  0 * (*uniformStride);
	renderPass.setBindGroup(0, bindGroup, 1, &dynamicOffset);
	renderPass.drawIndexed(indexCount, 1, 0, 0, 0);

	// Set binding group with a different uniform offset
	/*
	dynamicOffset = 1 * (*uniformStride);
	renderPass.setBindGroup(0, bindGroup, 1, &dynamicOffset);
	renderPass.drawIndexed(indexCount, 1, 0, 0, 0);
	*/

	renderPass.end();
	renderPass.release();

	// Finally encode and submit the render pass
	CommandBufferDescriptor cmdBufferDescriptor = {};
	cmdBufferDescriptor.label = "Command buffer";
	CommandBuffer command = encoder.finish(cmdBufferDescriptor);
	encoder.release();

	//std::cout << "Submitting command..." << std::endl;
	queue.submit(1, &command);
	command.release();
	//std::cout << "Command submitted." << std::endl;

	// At the end of the frame
	targetView.release();
#ifndef __EMSCRIPTEN__
	surface.present();
#endif

#if defined(WEBGPU_BACKEND_DAWN)
	device.tick();
#elif defined(WEBGPU_BACKEND_WGPU)
	device.poll(false);
#endif
}


bool Renderer::IsRunning() {
	return !glfwWindowShouldClose(window);
}


TextureView Renderer::GetNextSurfaceTextureView() {
	// Get the surface texture
	SurfaceTexture surfaceTexture;
	surface.getCurrentTexture(&surfaceTexture);
	if (surfaceTexture.status != SurfaceGetCurrentTextureStatus::Success) {
		return nullptr;
	}
	Texture texture = surfaceTexture.texture;

	// Create a view for this surface texture
	TextureViewDescriptor viewDescriptor;
	viewDescriptor.label = "Surface texture view";
	viewDescriptor.format = texture.getFormat();
	viewDescriptor.dimension = TextureViewDimension::_2D;
	viewDescriptor.baseMipLevel = 0;
	viewDescriptor.mipLevelCount = 1;
	viewDescriptor.baseArrayLayer = 0;
	viewDescriptor.arrayLayerCount = 1;
	viewDescriptor.aspect = TextureAspect::All;
	TextureView targetView = texture.createView(viewDescriptor);

	return targetView;
}


void Renderer::InitializePipeline() {
	
	std::cout << "Creating shader module..." << std::endl;
	ShaderModule shaderModule = loadShaderModule("..\\resources\\shaders2.wgsl");
	std::cout << fs::current_path().string() << std::endl;
	std::cout << "Shader module: " << shaderModule << std::endl;

	// Create the render pipeline
	RenderPipelineDescriptor pipelineDesc;

	// Configure the vertex pipeline
	std::vector<VertexBufferLayout> vertexBufferLayouts(3);
	
	// Position Attribute
	VertexAttribute positionAttrib;
	positionAttrib.shaderLocation = 0; // @location(0)
	positionAttrib.format = VertexFormat::Float32x3; // size of position
	positionAttrib.offset = 0;

	vertexBufferLayouts[0].attributeCount = 1;
	vertexBufferLayouts[0].attributes = &positionAttrib;
	vertexBufferLayouts[0].arrayStride = 3 * sizeof(float); // stride = size of position
	vertexBufferLayouts[0].stepMode = VertexStepMode::Vertex;

	// Normal attribute
	VertexAttribute normalAttrib;
	normalAttrib.shaderLocation = 1; // @location(1)
	normalAttrib.format = VertexFormat::Float32x3; 
	normalAttrib.offset = 0;

	vertexBufferLayouts[1].attributeCount = 1;
	vertexBufferLayouts[1].attributes = &normalAttrib;
	vertexBufferLayouts[1].arrayStride = 3 * sizeof(float); 
	vertexBufferLayouts[1].stepMode = VertexStepMode::Vertex;
	
	// Color attribute
	VertexAttribute colorAttrib;
	colorAttrib.shaderLocation = 2; // @location(2)
	colorAttrib.format = VertexFormat::Float32x3; // size of color
	colorAttrib.offset = 0;

	vertexBufferLayouts[2].attributeCount = 1;
	vertexBufferLayouts[2].attributes = &colorAttrib;
	vertexBufferLayouts[2].arrayStride = 3 * sizeof(float); // stride = size of color
	vertexBufferLayouts[2].stepMode = VertexStepMode::Vertex;

	pipelineDesc.vertex.bufferCount = static_cast<uint32_t>(vertexBufferLayouts.size());
	pipelineDesc.vertex.buffers = vertexBufferLayouts.data();

	// NB: We define the 'shaderModule' in the second part of this chapter.
	// Here we tell that the programmable vertex shader stage is described
	// by the function called 'vs_main' in that module.
	pipelineDesc.vertex.module = shaderModule;
	pipelineDesc.vertex.entryPoint = "vs_main";
	pipelineDesc.vertex.constantCount = 0;
	pipelineDesc.vertex.constants = nullptr;

	// Each sequence of 3 vertices is considered as a triangle
	pipelineDesc.primitive.topology = PrimitiveTopology::TriangleList;
	
	// We'll see later how to specify the order in which vertices should be
	// connected. When not specified, vertices are considered sequentially.
	pipelineDesc.primitive.stripIndexFormat = IndexFormat::Undefined;
	
	// The face orientation is defined by assuming that when looking
	// from the front of the face, its corner vertices are enumerated
	// in the counter-clockwise (CCW) order.
	pipelineDesc.primitive.frontFace = FrontFace::CCW;
	
	// But the face orientation does not matter much because we do not
	// cull (i.e. "hide") the faces pointing away from us (which is often
	// used for optimization).
	pipelineDesc.primitive.cullMode = CullMode::None;
	

	// We tell that the programmable fragment shader stage is described
	// by the function called 'fs_main' in the shader module.
	FragmentState fragmentState;
	fragmentState.module = shaderModule;
	fragmentState.entryPoint = "fs_main";
	fragmentState.constantCount = 0;
	fragmentState.constants = nullptr;

	BlendState blendState;
	blendState.color.srcFactor = BlendFactor::SrcAlpha;
	blendState.color.dstFactor = BlendFactor::OneMinusSrcAlpha;
	blendState.color.operation = BlendOperation::Add;
	blendState.alpha.srcFactor = BlendFactor::Zero;
	blendState.alpha.dstFactor = BlendFactor::One;
	blendState.alpha.operation = BlendOperation::Add;
	
	ColorTargetState colorTarget;
	colorTarget.format = surfaceFormat;
	colorTarget.blend = &blendState;
	colorTarget.writeMask = ColorWriteMask::All; // We could write to only some of the color channels.
	
	// We have only one target because our render pass has only one output color
	// attachment.
	fragmentState.targetCount = 1;
	fragmentState.targets = &colorTarget;
	pipelineDesc.fragment = &fragmentState;

	// Depth buffer
	DepthStencilState depthStencilState = Default;
	pipelineDesc.depthStencil = &depthStencilState;

	depthStencilState.depthCompare = CompareFunction::Less;
	depthStencilState.depthWriteEnabled = true;

	TextureFormat depthTextureFormat = TextureFormat::Depth24Plus;
	depthStencilState.format = depthTextureFormat;

	depthStencilState.stencilReadMask = 0;
	depthStencilState.stencilWriteMask = 0;

	pipelineDesc.depthStencil = &depthStencilState;

	pipelineDesc.multisample.count = 1;
	pipelineDesc.multisample.mask = ~0u;
	pipelineDesc.multisample.alphaToCoverageEnabled = false;

	// Create binding layout (don't forget to = Default)
	BindGroupLayoutEntry bindingLayout = Default;
	bindingLayout.binding = 0;
	bindingLayout.visibility = ShaderStage::Vertex | ShaderStage::Fragment;
	bindingLayout.buffer.type = BufferBindingType::Uniform;
	bindingLayout.buffer.minBindingSize = sizeof(MyUniforms);
	bindingLayout.buffer.hasDynamicOffset = true;
	
	// Create a bind group layout
	BindGroupLayoutDescriptor bindGroupLayoutDesc;
	bindGroupLayoutDesc.entryCount = 1;
	bindGroupLayoutDesc.entries = &bindingLayout;
	BindGroupLayout bindGroupLayout = device.createBindGroupLayout(bindGroupLayoutDesc);

	// Create the pipeline layout
	PipelineLayoutDescriptor layoutDesc{};
	layoutDesc.bindGroupLayoutCount = 1;
	layoutDesc.bindGroupLayouts = (WGPUBindGroupLayout*)&bindGroupLayout;
	PipelineLayout layout = device.createPipelineLayout(layoutDesc);

	// Assign the PipelineLayout to the RenderPipelineDescriptor's layout field
	pipelineDesc.layout = layout;
	pipeline = device.createRenderPipeline(pipelineDesc);

	// Create the depth texture
	TextureDescriptor depthTextureDesc;
	depthTextureDesc.dimension = TextureDimension::_2D;
	depthTextureDesc.format = depthTextureFormat;
	depthTextureDesc.mipLevelCount = 1;
	depthTextureDesc.sampleCount = 1;
	depthTextureDesc.size = { 640, 480, 1 };
	depthTextureDesc.usage = TextureUsage::RenderAttachment;
	depthTextureDesc.viewFormatCount = 1;
	depthTextureDesc.viewFormats = (WGPUTextureFormat*)&depthTextureFormat;
	depthTexture = device.createTexture(depthTextureDesc);

	// Create the view of the depth texture 
	TextureViewDescriptor depthTextureViewDesc;
	depthTextureViewDesc.aspect = TextureAspect::DepthOnly;
	depthTextureViewDesc.baseArrayLayer = 0;
	depthTextureViewDesc.arrayLayerCount = 1;
	depthTextureViewDesc.baseMipLevel = 0;
	depthTextureViewDesc.mipLevelCount = 1;
	depthTextureViewDesc.dimension = TextureViewDimension::_2D;
	depthTextureViewDesc.format = depthTextureFormat;
	depthTextureView = depthTexture.createView(depthTextureViewDesc);

	InitializeBuffers();

	// Upload the initial value of the uniforms
	InitializeUniforms();
	queue.writeBuffer(uniformBuffer, 0, &uniforms, sizeof(MyUniforms));

	// Upload second value
	/*
	uniforms.time = -1.0f;
	uniforms.color = { 1.0f, 1.0f, 1.0f, 0.7f };
	queue.writeBuffer(uniformBuffer, *uniformStride, &uniforms, sizeof(MyUniforms));
	*/

	// Create a binding
	BindGroupEntry binding{};
	binding.binding = 0;
	binding.buffer = uniformBuffer;
	binding.offset = 0;
	binding.size = sizeof(MyUniforms);

	// A bind group contains one or multiple bindings
	BindGroupDescriptor bindGroupDesc{};
	bindGroupDesc.layout = bindGroupLayout;
	// There must be as many bindings as declared in the layout!
	bindGroupDesc.entryCount = bindGroupLayoutDesc.entryCount;
	bindGroupDesc.entries = &binding;
	bindGroup = device.createBindGroup(bindGroupDesc);	

	// We no longer need to access the shader module
	shaderModule.release();
}


RequiredLimits Renderer::GetRequiredLimits(Adapter adapter) const {
	// Get adapter supported limits, in case we need them
	SupportedLimits supportedLimits;
	adapter.getLimits(&supportedLimits);

	// Don't forget to = Default
	RequiredLimits requiredLimits = Default;

	// We use at most 1 vertex attribute for now
	requiredLimits.limits.maxVertexAttributes = 3;
	// We should also tell that we use 1 vertex buffers
	requiredLimits.limits.maxVertexBuffers = 3;
	// Maximum size of a buffer is 6 vertices of 2 float each
	requiredLimits.limits.maxBufferSize = 3 * 6 * sizeof(float);
	// Maximum stride between 2 consecutive vertices in the vertex buffer
	requiredLimits.limits.maxVertexBufferArrayStride = 3 * sizeof(float); // 3 * sizeof(float)

	requiredLimits.limits.maxTextureDimension1D = 480;
	requiredLimits.limits.maxTextureDimension2D = 640;
	requiredLimits.limits.maxTextureArrayLayers = 1;

	requiredLimits.limits.minUniformBufferOffsetAlignment = supportedLimits.limits.minUniformBufferOffsetAlignment;
	requiredLimits.limits.minStorageBufferOffsetAlignment = supportedLimits.limits.minStorageBufferOffsetAlignment;

	requiredLimits.limits.maxBindGroups = 1;
	requiredLimits.limits.maxUniformBuffersPerShaderStage = 1;
	requiredLimits.limits.maxUniformBufferBindingSize = 16 * 4;
	requiredLimits.limits.maxDynamicUniformBuffersPerPipelineLayout = 1;

	*uniformStride = ceilToNextMultiple(
		(uint32_t)sizeof(MyUniforms),
		(uint32_t)requiredLimits.limits.minUniformBufferOffsetAlignment
	);

	return requiredLimits;
}


void Renderer::InitializeBuffers() {

	std::vector<float> pointData;
	std::vector<uint16_t> indexData;
	std::vector<float> colorData;
	std::vector<float> normalData;

	if(!loadGeometry("..\\resources\\piramide2.txt", pointData, colorData, indexData, normalData)){
		std::cout<< "*** ERROR *** No se puede cargar el fichero"<<std::endl;
	}

	indexCount = static_cast<uint32_t>(indexData.size());
	
	// Create vertex buffer
	BufferDescriptor bufferDesc;
	bufferDesc.mappedAtCreation = false;

	bufferDesc.label = "Vertex Position";
	bufferDesc.size = pointData.size() * sizeof(float);
	bufferDesc.usage = BufferUsage::CopyDst | BufferUsage::Vertex;
	pointBuffer = device.createBuffer(bufferDesc);
	queue.writeBuffer(pointBuffer, 0, pointData.data(), bufferDesc.size);

	bufferDesc.label = "Vertex Color";
	bufferDesc.size = colorData.size() * sizeof(float);
	bufferDesc.usage = BufferUsage::CopyDst | BufferUsage::Vertex;
	colorBuffer = device.createBuffer(bufferDesc);
	queue.writeBuffer(colorBuffer, 0, colorData.data(), bufferDesc.size);

	bufferDesc.label = "Vertex Normal";
	bufferDesc.size = normalData.size() * sizeof(float);
	bufferDesc.usage = BufferUsage::CopyDst | BufferUsage::Vertex;
	normalBuffer = device.createBuffer(bufferDesc);
	queue.writeBuffer(normalBuffer, 0, normalData.data(), bufferDesc.size);

	bufferDesc.label = "Vertex Index";
	bufferDesc.size = indexData.size() * sizeof(uint16_t);
	bufferDesc.size = (bufferDesc.size + 3) & ~3;
	bufferDesc.usage = BufferUsage::CopyDst | BufferUsage::Index;
	indexBuffer  = device.createBuffer(bufferDesc);
	queue.writeBuffer(indexBuffer, 0, indexData.data(), bufferDesc.size);

	// Uniform buffer
	bufferDesc.size = sizeof(MyUniforms); //+ (*uniformStride)
	bufferDesc.usage = BufferUsage::CopyDst | BufferUsage::Uniform;
	bufferDesc.mappedAtCreation = false;
	uniformBuffer = device.createBuffer(bufferDesc);

	float currentTime = 1.0f;
	queue.writeBuffer(uniformBuffer, 0, &currentTime, sizeof(float));
}


void Renderer::InitializeUniforms() {
	uniforms.time = 1.0f;
	uniforms.color = { 0.0f, 1.0f, 0.4f, 1.0f };

	mat4x4 S = transpose(mat4x4(
		0.3, 0.0, 0.0, 0.0,
		0.0, 0.3, 0.0, 0.0,
		0.0, 0.0, 0.3, 0.0,
		0.0, 0.0, 0.0, 1.0
	));

	// Translate the object
	mat4x4 T1 = transpose(mat4x4(
		1.0, 0.0, 0.0, 0.5,
		0.0, 1.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0,
		0.0, 0.0, 0.0, 1.0
	));

	// Translate the view
	glm::vec3 focalPoint(0.0, 0.0, -2.0);
	mat4x4 T2 = transpose(mat4x4(
		1.0, 0.0, 0.0, -focalPoint.x,
		0.0, 1.0, 0.0, -focalPoint.y,
		0.0, 0.0, 1.0, -focalPoint.z,
		0.0, 0.0, 0.0, 1.0
	));

	// Rotate the object
	float angle1 = 2.0f; // arbitrary time
	float c1 = cos(angle1);
	float s1 = sin(angle1);
	mat4x4 R1 = transpose(mat4x4(
		c1, s1, 0.0, 0.0,
		-s1, c1, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0,
		0.0, 0.0, 0.0, 1.0
	));

	// Rotate the view point
	float angle2 = 3.0f * PI / 4.0f;
	float c2 = cos(angle2);
	float s2 = sin(angle2);
	mat4x4 R2 = transpose(mat4x4(
		1.0, 0.0, 0.0, 0.0,
		0.0, c2, s2, 0.0,
		0.0, -s2, c2, 0.0,
		0.0, 0.0, 0.0, 1.0
	));

	T1S = T1 * S;
	uniforms.modelMatrix = R1 * T1 * S;
	uniforms.viewMatrix = T2 * R2;

	float ratio = 640.0f / 480.0f;
	float focalLength = 2.0;
	float near = 0.01f;
	float far = 100.0f;
	float divider = 1 / (focalLength * (far - near));
	uniforms.projectionMatrix = transpose(mat4x4(
		1.0, 0.0, 0.0, 0.0,
		0.0, ratio, 0.0, 0.0,
		0.0, 0.0, far * divider, -far * near * divider,
		0.0, 0.0, 1.0 / focalLength, 0.0
	));
	
}


bool Renderer::loadGeometry(const fs::path& path, 
								std::vector<float>& pointData, 
								std::vector<float>& colorData, 
								std::vector<uint16_t>& indexData,
								std::vector<float> &normalData)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    pointData.clear();
	colorData.clear();
    indexData.clear();
	normalData.clear();

    enum class Section {
        None,
        Points,
		Colors,
        Indices,
		Normal,
    };
    Section currentSection = Section::None;

    float value;
    uint16_t index;
    std::string line;
    while (!file.eof()) {
        getline(file, line);
        
        // overcome the `CRLF` problem
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
        
        if (line == "[points]") {
            currentSection = Section::Points;
        }
		else if(line == "[colors]"){
			currentSection = Section::Colors;
		}
		else if (line == "[indices]") {
            currentSection = Section::Indices;
        }
		else if(line == "[normal]"){
			currentSection = Section::Normal;
		}
        else if (line[0] == '#' || line.empty()) {
            // Do nothing, this is a comment
        }
        else if (currentSection == Section::Points) {
            std::istringstream iss(line);
            // Get x, y
            for (int i = 0; i < 3; ++i) {
                iss >> value;
                pointData.push_back(value);
            }
        }
		else if (currentSection == Section::Colors) {
            std::istringstream iss(line);
            // Get r, g, b
            for (int i = 0; i < 3; ++i) {
                iss >> value;
                colorData.push_back(value);
            }
        }
        else if (currentSection == Section::Indices) {
            std::istringstream iss(line);
            // Get corners #0 #1 and #2
            for (int i = 0; i < 3; ++i) {
                iss >> index;
                indexData.push_back(index);
            }
        }
		else if (currentSection == Section::Normal) {
			
			std::istringstream iss(line);
			// Get corners #0 #1 and #2
			for (int i = 0; i < 3; ++i) {
				iss >> value;
				normalData.push_back(value);
				std::cout << value << " ";
			}
			std::cout << std::endl;
			
		}
    }

	/*
	std::cout << "Normales" << std::endl;
	for (int i = 0; i < indexData.size(); i += 3) {

		int i1 = indexData[i];
		int i2 = indexData[i + 1];
		int i3 = indexData[i + 2];

		glm::vec3 p1(pointData[i1 * 3], pointData[i1 * 3 + 1], pointData[i1 * 3 + 2]);
		glm::vec3 p2(pointData[i2 * 3], pointData[i2 * 3 + 1], pointData[i2 * 3 + 2]);
		glm::vec3 p3(pointData[i3 * 3], pointData[i3 * 3 + 1], pointData[i3 * 3 + 2]);

		glm::vec3 v1(p2.x - p1.x, p2.y - p1.y, p2.z - p1.z);
		glm::vec3 v2(p3.x - p1.x, p3.y - p1.y, p3.z - p1.z);

		glm::vec3 n(v1.y*v2.z - v1.z*v2.y, v1.z*v2.x - v1.x*v2.z, v1.x*v2.y - v1.y*v2.x);

		std::cout << n.x << " " << n.y << " " << n.z << std::endl;

		//float mod = sqrt(n.x*n.x + n.y*n.y+ n.z*n.z);

		for(int j=0; j<3; j++)
		{
			normalData.push_back(n.x); //  / mod
			normalData.push_back(n.y);
			normalData.push_back(n.z);
		}
	}
	*/
	

    return true;
}


ShaderModule Renderer::loadShaderModule(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
		std::cout<<"*** ERROR *** Invalid path: "<<path<<std::endl;
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    std::string shaderSource(size, ' ');
    file.seekg(0);
    file.read(shaderSource.data(), size);

    ShaderModuleWGSLDescriptor shaderCodeDesc{};
    shaderCodeDesc.chain.next = nullptr;
    shaderCodeDesc.chain.sType = SType::ShaderModuleWGSLDescriptor;
    shaderCodeDesc.code = shaderSource.c_str();
    ShaderModuleDescriptor shaderDesc{};
    shaderDesc.nextInChain = &shaderCodeDesc.chain;
    return device.createShaderModule(shaderDesc);
}


bool  Renderer::loadGeometryFromObj(const fs::path& path,
									std::vector<float>& pointData,
									std::vector<float>& colorData,
									std::vector<float>& normalData) 
{
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;

	std::string warn;
	std::string err;

	bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.string().c_str());

	if (!warn.empty()) {
		std::cout << warn << std::endl;
	}

	if (!err.empty()) {
		std::cerr << err << std::endl;
	}

	if (!ret) {
		return false;
	}

	// Fill in vertexData here
	const auto& shape = shapes[0]; // look at the first shape only

	pointData.clear();
	colorData.clear();
	normalData.clear();

	for (size_t i = 0; i < shape.mesh.indices.size(); ++i) {
		const tinyobj::index_t& idx = shape.mesh.indices[i];

		pointData.push_back(attrib.vertices[3 * idx.vertex_index + 0]);
		pointData.push_back(attrib.vertices[3 * idx.vertex_index + 1]);
		pointData.push_back(attrib.vertices[3 * idx.vertex_index + 2]);

		normalData.push_back(attrib.normals[3 * idx.normal_index + 0]);
		normalData.push_back(attrib.normals[3 * idx.normal_index + 1]);
		normalData.push_back(attrib.normals[3 * idx.normal_index + 2]);

		colorData.push_back(attrib.colors[3 * idx.vertex_index + 0]);
		colorData.push_back(attrib.colors[3 * idx.vertex_index + 1]);
		colorData.push_back(attrib.colors[3 * idx.vertex_index + 2]);
	}

	return true;
}