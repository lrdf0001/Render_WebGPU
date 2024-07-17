#pragma once

#include <webgpu/webgpu.hpp>

#include <GLFW/glfw3.h>
#include <filesystem>

#include <array>


namespace fs = std::filesystem;

using namespace wgpu;

struct MyUniforms {
	std::array<float, 4> color;  // or float color[4]
	float time;
	float _pad[3];
};

class Renderer {
public:

	Renderer();

	// Initialize everything and return true if it went all right
	bool Initialize();

	// Uninitialize everything that was initialized
	void Terminate();

	// Draw a frame and handle events
	void MainLoop();

	// Return true as long as the main loop should keep on running
	bool IsRunning();

private:
	TextureView GetNextSurfaceTextureView();

	// Substep of Initialize() that creates the render pipeline
	void InitializePipeline();
	RequiredLimits GetRequiredLimits(Adapter adapter) const;
	void InitializeBuffers();

	bool loadGeometry(const fs::path& path, std::vector<float>& pointData, std::vector<float>& colorData, std::vector<uint16_t>& indexData);
	ShaderModule loadShaderModule(const fs::path& path);

private:
	// We put here all the variables that are shared between init and main loop
	GLFWwindow *window;
	Device device;
	Queue queue;
	Surface surface;
	std::unique_ptr<ErrorCallback> uncapturedErrorCallbackHandle;
	TextureFormat surfaceFormat = TextureFormat::Undefined;
	RenderPipeline pipeline;
	
	Buffer pointBuffer;
    Buffer indexBuffer;
	Buffer colorBuffer;
	uint32_t indexCount;

	Buffer uniformBuffer;
	BindGroup bindGroup;

	MyUniforms uniforms;
};