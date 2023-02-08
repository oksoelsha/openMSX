#include "ImGuiLayer.hh"

#include "CommandController.hh"
#include "Debugger.hh"
#include "Display.hh"
#include "FloatSetting.hh"
#include "GLUtil.hh"
#include "MSXMotherBoard.hh"
#include "MSXCPUInterface.hh"
#include "Reactor.hh"

#include <imgui.h>
#include <imgui_impl_sdl.h>
#include <imgui_impl_opengl3.h>
#include <imgui_memory_editor.h>

#include <SDL.h>

namespace openmsx {

static void HelpMarker(const char* desc)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

template<typename Getter, typename Setter>
static bool Checkbox(const char* label, Getter getter, Setter setter)
{
	bool value = getter();
	bool changed = ImGui::Checkbox(label, &value);
	if (changed) setter(value);
	return changed;
}

// Should we put these helpers in the ImGui namespace?
static bool SliderFloat(const char* label, FloatSetting& setting, const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
	float value = setting.getFloat();
	float min = narrow_cast<float>(setting.getMinValue());
	float max = narrow_cast<float>(setting.getMaxValue());
	bool changed = ImGui::SliderFloat(label, &value, min, max, format, flags);
	if (changed) setting.setDouble(value); // TODO setFloat()
	return changed;
}

struct DebuggableEditor : public MemoryEditor
{
	DebuggableEditor() {
		Open = false;
		ReadFn = [](const ImU8* userdata, size_t offset) -> ImU8 {
			auto* debuggable = reinterpret_cast<Debuggable*>(const_cast<ImU8*>(userdata));
			return debuggable->read(narrow<unsigned>(offset));
		};
		WriteFn = [](ImU8* userdata, size_t offset, ImU8 data) -> void {
			auto* debuggable = reinterpret_cast<Debuggable*>(userdata);
			debuggable->write(narrow<unsigned>(offset), data);
		};
	}

	void DrawWindow(const char* title, Debuggable& debuggable, size_t base_display_addr = 0x0000) {
		MemoryEditor::DrawWindow(title, &debuggable, debuggable.getSize(), base_display_addr);
	}
};

ImGuiLayer::ImGuiLayer(Reactor& reactor_)
	: Layer(Layer::COVER_PARTIAL, Layer::Z_IMGUI)
	, reactor(reactor_)
{
}

ImGuiLayer::~ImGuiLayer()
{
}

void ImGuiLayer::paint(OutputSurface& /*surface*/)
{
	// Start the Dear ImGui frame
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

	auto& rendererSettings = reactor.getDisplay().getRenderSettings();
	auto& commandController = reactor.getCommandController();

	if (show_demo_window) {
		ImGui::ShowDemoWindow(&show_demo_window);
	}

	// Show the enabled 'debuggables'
	if (auto* motherBoard = reactor.getMotherBoard()) {
		auto& debugger = motherBoard->getDebugger();
		for (const auto& [name, editor] : debuggables) {
			if (editor->Open) {
				if (auto* debuggable = debugger.findDebuggable(name)) {
					editor->DrawWindow(name.c_str(), *debuggable);
				}
			}
		}
	}

	ImGui::Begin("OpenMSX ImGui integration proof of concept", nullptr, ImGuiWindowFlags_MenuBar);

	if (ImGui::BeginMenuBar()) {
		if (auto* motherBoard = reactor.getMotherBoard()) {
			auto& debugger = motherBoard->getDebugger();
			if (ImGui::BeginMenu("Debuggables")) {
				for (auto& name : view::keys(debugger.getDebuggables())) {
					auto [it, inserted] = debuggables.try_emplace(name);
					auto& editor = it->second;
					if (inserted) editor = std::make_unique<DebuggableEditor>();
					ImGui::MenuItem(name.c_str(), nullptr, &editor->Open);
				}
				ImGui::EndMenu();
			}
		}
		ImGui::EndMenuBar();
	}

	ImGui::Checkbox("ImGui Demo Window", &show_demo_window);
	HelpMarker("Show the ImGui demo window.\n"
	           "This is purely to demonstrate the ImGui capabilities.\n"
	           "There is no connection with any openMSX functionality.");

	if (ImGui::Button("Reset MSX")) {
		commandController.executeCommand("reset");
	}
	HelpMarker("Reset the emulated MSX machine.");

	SliderFloat("noise", rendererSettings.getNoiseSetting(), "%.1f");
	HelpMarker("Stupid example. But good enough to demonstrate that this stuff is very easy.");

	ImGui::End();

	if (first) {
		// on startup, focus main openMSX window instead of the GUI window
		first = false;
		ImGui::SetWindowFocus(nullptr);
	}


	// Rendering
	ImGuiIO& io = ImGui::GetIO();
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	// Update and Render additional Platform Windows
	// (Platform functions may change the current OpenGL context, so we
	// save/restore it to make it easier to paste this code elsewhere.
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
		SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
		SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
		SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
	}
}

} // namespace openmsx